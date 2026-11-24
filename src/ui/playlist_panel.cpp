// Panel PLAYLISTA: zakladki (wiele playlist), lista wpisow, sterowanie powtarzaniem/losowaniem,
// dodawanie, import/eksport M3U. Czyta PlayerView (PaylistModel tylko do odczytu), zamierzenia
// wysyla przez PlayerActions. Kolory wylacznie z palette().
#include "vgmplay_ui/player_screen.h"

#include "screen_util.h"
#include "vgmplay_ui/format.h"
#include "vgmplay_ui/strings.h"
#include "vgmplay_ui/theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace vgmplay::ui {
using namespace detail;

void PlayerScreen::toggleLanguage(const PlayerView& view, PlayerActions& actions)
{
    if (view.settings) {
        settings::Settings s = *view.settings;
        s.ui.language = s.ui.language == i18n::Language::Pl ? i18n::Language::En : i18n::Language::Pl;
        actions.applySettings(s);
    } else {
        setLang(lang() == Lang::En ? Lang::Pl : Lang::En);
    }
}

void PlayerScreen::toggleGd3(const PlayerView& view, PlayerActions& actions)
{
    if (view.settings) {
        settings::Settings s = *view.settings;
        s.ui.gd3Language = s.ui.gd3Language == format::Gd3Language::Jp ? format::Gd3Language::En
                                                                       : format::Gd3Language::Jp;
        actions.applySettings(s);
    } else {
        gd3Jp_ = !gd3Jp_;
    }
}

namespace {

const playlist::Playlist* activeList(const PlayerView& v, std::size_t* index = nullptr)
{
    if (!v.playlists || v.playlists->playlistCount() == 0) return nullptr;
    const std::size_t a = v.playlists->activeIndex();
    if (index) *index = a;
    return &v.playlists->playlist(a);
}

} // namespace

void PlayerScreen::applyPlaylist(const NavEvent& ev, const PlayerView& view, PlayerActions& actions)
{
    std::size_t ai = 0;
    const playlist::Playlist* pl = activeList(view, &ai);
    if (!pl) return;
    const long long n = static_cast<long long>(pl->entries.size());
    plSel_ = std::clamp<long long>(plSel_, 0, std::max<long long>(0, n - 1));
    const std::size_t sel = static_cast<std::size_t>(plSel_);
    const int page = std::max(1, plVisible_ - 1);
    switch (ev.action) {
    case Action::Up: plSel_ = std::max<long long>(0, plSel_ - 1); break;
    case Action::Down: plSel_ = std::min<long long>(std::max<long long>(0, n - 1), plSel_ + 1); break;
    case Action::PageUp: plSel_ = std::max<long long>(0, plSel_ - page); break;
    case Action::PageDown: plSel_ = std::min<long long>(std::max<long long>(0, n - 1), plSel_ + page); break;
    case Action::Left:
        if (view.playlists->playlistCount() > 1 && !ev.repeat)
            actions.selectPlaylist((ai + view.playlists->playlistCount() - 1) % view.playlists->playlistCount());
        break;
    case Action::Right:
        if (view.playlists->playlistCount() > 1 && !ev.repeat)
            actions.selectPlaylist((ai + 1) % view.playlists->playlistCount());
        break;
    case Action::Accept:
        if (ev.repeat) break;
        if (n == 0) actions.requestOpenFile();
        else actions.playEntry(sel);
        break;
    case Action::Delete: // klawiatura: od razu (wpis, nie plik); pad (X) pyta o potwierdzenie
        if (n > 0 && !ev.repeat) actions.removeEntry(sel);
        break;
    case Action::ToggleSolo: // pad X
        if (n > 0 && !ev.repeat) openDialog(DlgKind::DeleteEntry, sel);
        break;
    case Action::ResetMix: // pad Y
        if (n > 0 && !ev.repeat) openDialog(DlgKind::EntryMenu, sel);
        break;
    case Action::MoveUp:
        if (sel > 0 && n > 0) { actions.moveEntry(sel, sel - 1); --plSel_; }
        break;
    case Action::MoveDown:
        if (n > 0 && sel + 1 < static_cast<std::size_t>(n)) { actions.moveEntry(sel, sel + 1); ++plSel_; }
        break;
    default: break;
    }
}

void PlayerScreen::drawPlaylist(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x, float y,
                                float w, float h)
{
    const Palette& p = palette();
    if (!beginPanel("##playlist", x, y, w, h)) { ImGui::End(); return; }
    if (clickedInside()) nav.setFocus(Panel::Playlist);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool focused = nav.focus() == Panel::Playlist;
    const ImGuiIO& io = ImGui::GetIO();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 wp = ImGui::GetWindowPos();

    std::size_t ai = 0;
    const playlist::Playlist* pl = activeList(view, &ai);
    const std::size_t listCount = view.playlists ? view.playlists->playlistCount() : 0;
    const long long n = pl ? static_cast<long long>(pl->entries.size()) : 0;

    char title[160];
    if (pl) std::snprintf(title, sizeof title, "%s  %lld", tr(Str::PanelPlaylist), n);
    else std::snprintf(title, sizeof title, "%s", tr(Str::PanelPlaylist));
    const float bar = drawFrame(dl, title, focused);
    if (focused) {
        const char* hint = tr(Str::PlHint);
        const float hw = ImGui::CalcTextSize(hint).x;
        if (hw + ImGui::CalcTextSize(title).x + lh * 3.0f < w)
            dl->AddText(ImVec2(wp.x + w - hw - 8.0f * dpi, wp.y + 2.0f * dpi), col32(p.text), hint);
    }

    // ---- sterowanie (zawijane przyciski) ----
    const float btnH = lh + 4.0f * dpi;
    ButtonFlow flow;
    flow.dl = dl;
    flow.x0 = wp.x + 4.0f * dpi;
    flow.right = wp.x + w - 4.0f * dpi;
    flow.cx = flow.x0;
    flow.cy = wp.y + bar + 3.0f * dpi;
    flow.h = btnH;
    flow.dpi = dpi;
    const bool hasLists = pl != nullptr;
    if (flow.button("<", listCount > 1, false, tr(Str::PlPrevList))) {
        nav.setFocus(Panel::Playlist);
        actions.selectPlaylist((ai + listCount - 1) % listCount);
    }
    {
        std::string nm = hasLists ? sanitizeForDisplay(pl->name) : std::string("-");
        char pos[24];
        std::snprintf(pos, sizeof pos, "%zu/%zu ", hasLists ? ai + 1 : 0, listCount);
        flow.text(std::string(pos) + nm, w * 0.42f, col32(p.accent));
    }
    if (flow.button(">", listCount > 1, false, tr(Str::PlNextList))) {
        nav.setFocus(Panel::Playlist);
        actions.selectPlaylist((ai + 1) % listCount);
    }
    if (flow.button("+", hasLists, false, tr(Str::PlNew))) {
        nav.setFocus(Panel::Playlist);
        char nm[96];
        std::snprintf(nm, sizeof nm, "%s %zu", tr(Str::DefaultListName), listCount + 1);
        openDialog(DlgKind::NewList, 0, nm);
    }
    if (flow.button(tr(Str::PlRename), hasLists)) {
        nav.setFocus(Panel::Playlist);
        openDialog(DlgKind::RenameList, ai, pl->name);
    }
    if (flow.button(tr(Str::PlDelete), listCount > 1)) {
        nav.setFocus(Panel::Playlist);
        openDialog(DlgKind::DeleteList, ai);
    }
    flow.newline();
    if (flow.button(tr(Str::PlShuffle), n > 1)) { nav.setFocus(Panel::Playlist); actions.shufflePlaylist(); }
    {
        using playlist::RepeatMode;
        const Str rs = view.repeat == RepeatMode::Off ? Str::PlRepeatOff
                       : view.repeat == RepeatMode::One ? Str::PlRepeatOne : Str::PlRepeatAll;
        if (flow.button(tr(rs), true, view.repeat != RepeatMode::Off, tr(Str::PlRepeatTip))) {
            nav.setFocus(Panel::Playlist);
            actions.setRepeat(view.repeat == RepeatMode::Off ? RepeatMode::All
                              : view.repeat == RepeatMode::All ? RepeatMode::One : RepeatMode::Off);
        }
    }
    if (flow.button(tr(Str::PlAddFiles), hasLists, false, tr(Str::BtnOpen))) { nav.setFocus(Panel::Playlist); actions.requestOpenFile(); }
    if (flow.button(tr(Str::PlAddFolder), hasLists)) { nav.setFocus(Panel::Playlist); actions.requestAddFolder(); }
    if (flow.button(tr(Str::PlImport), hasLists)) { nav.setFocus(Panel::Playlist); actions.requestImportM3u(); }
    if (flow.button(tr(Str::PlExport), n > 0)) { nav.setFocus(Panel::Playlist); actions.requestExportM3u(); }
    if (view.scan.active) {
        flow.newline();
        char sb[128];
        std::snprintf(sb, sizeof sb, tr(Str::PlScanning), static_cast<unsigned long long>(view.scan.files),
                      static_cast<unsigned long long>(view.scan.dirs));
        flow.text(sb, w * 0.7f, col32(p.notice));
        if (flow.button(tr(Str::PlCancel), true)) actions.cancelScan();
    }

    // ---- lista ----
    const float listTop = flow.bottom() + 4.0f * dpi;
    const float listBottom = wp.y + h - 3.0f * dpi;
    const float rowH = lh + 2.0f * dpi;
    plVisible_ = std::max(1, static_cast<int>((listBottom - listTop) / rowH));
    const float lx0 = wp.x + 2.0f * dpi, lx1 = wp.x + w - 6.0f * dpi;

    if (!pl || n == 0) {
        const char* msg = view.playlistsLoading ? tr(Str::PlLoadingLists) : tr(Str::PlEmpty);
        clippedText(dl, wp.x + 6.0f * dpi, wp.x + w - 4.0f * dpi, listTop + dpi, col32(p.textDim), msg);
        if (!pl) { plSel_ = 0; }
        ImGui::End();
        return;
    }

    // Zmiana playlisty lub biezacego wpisu: selekcja podaza za kursorem odtwarzania.
    if (plLastList_ != ai) { plLastList_ = ai; plLastCursor_ = -2; plScroll_ = 0; plSel_ = std::max<long long>(0, pl->cursor); }
    if (pl->cursor != plLastCursor_) {
        plLastCursor_ = pl->cursor;
        if (pl->cursor >= 0) plSel_ = pl->cursor;
    }
    plSel_ = std::clamp<long long>(plSel_, 0, n - 1);
    const int maxScroll = static_cast<int>(std::max<long long>(0, n - plVisible_));
    if (plSel_ < plScroll_) plScroll_ = static_cast<int>(plSel_);
    if (plSel_ >= plScroll_ + plVisible_) plScroll_ = static_cast<int>(plSel_) - plVisible_ + 1;
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f) plScroll_ -= static_cast<int>(io.MouseWheel * 3.0f);
    plScroll_ = std::clamp(plScroll_, 0, maxScroll);

    const format::Gd3Language gl = jp(view) ? format::Gd3Language::Jp : format::Gd3Language::En;
    const float numW = ImGui::CalcTextSize(n >= 1000 ? "0000" : "000").x + 6.0f * dpi;
    const float lenW = ImGui::CalcTextSize("00:00").x + 8.0f * dpi;
    int hoverRow = -1;
    for (int r = 0; r < plVisible_; ++r) {
        const long long i = plScroll_ + r;
        if (i >= n) break;
        const float ry = listTop + static_cast<float>(r) * rowH;
        const ImVec2 a(lx0, ry), b(lx1, ry + rowH);
        const bool hover = ImGui::IsMouseHoveringRect(a, b);
        if (hover) hoverRow = static_cast<int>(i);
        const bool isSel = i == plSel_;
        const bool isCur = i == pl->cursor;
        if (isSel) dl->AddRectFilled(a, b, col32(focused ? p.cursorCell : p.gridBar));
        else if (hover) dl->AddRectFilled(a, b, col32(p.gridBeat));
        const ImU32 tc = col32(isSel && focused ? p.cursorText : (isCur ? p.playing : p.text));
        const playlist::Entry& e = pl->entries[static_cast<std::size_t>(i)];
        char num[16];
        std::snprintf(num, sizeof num, "%c%lld", isCur ? '>' : ' ', i + 1);
        clippedText(dl, a.x + 3.0f * dpi, a.x + 3.0f * dpi + numW, ry + dpi, tc, num);
        const std::uint64_t ms = entryDurationMs(e, view.meta);
        char len[16] = "--:--";
        if (ms > 0) std::snprintf(len, sizeof len, "%s", formatTime(static_cast<double>(ms) / 1000.0).c_str());
        const float lw = ImGui::CalcTextSize(len).x;
        dl->AddText(ImVec2(b.x - lw - 4.0f * dpi, ry + dpi), tc, len);
        const std::string t = entryTitle(e, view.meta, gl);
        clippedText(dl, a.x + 3.0f * dpi + numW, b.x - lenW, ry + dpi, tc, t.c_str());
    }
    if (maxScroll > 0) {
        const float th = listBottom - listTop;
        const float bh = std::max(8.0f * dpi, th * plVisible_ / static_cast<float>(n));
        const float by = listTop + (th - bh) * plScroll_ / static_cast<float>(maxScroll);
        dl->AddRectFilled(ImVec2(wp.x + w - 4.0f * dpi, by), ImVec2(wp.x + w - 2.0f * dpi, by + bh), col32(p.border));
    }

    // ---- mysz: wybor, dwuklik = odtworz, prawy = menu, przeciaganie = przenies ----
    if (hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        nav.setFocus(Panel::Playlist);
        plSel_ = hoverRow;
        dragFrom_ = hoverRow;
        dragging_ = false;
        dragStart_ = io.MousePos;
    }
    if (hoverRow >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        actions.playEntry(static_cast<std::size_t>(hoverRow));
        dragFrom_ = -1;
    }
    if (hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        nav.setFocus(Panel::Playlist);
        plSel_ = hoverRow;
        openDialog(DlgKind::EntryMenu, static_cast<std::size_t>(hoverRow));
        dragFrom_ = -1;
    }
    if (dragFrom_ >= 0) {
        if (!io.MouseDown[0]) {
            if (dragging_ && hoverRow >= 0 && hoverRow != dragFrom_) {
                actions.moveEntry(static_cast<std::size_t>(dragFrom_), static_cast<std::size_t>(hoverRow));
                plSel_ = hoverRow;
            }
            dragFrom_ = -1;
            dragging_ = false;
        } else {
            const float dy = io.MousePos.y - dragStart_.y;
            if (std::abs(dy) > 4.0f * dpi) dragging_ = true;
            if (dragging_) { // przeciaganie przy krawedzi przewija liste
                if (io.MousePos.y < listTop) plScroll_ = std::max(0, plScroll_ - 1);
                else if (io.MousePos.y > listBottom) plScroll_ = std::min(maxScroll, plScroll_ + 1);
                if (hoverRow >= 0) {
                    const float ly = listTop + static_cast<float>(hoverRow - plScroll_) * rowH;
                    const float lineY = hoverRow > dragFrom_ ? ly + rowH : ly;
                    dl->AddLine(ImVec2(lx0, lineY), ImVec2(lx1, lineY), col32(p.accent), 2.0f * dpi);
                }
            }
        }
    }
    ImGui::End();
}

} // namespace vgmplay::ui
