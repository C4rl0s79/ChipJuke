// Okna modalne: ustawienia (F10), pytania o potwierdzenie, nazwy playlist, menu wpisu, raporty.
// Nawigacja: NavInput (Gora/Dol/Lewo/Prawo/Accept/Back) - to samo na klawiaturze i padzie; mysz
// dodatkowo. Zmiany ustawien tylko przez PlayerActions::applySettings.
#include "vgmplay_ui/player_screen.h"

#include "screen_util.h"
#include "vgmplay_ui/format.h"
#include "vgmplay_ui/pattern_view.h"
#include "vgmplay_ui/strings.h"
#include "vgmplay_ui/theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vgmplay::ui {
using namespace detail;

namespace {

constexpr int kSettingsRows = 8;

std::string fmt1(const char* f, const char* a)
{
    char b[512];
    std::snprintf(b, sizeof b, f, a);
    return b;
}

} // namespace

void PlayerScreen::openDialog(DlgKind k, std::size_t index, std::string text)
{
    dlg_ = Dialog{};
    dlg_.kind = k;
    dlg_.index = index;
    dlg_.text = std::move(text);
    dlg_.focusText = k == DlgKind::NewList || k == DlgKind::RenameList;
    dlg_.sel = k == DlgKind::DeleteList || k == DlgKind::DeleteEntry ? 1 : 0;
}

void PlayerScreen::closeDialog() { dlg_ = Dialog{}; }

void PlayerScreen::confirmDialog(const PlayerView& view, PlayerActions& actions)
{
    const Dialog d = dlg_;
    closeDialog();
    switch (d.kind) {
    case DlgKind::NewList: actions.createPlaylist(d.text); break;
    case DlgKind::RenameList: actions.renamePlaylist(d.index, d.text); break;
    case DlgKind::DeleteList: actions.deletePlaylist(d.index); break;
    case DlgKind::DeleteEntry: actions.removeEntry(d.index); break;
    case DlgKind::Report: actions.dismissNotice(); break;
    case DlgKind::EntryMenu:
        switch (d.sel) {
        case 0: actions.playEntry(d.index); break;
        case 1: actions.removeEntry(d.index); break;
        case 2:
            if (d.index > 0) { actions.moveEntry(d.index, d.index - 1); plSel_ = static_cast<long long>(d.index) - 1; }
            break;
        case 3: {
            const std::size_t cnt = view.playlists && view.playlists->playlistCount()
                                        ? view.playlists->playlist(view.playlists->activeIndex()).entries.size() : 0;
            if (d.index + 1 < cnt) { actions.moveEntry(d.index, d.index + 1); plSel_ = static_cast<long long>(d.index) + 1; }
            break;
        }
        default: break;
        }
        break;
    case DlgKind::None: break;
    }
}

void PlayerScreen::changeSetting(int row, int dir, const PlayerView& view, PlayerActions& actions)
{
    if (!view.settings || dir == 0) return;
    settings::Settings s = *view.settings;
    switch (row) {
    case 1: s.playback.loopCount = static_cast<std::uint32_t>(
                std::clamp<long long>(static_cast<long long>(s.playback.loopCount) + dir, 0, 99)); break;
    case 2: s.playback.fadeMs = static_cast<std::uint32_t>(
                std::clamp<long long>(static_cast<long long>(s.playback.fadeMs) + 250LL * dir, 0, 30000)); break;
    case 3: s.playback.nonLoopEnd = s.playback.nonLoopEnd == NonLoopEnd::Stop ? NonLoopEnd::FadeOut : NonLoopEnd::Stop; break;
    case 4: s.playback.volume = std::clamp(s.playback.volume + 0.05f * static_cast<float>(dir), 0.0f, 1.0f); break;
    case 5: s.ui.language = s.ui.language == i18n::Language::Pl ? i18n::Language::En : i18n::Language::Pl; break;
    case 6: s.ui.gd3Language = s.ui.gd3Language == format::Gd3Language::Jp ? format::Gd3Language::En : format::Gd3Language::Jp; break;
    case 7: s.ui.rowTicks = stepRowTicks(nearestRowTicks(s.ui.rowTicks), -dir); break;   // Prawo = dluzszy wiersz
    default: return;
    }
    actions.applySettings(s);
}

void PlayerScreen::applyModal(const NavEvent& ev, const PlayerView& view, PlayerActions& actions)
{
    if (settingsOpen_) {
        switch (ev.action) {
        case Action::Back:
        case Action::ToggleSettings:
            if (!ev.repeat) settingsOpen_ = false;
            break;
        case Action::Up: setSel_ = std::max(0, setSel_ - 1); break;
        case Action::Down: setSel_ = std::min(kSettingsRows - 1, setSel_ + 1); break;
        case Action::Left: changeSetting(setSel_, -1, view, actions); break;
        case Action::Right: changeSetting(setSel_, +1, view, actions); break;
        case Action::Accept:
            if (!ev.repeat) changeSetting(setSel_, +1, view, actions);
            break;
        case Action::TogglePlay: if (!ev.repeat) actions.togglePlay(); break;
        default: break;
        }
        return;
    }
    if (ev.repeat && ev.action != Action::Up && ev.action != Action::Down) return;
    switch (ev.action) {
    case Action::Back:
        if (dlg_.kind == DlgKind::Report && !(view.notice && view.notice->needsAck)) actions.dismissNotice();
        closeDialog();   // potwierdzenie (needsAck) wymaga Accept
        break;
    case Action::Accept: confirmDialog(view, actions); break;
    case Action::Up:
        if (dlg_.kind == DlgKind::EntryMenu) dlg_.sel = std::max(0, dlg_.sel - 1);
        break;
    case Action::Down:
        if (dlg_.kind == DlgKind::EntryMenu) dlg_.sel = std::min(4, dlg_.sel + 1);
        break;
    default: break;
    }
}

void PlayerScreen::drawModal(const PlayerView& view, PlayerActions& actions, float W, float H)
{
    if (!modalOpen()) return;
    const Palette& p = palette();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::SetNextWindowFocus();
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
    if (!ImGui::Begin("##modal", nullptr, flags)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), col32(p.overlay));
    if (settingsOpen_) drawSettings(view, actions, W, H);
    else drawDialog(view, actions, W, H);
    ImGui::End();
}

void PlayerScreen::drawSettings(const PlayerView& view, PlayerActions& actions, float W, float H)
{
    const Palette& p = palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const ImGuiIO& io = ImGui::GetIO();
    const float rowH = lh * 1.6f;
    const float bw = std::min(W - 8.0f * dpi, lh * 46.0f);
    const float bh = lh * 2.0f + rowH * kSettingsRows + lh * 10.4f;
    const float x0 = (W - bw) * 0.5f;
    const float y0 = std::max(4.0f * dpi, (H - bh) * 0.5f);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), col32(p.dialog));
    // Pasek tytulu i obramowanie.
    const float bar = lh + 4.0f * dpi;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bar), col32(p.focusHeader));
    dl->AddText(ImVec2(x0 + 6.0f * dpi, y0 + 2.0f * dpi), col32(p.text), tr(Str::SetTitle));
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), col32(p.focusBorder), 0.0f, 0, 2.0f * dpi);
    if (!view.settings) {
        clippedText(dl, x0 + 8.0f * dpi, x0 + bw - 8.0f * dpi, y0 + bar + 8.0f * dpi, col32(p.textDim), "-");
        return;
    }
    const settings::Settings& s = *view.settings;
    const float labelX = x0 + 12.0f * dpi;
    const float valX0 = x0 + bw * 0.46f, valX1 = x0 + bw - 12.0f * dpi;

    struct Row { Str label; std::string value; bool adjustable; };
    char tmp[64];
    std::vector<Row> rows;
    rows.push_back({Str::SetAudio, tr(Str::SetAudioDefault), false});
    if (s.playback.loopCount == 0) rows.push_back({Str::SetLoops, tr(Str::SetLoopsInf), true});
    else { std::snprintf(tmp, sizeof tmp, "%u", s.playback.loopCount); rows.push_back({Str::SetLoops, tmp, true}); }
    std::snprintf(tmp, sizeof tmp, "%.2f s", static_cast<double>(s.playback.fadeMs) / 1000.0);
    rows.push_back({Str::SetFade, tmp, true});
    rows.push_back({Str::SetNonLoop, tr(s.playback.nonLoopEnd == NonLoopEnd::Stop ? Str::SetStop : Str::SetFadeOut), true});
    std::snprintf(tmp, sizeof tmp, "%d%%", static_cast<int>(s.playback.volume * 100.0f + 0.5f));
    rows.push_back({Str::SetVolume, tmp, true});
    rows.push_back({Str::SetLanguage, s.ui.language == i18n::Language::Pl ? "Polski" : "English", true});
    rows.push_back({Str::SetGd3, s.ui.gd3Language == format::Gd3Language::Jp ? "JP (日本語)" : "EN", true});
    rows.push_back({Str::SetRowLen, rowTicksLabel(s.ui.rowTicks), true});

    float ry = y0 + bar + lh * 0.6f;
    for (int i = 0; i < kSettingsRows; ++i) {
        const Row& r = rows[static_cast<std::size_t>(i)];
        const ImVec2 a(x0 + 3.0f * dpi, ry), b(x0 + bw - 3.0f * dpi, ry + rowH);
        const bool hover = ImGui::IsMouseHoveringRect(a, b);
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            setSel_ = i;
            if (r.adjustable) {
                const float mx = io.MousePos.x;
                if (mx >= valX0) changeSetting(i, mx < (valX0 + valX1) * 0.5f ? -1 : +1, view, actions);
            }
        }
        if (hover && r.adjustable && io.MouseWheel != 0.0f) changeSetting(i, io.MouseWheel > 0 ? +1 : -1, view, actions);
        const bool isSel = i == setSel_;
        if (isSel) dl->AddRectFilled(a, b, col32(p.cursorCell));
        const ImU32 tc = col32(isSel ? p.cursorText : p.text);
        const float ty = ry + (rowH - lh) * 0.5f;
        clippedText(dl, labelX, valX0 - 6.0f * dpi, ty, tc, tr(r.label));
        std::string v = r.adjustable ? "<  " + r.value + "  >" : r.value;
        clippedText(dl, valX0, valX1, ty, col32(isSel ? p.cursorText : (r.adjustable ? p.accent : p.textDim)), v.c_str());
        ry += rowH;
    }
    ry += lh * 0.4f;
    dl->AddText(ImVec2(labelX, ry), col32(p.textDim), tr(Str::SetControls));
    ry += lh * 1.3f;
    for (Str st : {Str::SetCtl1, Str::SetCtl2, Str::SetCtl3, Str::SetCtl4, Str::SetCtl6, Str::SetCtl5}) {
        clippedText(dl, labelX, x0 + bw - 8.0f * dpi, ry, col32(p.text), tr(st));
        ry += lh * 1.2f;
    }
    clippedText(dl, labelX, x0 + bw - 8.0f * dpi, y0 + bh - lh * 1.4f, col32(p.textDim), tr(Str::SetHint));
}

void PlayerScreen::drawDialog(const PlayerView& view, PlayerActions& actions, float W, float H)
{
    const Palette& p = palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const bool clickOk = dlg_.age > 0;   // klik, ktory otworzyl okno, nie moze go od razu potwierdzic
    ++dlg_.age;

    const bool report = dlg_.kind == DlgKind::Report;
    const float bw = std::min(W - 8.0f * dpi, lh * (report ? 56.0f : 40.0f));
    const float pad = 12.0f * dpi;
    const float textW = bw - 2.0f * pad;

    // Zawartosc: tytul, tekst (zawijany), pozycje/pole tekstowe, przyciski.
    std::string title, body;
    std::vector<std::string> items;
    std::vector<std::string> buttons;   // indeks przycisku == wynik
    switch (dlg_.kind) {
    case DlgKind::NewList: title = tr(Str::DlgNewList); break;
    case DlgKind::RenameList: title = tr(Str::DlgRenameList); break;
    case DlgKind::DeleteList: {
        title = tr(Str::PlDelete);
        const bool ok = view.playlists && dlg_.index < view.playlists->playlistCount();
        body = fmt1(tr(Str::DlgDeleteList), ok ? sanitizeForDisplay(view.playlists->playlist(dlg_.index).name).c_str() : "?");
        buttons = {tr(Str::DlgNo), tr(Str::DlgYes)};
        break;
    }
    case DlgKind::DeleteEntry: {
        title = tr(Str::PlDelete);
        std::string t = "?";
        if (view.playlists && view.playlists->playlistCount()) {
            const auto& l = view.playlists->playlist(view.playlists->activeIndex());
            if (dlg_.index < l.entries.size())
                t = entryTitle(l.entries[dlg_.index], view.meta,
                               jp(view) ? format::Gd3Language::Jp : format::Gd3Language::En);
        }
        body = fmt1(tr(Str::DlgDeleteEntry), t.c_str());
        buttons = {tr(Str::DlgNo), tr(Str::DlgYes)};
        break;
    }
    case DlgKind::EntryMenu:
        title = tr(Str::PanelPlaylist);
        items = {tr(Str::MenuPlay), tr(Str::MenuRemove), tr(Str::MenuMoveUp), tr(Str::MenuMoveDown), tr(Str::MenuCancel)};
        break;
    case DlgKind::Report:
        title = tr(Str::DlgInfo);
        if (view.notice) {
            body = sanitizeForDisplay(view.notice->text);
            for (std::size_t i = 0; i < view.notice->details.size() && i < 12; ++i)
                items.push_back(sanitizeForDisplay(view.notice->details[i]));
            if (view.notice->details.size() > 12) items.push_back("...");
        }
        buttons = {view.notice && view.notice->needsAck ? tr(Str::DlgAckSave) : tr(Str::DlgOk)};
        break;
    case DlgKind::None: return;
    }

    const bool textMode = dlg_.kind == DlgKind::NewList || dlg_.kind == DlgKind::RenameList;
    const float bodyH = body.empty() ? 0.0f
                                     : ImGui::CalcTextSize(body.c_str(), nullptr, false, textW).y + lh * 0.6f;
    const float itemH = lh * 1.5f;
    float contentH = bodyH + (textMode ? lh * 3.2f : 0.0f) + itemH * static_cast<float>(items.size());
    if (report) contentH = bodyH + lh * 1.3f * static_cast<float>(items.size());
    const float btnH = lh + 6.0f * dpi;
    const float bar = lh + 4.0f * dpi;
    const float bh = bar + pad + contentH + (buttons.empty() ? 0.0f : btnH + pad) + pad * 0.5f;
    const float x0 = (W - bw) * 0.5f, y0 = std::max(4.0f * dpi, (H - bh) * 0.5f);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), col32(p.dialog));
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bar), col32(p.focusHeader));
    dl->AddText(ImVec2(x0 + 6.0f * dpi, y0 + 2.0f * dpi), col32(p.text), title.c_str());
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), col32(p.focusBorder), 0.0f, 0, 2.0f * dpi);

    float cy = y0 + bar + pad;
    const float cx = x0 + pad;
    if (!body.empty()) {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(cx, cy), col32(report && view.notice && view.notice->error ? p.error : p.text),
                    body.c_str(), nullptr, textW);
        cy += bodyH;
    }

    if (textMode) {
        dl->AddText(ImVec2(cx, cy), col32(p.textDim), tr(Str::DlgName));
        cy += lh * 1.3f;
        char buf[260];
        std::snprintf(buf, sizeof buf, "%s", dlg_.text.c_str());
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::SetNextItemWidth(textW);
        if (dlg_.focusText) { ImGui::SetKeyboardFocusHere(); dlg_.focusText = false; }
        const bool entered = ImGui::InputText("##name", buf, 256, ImGuiInputTextFlags_EnterReturnsTrue);
        dlg_.text = buf;
        cy += lh * 1.9f;
        if (clickOk && entered) { confirmDialog(view, actions); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { closeDialog(); return; }
        clippedText(dl, cx, cx + textW, cy - lh * 0.3f, col32(p.textDim), "Enter / A  OK    Esc / B");
    }

    if (dlg_.kind == DlgKind::EntryMenu) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            const ImVec2 a(x0 + 4.0f * dpi, cy), b(x0 + bw - 4.0f * dpi, cy + itemH);
            const bool hover = ImGui::IsMouseHoveringRect(a, b);
            if (hover) dlg_.sel = static_cast<int>(i);
            const bool isSel = dlg_.sel == static_cast<int>(i);
            if (isSel) dl->AddRectFilled(a, b, col32(p.cursorCell));
            clippedText(dl, a.x + 8.0f * dpi, b.x - 4.0f * dpi, cy + (itemH - lh) * 0.5f,
                        col32(isSel ? p.cursorText : p.text), items[i].c_str());
            if (hover && clickOk && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { confirmDialog(view, actions); return; }
            cy += itemH;
        }
    }
    if (report) {
        for (const std::string& it : items) {
            clippedText(dl, cx, cx + textW, cy, col32(p.textDim), it.c_str());
            cy += lh * 1.3f;
        }
    }

    if (!buttons.empty()) {
        ButtonFlow flow;
        flow.dl = dl;
        flow.x0 = cx;
        flow.right = x0 + bw - pad;
        flow.cx = cx;
        flow.cy = y0 + bh - btnH - pad;
        flow.h = btnH;
        flow.dpi = dpi;
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            const bool hotSel = !report && static_cast<int>(i) == dlg_.sel;
            if (flow.button(buttons[i], true, hotSel) && clickOk) {
                if (report) { confirmDialog(view, actions); return; }
                if (i == 1) { confirmDialog(view, actions); return; }
                closeDialog();
                return;
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && textMode) closeDialog();
}

} // namespace vgmplay::ui
