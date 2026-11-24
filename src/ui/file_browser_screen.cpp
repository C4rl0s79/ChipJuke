// Wbudowana przegladarka plikow (E4): okno nakladkowe sterowane padem i klawiatura (kontekst routera
// ScreenId::FileBrowser). Logika listowania/wyboru: file_browser.cpp (bez ImGui); tu tylko rysowanie, mapowanie
// akcji na maszyne stanow i przekazanie WYBORU do kontrolera przez PlayerActions (addPaths / importM3uFile /
// exportM3uFile). Kolory wylacznie z palette().
#include "vgmplay_ui/player_screen.h"

#include "screen_util.h"
#include "vgmplay_ui/format.h"
#include "vgmplay_ui/strings.h"
#include "vgmplay_ui/theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace vgmplay::ui {
using namespace detail;

namespace {

std::string toUtf8(const std::filesystem::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string humanSize(std::uint64_t b)
{
    char t[32];
    if (b < 1024) std::snprintf(t, sizeof t, "%llu B", static_cast<unsigned long long>(b));
    else if (b < 1024ull * 1024) std::snprintf(t, sizeof t, "%.0f KB", static_cast<double>(b) / 1024.0);
    else std::snprintf(t, sizeof t, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
    return t;
}

// Koniec napisu zamiast poczatku, gdy sie nie miesci (sciezki): "...\dluga\sciezka".
std::string tailFit(const std::string& s, float maxW)
{
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::size_t i = 0;
    while (i < s.size()) {
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        const std::string cand = "..." + s.substr(i);
        if (ImGui::CalcTextSize(cand.c_str()).x <= maxW) return cand;
    }
    return "...";
}

Str titleOf(BrowserMode m)
{
    switch (m) {
    case BrowserMode::AddFiles: return Str::BrwTitleAdd;
    case BrowserMode::AddFolder: return Str::BrwTitleFolder;
    case BrowserMode::ImportM3u: return Str::BrwTitleImport;
    case BrowserMode::ExportM3u: return Str::BrwTitleExport;
    }
    return Str::BrwTitleAdd;
}

template <class... A>
std::string fmt(const char* f, A... a)
{
    char b[640];
    std::snprintf(b, sizeof b, f, a...);
    return b;
}

} // namespace

// ---------------------------------------------------------------- otwieranie i akcje

void PlayerScreen::openPicker(BrowserMode mode, const PlayerView& view, PlayerActions& actions, bool viaPad)
{
    if (viaPad) {   // pad: wbudowana przegladarka (okno systemowe nie jest dostepne z pada)
        openBrowser(mode, view);
        return;
    }
    switch (mode) {   // mysz / klawiatura: okno systemowe
    case BrowserMode::AddFiles: actions.requestOpenFile(); break;
    case BrowserMode::AddFolder: actions.requestAddFolder(); break;
    case BrowserMode::ImportM3u: actions.requestImportM3u(); break;
    case BrowserMode::ExportM3u: actions.requestExportM3u(); break;
    }
}

void PlayerScreen::openBrowser(BrowserMode mode, const PlayerView& view)
{
    std::string name;
    if (mode == BrowserMode::ExportM3u && view.playlists && view.playlists->playlistCount() > 0)
        name = view.playlists->playlist(view.playlists->activeIndex()).name;
    std::vector<std::string> recent;
    if (view.settings) recent = view.settings->library.recentFolders;
    browser_.open(mode, recent, view.allowNetworkPaths, name);
}

void PlayerScreen::finishBrowser(const BrowserOutcome& out, const PlayerView& view, PlayerActions& actions)
{
    using Kind = BrowserOutcome::Kind;
    if (out.kind == Kind::None) return;
    if (out.kind == Kind::Close) {
        browser_.close();
        return;
    }
    if (!out.paths.empty()) {
        if (out.kind == Kind::AddPaths) actions.addPaths(out.paths);
        else if (out.kind == Kind::Import) actions.importM3uFile(out.paths.front());
        else if (out.kind == Kind::Export) actions.exportM3uFile(out.paths.front());
    }
    if (view.settings && !out.folder.empty()) {   // zapamietanie folderu (na gorze listy dyskow przy kolejnym otwarciu)
        settings::Settings s = *view.settings;
        s.library.recentFolders = pushRecentFolder(s.library.recentFolders, toUtf8(out.folder));
        if (s.library.recentFolders != view.settings->library.recentFolders) actions.applySettings(s);
    }
    browser_.close();
}

void PlayerScreen::applyBrowser(const NavEvent& ev, const PlayerView& view, PlayerActions& actions)
{
    BrowserOutcome out;
    switch (ev.action) {
    case Action::Up: browser_.move(-1); break;
    case Action::Down: browser_.move(1); break;
    case Action::PageUp: browser_.page(-1); break;
    case Action::PageDown: browser_.page(1); break;
    case Action::Left:
        if (!ev.repeat && !browser_.atRoot()) out = browser_.back();
        break;
    case Action::Right:
        if (!ev.repeat) out = browser_.enterSelected();
        break;
    case Action::Confirm:
        if (!ev.repeat) out = browser_.activate();
        break;
    case Action::Back:
        if (!ev.repeat) out = browser_.back();
        break;
    case Action::ToggleTrackerView:   // Select / F4: zamknij od razu
        if (!ev.repeat) out.kind = BrowserOutcome::Kind::Close;
        break;
    case Action::ToggleSolo:   // X / S: dodaj biezacy folder, zapisz tutaj
        if (!ev.repeat) out = browser_.useHere();
        break;
    case Action::Menu:        // Y: zaznacz
    case Action::PlayPause:   // Start / spacja: zaznacz
        if (!ev.repeat) browser_.toggleMark();
        break;
    default: break;
    }
    finishBrowser(out, view, actions);
}

std::vector<PlayerScreen::Hint> PlayerScreen::browserHints() const
{
    std::vector<Hint> h;
    const std::vector<BrowserEntry>& e = browser_.entries();
    const int sel = browser_.selection();
    const bool onFile = sel >= 0 && static_cast<std::size_t>(sel) < e.size() && e[static_cast<std::size_t>(sel)].kind == EntryKind::File;
    h.push_back({Action::Confirm, onFile ? Str::HkSelect : Str::HkOpenDir});
    h.push_back({Action::Back, browser_.atRoot() ? Str::HkClose : Str::HkUp});
    switch (browser_.mode()) {
    case BrowserMode::AddFiles:
        h.push_back({Action::ToggleSolo, Str::HkAddFolder});
        h.push_back({Action::Menu, Str::HkMark});
        break;
    case BrowserMode::AddFolder:
        h.push_back({Action::ToggleSolo, Str::HkAddFolder});
        h.push_back({Action::Menu, Str::HkMark});
        break;
    case BrowserMode::ExportM3u: h.push_back({Action::ToggleSolo, Str::HkSaveHere}); break;
    case BrowserMode::ImportM3u: break;
    }
    h.push_back({Action::NextTab, Str::HkPage, false, Action::PrevTab});
    h.push_back({Action::ToggleTrackerView, Str::HkClose});
    return h;
}

// ---------------------------------------------------------------- rysowanie

void PlayerScreen::drawBrowser(NavInput& nav, const PlayerView& view, PlayerActions& actions, float W, float H)
{
    const Palette& p = palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const ImGuiIO& io = ImGui::GetIO();

    const float bw = std::min(W - 8.0f * dpi, lh * 56.0f);
    const float bh = std::min(H - 8.0f * dpi, lh * 36.0f);
    const float x0 = (W - bw) * 0.5f, y0 = (H - bh) * 0.5f;
    const float pad = 8.0f * dpi;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), col32(p.dialog));
    const float bar = lh + 4.0f * dpi;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bar), col32(p.focusHeader));
    dl->AddText(ImVec2(x0 + 6.0f * dpi, y0 + 2.0f * dpi), col32(p.text), tr(titleOf(browser_.mode())));
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), col32(p.focusBorder), 0.0f, 0, 2.0f * dpi);

    // Biezaca sciezka.
    const std::string where = browser_.atRoot() ? std::string(tr(Str::BrwRoot)) : sanitizeForDisplay(toUtf8(browser_.dir()));
    const float pathY = y0 + bar + 4.0f * dpi;
    dl->AddText(ImVec2(x0 + pad, pathY), col32(p.accent), tailFit(where, bw - 2.0f * pad).c_str());

    // Uklad: lista, stopka (status, przyciski, legenda).
    const float rowH = lh * 1.5f;
    const float legendH = lh * 1.7f;
    const float btnH = lh + 6.0f * dpi;
    const float statusH = lh * 1.3f;
    const float footerH = statusH + btnH + 4.0f * dpi + legendH + 2.0f * pad;
    const float listTop = pathY + lh * 1.4f;
    const float listBottom = y0 + bh - footerH;
    const int visible = std::max(1, static_cast<int>((listBottom - listTop) / rowH));
    browser_.setVisibleRows(visible);
    const std::vector<BrowserEntry>& entries = browser_.entries();
    const float lx0 = x0 + 3.0f * dpi, lx1 = x0 + bw - 3.0f * dpi - 6.0f * dpi;
    dl->AddRectFilled(ImVec2(lx0, listTop), ImVec2(x0 + bw - 3.0f * dpi, listBottom), col32(withAlpha(p.panel, 0.6f)));

    if (io.MouseWheel != 0.0f && ImGui::IsMouseHoveringRect(ImVec2(lx0, listTop), ImVec2(lx1, listBottom)))
        browser_.scrollBy(-static_cast<int>(io.MouseWheel * 3.0f));

    const bool marking = canMark(browser_.mode());
    int hoverRow = -1;
    dl->PushClipRect(ImVec2(lx0, listTop), ImVec2(lx1 + 6.0f * dpi, listBottom), true);
    for (int r = 0; r < visible; ++r) {
        const int i = browser_.scrollTop() + r;
        if (i >= static_cast<int>(entries.size())) break;
        const BrowserEntry& e = entries[static_cast<std::size_t>(i)];
        const float ry = listTop + static_cast<float>(r) * rowH;
        const ImVec2 a(lx0, ry), b(lx1, ry + rowH);
        const bool hover = ImGui::IsMouseHoveringRect(a, b);
        if (hover) hoverRow = i;
        const bool isSel = i == browser_.selection();
        if (isSel) dl->AddRectFilled(a, b, col32(p.cursorCell));
        else if (hover) dl->AddRectFilled(a, b, col32(p.gridBeat));
        const ImU32 tc = col32(isSel ? p.cursorText : (e.kind == EntryKind::File ? p.text : p.accent));
        const ImU32 dim = col32(isSel ? p.cursorText : p.textDim);

        float cx = a.x + 6.0f * dpi;
        const float cy = ry + rowH * 0.5f;
        const float s = lh * 0.55f;   // polowa rozmiaru ikony
        const bool canBeMarked = marking && (browser_.mode() == BrowserMode::AddFiles ? e.kind == EntryKind::File
                                                                                       : e.kind == EntryKind::Dir);
        if (canBeMarked) {   // pole zaznaczenia (Y)
            const ImVec2 qa(cx, cy - s * 0.8f), qb(cx + s * 1.6f, cy + s * 0.8f);
            dl->AddRect(qa, qb, tc, 0.0f, 0, dpi);
            if (browser_.isMarked(i)) dl->AddRectFilled(ImVec2(qa.x + 2.0f * dpi, qa.y + 2.0f * dpi), ImVec2(qb.x - 2.0f * dpi, qb.y - 2.0f * dpi), tc);
        }
        cx += s * 1.6f + 6.0f * dpi;
        switch (e.kind) {   // ikony z prostych ksztaltow (bez zaleznosci od glifow czcionki)
        case EntryKind::Dir:
        case EntryKind::Recent:
            dl->AddRectFilled(ImVec2(cx, cy - s * 0.7f), ImVec2(cx + s * 0.9f, cy - s * 0.4f), tc);
            dl->AddRectFilled(ImVec2(cx, cy - s * 0.45f), ImVec2(cx + s * 2.0f, cy + s * 0.8f), tc);
            break;
        case EntryKind::Drive:
            dl->AddRectFilled(ImVec2(cx, cy - s * 0.3f), ImVec2(cx + s * 2.0f, cy + s * 0.6f), tc);
            dl->AddRectFilled(ImVec2(cx + s * 1.4f, cy + s * 0.15f), ImVec2(cx + s * 1.8f, cy + s * 0.4f), col32(p.dialog));
            break;
        case EntryKind::Parent:
            dl->AddTriangleFilled(ImVec2(cx, cy + s * 0.6f), ImVec2(cx + s * 2.0f, cy + s * 0.6f), ImVec2(cx + s, cy - s * 0.7f), tc);
            break;
        case EntryKind::File:
            dl->AddRect(ImVec2(cx + s * 0.2f, cy - s * 0.8f), ImVec2(cx + s * 1.7f, cy + s * 0.8f), tc, 0.0f, 0, dpi);
            dl->AddLine(ImVec2(cx + s * 0.5f, cy - s * 0.2f), ImVec2(cx + s * 1.4f, cy - s * 0.2f), tc);
            dl->AddLine(ImVec2(cx + s * 0.5f, cy + s * 0.2f), ImVec2(cx + s * 1.4f, cy + s * 0.2f), tc);
            break;
        }
        cx += s * 2.0f + 8.0f * dpi;

        // Prawa strona: rozmiar / znaczniki (chmura, dowiazanie, rodzaj dysku).
        float rx = b.x - 6.0f * dpi;
        auto tag = [&](const std::string& t, ImU32 c) {
            const float tw = ImGui::CalcTextSize(t.c_str()).x;
            rx -= tw;
            dl->AddText(ImVec2(rx, ry + (rowH - lh) * 0.5f), c, t.c_str());
            rx -= 10.0f * dpi;
        };
        if (e.kind == EntryKind::File && !e.cloud) tag(humanSize(e.size), dim);
        if (e.cloud) {   // ikona chmury: dwa kola i podstawa + podpis
            tag(tr(Str::BrwCloud), col32(isSel ? p.cursorText : p.notice));
            const ImU32 cc = col32(isSel ? p.cursorText : p.notice);
            const float cs0 = lh * 0.28f;
            rx -= cs0 * 3.0f;
            dl->AddCircleFilled(ImVec2(rx + cs0 * 0.8f, cy), cs0, cc);
            dl->AddCircleFilled(ImVec2(rx + cs0 * 1.9f, cy - cs0 * 0.5f), cs0 * 1.1f, cc);
            dl->AddRectFilled(ImVec2(rx + cs0 * 0.8f, cy), ImVec2(rx + cs0 * 2.6f, cy + cs0), cc);
            rx -= 4.0f * dpi;
        }
        if (e.link) tag(tr(Str::BrwLink), dim);
        if (e.kind == EntryKind::Recent) tag(tr(Str::BrwRecent), dim);
        if (e.kind == EntryKind::Drive) {
            if (e.drive == DriveKind::Network) tag(tr(Str::BrwDriveNet), dim);
            else if (e.drive == DriveKind::Removable) tag(tr(Str::BrwDriveRem), dim);
            else if (e.drive == DriveKind::Optical) tag(tr(Str::BrwDriveOpt), dim);
        }
        clippedText(dl, cx, std::max(cx, rx), ry + (rowH - lh) * 0.5f, tc, sanitizeForDisplay(e.name).c_str());
    }
    dl->PopClipRect();

    // Pasek przewijania listy.
    const int n = static_cast<int>(entries.size());
    if (n > visible) {
        const Thumb t = scrollThumb(static_cast<float>(browser_.scrollTop()), static_cast<float>(n), static_cast<float>(visible),
                                    listBottom - listTop, 8.0f * dpi);
        dl->AddRectFilled(ImVec2(x0 + bw - 6.0f * dpi, listTop + t.pos), ImVec2(x0 + bw - 3.0f * dpi, listTop + t.pos + t.len), col32(p.border));
    }
    // Pusta / wczytywana lista.
    const bool onlyParent = n <= 1 && (n == 0 || entries[0].kind == EntryKind::Parent);
    if (browser_.loading())
        dl->AddText(ImVec2(lx0 + 10.0f * dpi, listTop + rowH * 1.2f), col32(p.textDim), tr(Str::BrwLoading));
    else if (onlyParent && browser_.status().error.empty())
        dl->AddText(ImVec2(lx0 + 10.0f * dpi, listTop + rowH * (n + 0.2f)), col32(p.textDim), tr(Str::BrwEmpty));

    // Mysz: klik zaznacza, dwuklik aktywuje.
    if (hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) browser_.select(hoverRow);
    if (hoverRow >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        browser_.select(hoverRow);
        finishBrowser(browser_.activate(), view, actions);
        if (!browser_.isOpen()) return;
    }

    // Status.
    const Listing& st = browser_.status();
    std::string msg;
    ImVec4 mc = p.textDim;
    if (browser_.networkBlocked()) { msg = tr(Str::BrwBlockedNet); mc = p.error; }
    else if (!st.error.empty()) { msg = fmt(tr(Str::BrwError), st.error.c_str()); mc = p.error; }
    else if (st.timedOut) { msg = tr(Str::BrwTimedOut); mc = p.notice; }
    else if (st.truncated) { msg = fmt(tr(Str::BrwTruncated), static_cast<unsigned>(kBrowserMaxEntries)); mc = p.notice; }
    else if (browser_.mode() == BrowserMode::ExportM3u && !browser_.exportTarget().empty())
        msg = fmt(tr(Str::BrwSaveAs), sanitizeForDisplay(toUtf8(browser_.exportTarget().filename())).c_str());
    const float statusY = listBottom + 4.0f * dpi;
    std::string marked;
    if (browser_.markedCount() > 0) marked = fmt(tr(Str::BrwMarked), static_cast<unsigned>(browser_.markedCount()));
    const float markedW = marked.empty() ? 0.0f : ImGui::CalcTextSize(marked.c_str()).x + 8.0f * dpi;
    clippedText(dl, x0 + pad, x0 + bw - pad - markedW, statusY, col32(mc), msg.c_str());
    if (!marked.empty()) dl->AddText(ImVec2(x0 + bw - pad - markedW + 8.0f * dpi, statusY), col32(p.accent), marked.c_str());

    // Przyciski (mysz; pad ma te same akcje na przyciskach).
    ButtonFlow flow;
    flow.dl = dl;
    flow.x0 = x0 + pad;
    flow.right = x0 + bw - pad;
    flow.cx = flow.x0;
    flow.cy = statusY + statusH;
    flow.h = btnH;
    flow.dpi = dpi;
    if (flow.button(tr(Str::BrwBtnUp), !browser_.atRoot())) finishBrowser(browser_.back(), view, actions);
    if (!browser_.isOpen()) return;
    const bool hasHere = !browser_.atRoot() && !browser_.loading();
    switch (browser_.mode()) {
    case BrowserMode::AddFiles:
        if (flow.button(tr(Str::BrwBtnAddHere), hasHere)) finishBrowser(browser_.useHere(), view, actions);
        break;
    case BrowserMode::AddFolder:
        if (flow.button(tr(browser_.markedCount() > 0 ? Str::BrwBtnAddMarked : Str::BrwBtnAddHere),
                        hasHere || browser_.markedCount() > 0))
            finishBrowser(browser_.useHere(), view, actions);
        break;
    case BrowserMode::ExportM3u:
        if (flow.button(tr(Str::BrwBtnSaveHere), hasHere && !browser_.exportTarget().empty()))
            finishBrowser(browser_.useHere(), view, actions);
        break;
    case BrowserMode::ImportM3u: break;
    }
    if (!browser_.isOpen()) return;
    if (flow.button(tr(Str::BrwBtnClose), true)) {
        browser_.close();
        return;
    }

    // Legenda przyciskow pada (zawsze: z pada nie ma innej wskazowki).
    const float legendY = y0 + bh - legendH - pad * 0.5f;
    dl->AddLine(ImVec2(x0 + pad, legendY - 2.0f * dpi), ImVec2(x0 + bw - pad, legendY - 2.0f * dpi), col32(p.border));
    drawHintRow(dl, browserHints(), std::string(), p.notice, x0, legendY, bw, legendH);
    (void)nav;
}

} // namespace vgmplay::ui
