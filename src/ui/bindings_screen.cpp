// E3: ekran przypisan (Ustawienia > Przypisania), nazwy zrodel/akcji do wyswietlenia oraz pasek podpowiedzi
// przyciskow pada. Logika edycji wiazan (konflikty, minimalny zestaw) jest w src/input (bindings_edit).
#include "vgmplay_ui/player_screen.h"

#include "screen_util.h"
#include "vgmplay_input/bindings_edit.hpp"
#include "vgmplay_ui/strings.h"
#include "vgmplay_ui/theme.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace vgmplay::ui {
using namespace detail;
using input::Action;
using input::BindingSource;
using input::SourceClass;
using input::SourceKind;

namespace {

struct TitleEntry { Action action; const char* en; const char* pl; };

constexpr TitleEntry kTitles[] = {
    {Action::Up, "Up", "Góra"}, {Action::Down, "Down", "Dół"}, {Action::Left, "Left", "Lewo"},
    {Action::Right, "Right", "Prawo"}, {Action::PageUp, "Page up", "Strona w górę"},
    {Action::PageDown, "Page down", "Strona w dół"},
    {Action::Confirm, "Confirm", "Potwierdź"}, {Action::Back, "Back / stop", "Wstecz / stop"},
    {Action::Menu, "Menu / settings", "Menu / ustawienia"},
    {Action::NextTab, "Next panel", "Następny panel"}, {Action::PrevTab, "Previous panel", "Poprzedni panel"},
    {Action::PlayPause, "Play / pause", "Odtwarzaj / pauza"}, {Action::Stop, "Stop", "Stop"},
    {Action::PrevTrack, "Previous track", "Poprzedni utwór"}, {Action::NextTrack, "Next track", "Następny utwór"},
    {Action::SeekBack, "Seek back", "Przewiń wstecz"}, {Action::SeekForward, "Seek forward", "Przewiń do przodu"},
    {Action::VolumeUp, "Volume up", "Głośniej"}, {Action::VolumeDown, "Volume down", "Ciszej"},
    {Action::ToggleLoop, "Repeat mode", "Tryb powtarzania"}, {Action::Shuffle, "Shuffle", "Losuj"},
    {Action::ToggleMute, "Mute channel", "Wycisz kanał"}, {Action::ToggleSolo, "Solo channel / remove entry", "Solo kanału / usuń wpis"},
    {Action::UnmuteAll, "Unmute all", "Włącz wszystkie"}, {Action::ClearSolo, "Clear solo", "Wyczyść solo"},
    {Action::ResetMix, "Reset mix", "Reset miksu"},
    {Action::ToggleTrackerView, "Scope bar height", "Wysokość paska oscyloskopów"},
    {Action::PatternFaster, "Shorter pattern row", "Krótszy wiersz wzorca"},
    {Action::PatternSlower, "Longer pattern row", "Dłuższy wiersz wzorca"},
    {Action::Delete, "Remove entry", "Usuń wpis"},
    {Action::MoveUp, "Move entry up", "Przenieś wpis wyżej"}, {Action::MoveDown, "Move entry down", "Przenieś wpis niżej"},
    {Action::NewPlaylist, "New playlist", "Nowa playlista"}, {Action::RenamePlaylist, "Rename playlist", "Zmień nazwę playlisty"},
    {Action::DeletePlaylist, "Delete playlist", "Usuń playlistę"},
    {Action::ToggleSettings, "Settings", "Ustawienia"},
    {Action::ToggleLang, "Language", "Język"}, {Action::ToggleGd3Lang, "Tag language", "Język tagów"},
};

} // namespace

const char* detail::actionTitle(input::Action a)
{
    for (const TitleEntry& t : kTitles)
        if (t.action == a) return lang() == Lang::Pl ? t.pl : t.en;
    return "?";
}

std::string detail::sourceLabel(const input::BindingSource& s)
{
    char b[48];
    switch (s.kind) {
    case SourceKind::Keyboard: {
        const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(s.code));
        return n && *n ? n : "?";
    }
    case SourceKind::GamepadButton:
        switch (s.code) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "A";
        case SDL_GAMEPAD_BUTTON_EAST: return "B";
        case SDL_GAMEPAD_BUTTON_WEST: return "X";
        case SDL_GAMEPAD_BUTTON_NORTH: return "Y";
        case SDL_GAMEPAD_BUTTON_BACK: return "Select";
        case SDL_GAMEPAD_BUTTON_START: return "Start";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L3";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R3";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad Up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad Down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad Left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad Right";
        default: std::snprintf(b, sizeof b, "Btn %d", s.code); return b;
        }
    case SourceKind::GamepadAxisPositive:
    case SourceKind::GamepadAxisNegative: {
        const bool pos = s.kind == SourceKind::GamepadAxisPositive;
        switch (s.code) {
        case SDL_GAMEPAD_AXIS_LEFTX: return pos ? "LS Right" : "LS Left";
        case SDL_GAMEPAD_AXIS_LEFTY: return pos ? "LS Down" : "LS Up";
        case SDL_GAMEPAD_AXIS_RIGHTX: return pos ? "RS Right" : "RS Left";
        case SDL_GAMEPAD_AXIS_RIGHTY: return pos ? "RS Down" : "RS Up";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LT";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT";
        default: std::snprintf(b, sizeof b, "Axis %d%c", s.code, pos ? '+' : '-'); return b;
        }
    }
    }
    return "?";
}

std::string detail::classLabel(std::span<const input::Binding> b, input::Action a, input::SourceClass cls)
{
    std::string r;
    for (const BindingSource& s : input::sourcesOf(b, a, cls)) {
        if (!r.empty()) r += ", ";
        r += sourceLabel(s);
    }
    return r.empty() ? "-" : r;
}

// ------------------------------------------------------------------ ekran przypisan

void PlayerScreen::openBindings(const PlayerView& view)
{
    bind_ = input::effectiveBindings(view.settings ? std::span<const input::Binding>(view.settings->input.bindings)
                                                   : std::span<const input::Binding>());
    bindRow_ = 0;
    bindCol_ = 0;
    bindScroll_ = 0;
    bindCapturing_ = false;
    bindPending_.reset();
    bindMsg_.clear();
    bindingsOpen_ = true;
}

void PlayerScreen::commitBindings(const PlayerView& view, PlayerActions& actions)
{
    if (!view.settings) return;
    settings::Settings s = *view.settings;
    s.input.bindings = bind_ == input::defaultBindings() ? std::vector<input::Binding>{} : bind_;   // pusty = domyslne
    actions.applySettings(s);
}

void PlayerScreen::applyBindings(const NavEvent& ev, const PlayerView& view, PlayerActions& actions)
{
    if (bindCapturing_) return;   // router nie dostaje zdarzen w trakcie przechwytywania
    const std::span<const input::Action> acts = input::rebindableActions();
    const int n = static_cast<int>(acts.size());
    if (bindPending_) {
        if (ev.repeat) return;
        if (ev.action == Action::Confirm) {
            const PendingBind p = *bindPending_;
            bindPending_.reset();
            const input::AssignResult r = input::assign(bind_, p.action, p.src);
            bindMsg_ = r.ok ? std::string() : tr(Str::BindCritical);
            bindMsgFrames_ = 240;
            if (r.ok) commitBindings(view, actions);
        } else if (ev.action == Action::Back) {
            bindPending_.reset();
        }
        return;
    }
    switch (ev.action) {
    case Action::Back:
    case Action::ToggleSettings:
        if (!ev.repeat) bindingsOpen_ = false;
        break;
    case Action::Up: bindRow_ = std::max(0, bindRow_ - 1); break;
    case Action::Down: bindRow_ = std::min(n, bindRow_ + 1); break;
    case Action::PageUp: bindRow_ = std::max(0, bindRow_ - 8); break;
    case Action::PageDown: bindRow_ = std::min(n, bindRow_ + 8); break;
    case Action::Left: bindCol_ = 0; break;
    case Action::Right: bindCol_ = 1; break;
    case Action::Confirm:
        if (ev.repeat) break;
        if (bindRow_ >= n) {
            bind_ = input::defaultBindings();
            bindMsg_.clear();
            commitBindings(view, actions);
        } else if (nav_) {
            bindCapturing_ = true;
            nav_->beginCapture(bindCol_ == 0 ? CaptureKind::Keyboard : CaptureKind::Gamepad, SDL_GetTicks());
            bindMsg_.clear();
        }
        break;
    case Action::Delete:
    case Action::ToggleSolo:   // X
        if (ev.repeat || bindRow_ >= n) break;
        if (input::clearClass(bind_, acts[static_cast<std::size_t>(bindRow_)],
                              bindCol_ == 0 ? SourceClass::Keyboard : SourceClass::Gamepad)) {
            bindMsg_.clear();
            commitBindings(view, actions);
        } else {
            bindMsg_ = tr(Str::BindCritical);
            bindMsgFrames_ = 240;
        }
        break;
    default: break;
    }
}

void PlayerScreen::pollCapture(NavInput& nav, const PlayerView& view, PlayerActions& actions)
{
    if (!bindCapturing_) return;
    const CaptureState st = nav.captureState();
    if (st == CaptureState::Waiting) return;
    const BindingSource src = nav.captureResult();
    nav.endCapture();
    bindCapturing_ = false;
    const std::span<const input::Action> acts = input::rebindableActions();
    if (st == CaptureState::Cancelled || st == CaptureState::TimedOut) {
        bindMsg_ = tr(st == CaptureState::Cancelled ? Str::BindCancelled : Str::BindTimeout);
        bindMsgFrames_ = 120;
        return;
    }
    if (st != CaptureState::Done || bindRow_ < 0 || bindRow_ >= static_cast<int>(acts.size())) return;
    const Action a = acts[static_cast<std::size_t>(bindRow_)];
    if (const std::optional<Action> c = input::findConflict(bind_, src, a)) {
        bindPending_ = PendingBind{a, src, *c};   // ostrzezenie; Confirm = zamiana
        return;
    }
    const input::AssignResult r = input::assign(bind_, a, src);
    bindMsg_ = r.ok ? std::string() : tr(Str::BindCritical);
    bindMsgFrames_ = 240;
    if (r.ok) commitBindings(view, actions);
}

void PlayerScreen::drawBindings(NavInput& nav, const PlayerView&, PlayerActions&, float W, float H)
{
    const Palette& p = palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const ImGuiIO& io = ImGui::GetIO();
    const std::span<const input::Action> acts = input::rebindableActions();
    const int n = static_cast<int>(acts.size());
    const float rowH = lh * 1.45f;
    const float bw = std::min(W - 8.0f * dpi, lh * 56.0f);
    const float bh = std::min(H - 8.0f * dpi, lh * 36.0f);
    const float x0 = (W - bw) * 0.5f;
    const float y0 = std::max(4.0f * dpi, (H - bh) * 0.5f);
    const float bar = lh + 4.0f * dpi;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), col32(p.dialog));
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bar), col32(p.focusHeader));
    dl->AddText(ImVec2(x0 + 6.0f * dpi, y0 + 2.0f * dpi), col32(p.text), tr(Str::BindTitle));
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), col32(p.focusBorder), 0.0f, 0, 2.0f * dpi);

    const float c0 = x0 + 10.0f * dpi, c1 = x0 + bw * 0.38f, c2 = x0 + bw * 0.69f, cEnd = x0 + bw - 8.0f * dpi;
    float y = y0 + bar + lh * 0.4f;
    dl->AddText(ImVec2(c1, y), col32(p.textDim), tr(Str::BindKeyboard));
    dl->AddText(ImVec2(c2, y), col32(p.textDim), tr(Str::BindPad));
    y += lh * 1.4f;

    const float footer = lh * 3.6f;
    const int visible = std::max(1, static_cast<int>((y0 + bh - footer - y) / rowH));
    const int total = n + 1;   // + "Przywroc domyslne"
    if (bindRow_ < bindScroll_) bindScroll_ = bindRow_;
    if (bindRow_ >= bindScroll_ + visible) bindScroll_ = bindRow_ - visible + 1;
    bindScroll_ = std::clamp(bindScroll_, 0, std::max(0, total - visible));

    const bool capturing = bindCapturing_ && nav.captureState() == CaptureState::Waiting;
    for (int r = 0; r < visible; ++r) {
        const int i = bindScroll_ + r;
        if (i >= total) break;
        const float ry = y + static_cast<float>(r) * rowH;
        const ImVec2 a(x0 + 3.0f * dpi, ry), b(x0 + bw - 3.0f * dpi, ry + rowH);
        const bool isSel = i == bindRow_;
        if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !capturing && !bindPending_) {
            bindRow_ = i;
            bindCol_ = io.MousePos.x >= c2 - 4.0f * dpi ? 1 : 0;
        }
        const float ty = ry + (rowH - lh) * 0.5f;
        if (i == n) {
            if (isSel) dl->AddRectFilled(a, b, col32(p.cursorCell));
            clippedText(dl, c0, cEnd, ty, col32(isSel ? p.cursorText : p.accent), tr(Str::BindRestore));
            continue;
        }
        const Action act = acts[static_cast<std::size_t>(i)];
        if (isSel) dl->AddRectFilled(a, b, col32(withAlpha(p.cursorCell, 0.45f)));
        clippedText(dl, c0, c1 - 6.0f * dpi, ty, col32(isSel ? p.cursorText : p.text), actionTitle(act));
        for (int col = 0; col < 2; ++col) {
            const float cx0 = col == 0 ? c1 : c2, cx1 = col == 0 ? c2 - 6.0f * dpi : cEnd;
            const bool cell = isSel && bindCol_ == col;
            if (cell) {
                dl->AddRectFilled(ImVec2(cx0 - 4.0f * dpi, ry + dpi), ImVec2(cx1, ry + rowH - dpi), col32(p.cursorCell));
                drawFocusRing(dl, ImVec2(cx0 - 4.0f * dpi, ry + dpi), ImVec2(cx1, ry + rowH - dpi), dpi);
            }
            std::string txt = cell && capturing ? std::string("...") : classLabel(bind_, act, col == 0 ? SourceClass::Keyboard : SourceClass::Gamepad);
            clippedText(dl, cx0, cx1, ty, col32(cell ? p.cursorText : p.accent), txt.c_str());
        }
    }

    float fy = y0 + bh - footer + lh * 0.2f;
    std::string msg;
    ImVec4 mc = p.notice;
    if (capturing) {
        msg = tr(nav.captureKind() == CaptureKind::Keyboard ? Str::BindPressKey : Str::BindPress);
        char t[24];
        std::snprintf(t, sizeof t, "  %llu s", static_cast<unsigned long long>(nav.captureRemainingMs(SDL_GetTicks()) / 1000 + 1));
        msg += t;
        mc = p.accent;
    } else if (bindPending_) {
        char t[400];
        std::snprintf(t, sizeof t, tr(Str::BindConflict), sourceLabel(bindPending_->src).c_str(), actionTitle(bindPending_->with));
        msg = t;
        mc = p.error;
    } else if (!bindMsg_.empty()) {
        msg = bindMsg_;
        if (bindMsgFrames_ > 0) --bindMsgFrames_;
        else bindMsg_.clear();
    }
    clippedText(dl, c0, x0 + bw - 6.0f * dpi, fy, col32(mc), msg.c_str());
    fy += lh * 1.4f;
    clippedText(dl, c0, x0 + bw - 6.0f * dpi, fy, col32(p.textDim), tr(Str::BindHint));
}

// ------------------------------------------------------------------ pasek podpowiedzi

namespace {

struct Hint {
    Action action;     // akcja, ktorej przycisk pada pokazujemy
    Str label;
    bool dpad = false; // Up/Down/Left/Right: pokaz "D-pad", gdy przypisany
};

} // namespace

void PlayerScreen::drawHints(NavInput& nav, const PlayerView&, float x, float y, float w, float h)
{
    const Palette& p = palette();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col32(p.panel));
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col32(p.border));

    std::vector<Hint> hints;
    std::string note;
    ImVec4 noteColor = p.notice;
    if (bindingsOpen_) {
        if (bindCapturing_) note = tr(Str::BindPress);
        else if (bindPending_) hints = {{Action::Confirm, Str::HkSwap}, {Action::Back, Str::HkCancel}};
        else hints = {{Action::Up, Str::HkMove, true}, {Action::Confirm, Str::HkAssign}, {Action::ToggleSolo, Str::HkClear},
                      {Action::Back, Str::HkBack}};
    } else if (settingsOpen_ && dlg_.kind != DlgKind::NetworkConfirm) {
        hints = {{Action::Up, Str::HkAdjust, true}, {Action::Confirm, Str::HkSelect}, {Action::Back, Str::HkBack}};
    } else if (dlg_.kind != DlgKind::None) {
        if (dlg_.kind == DlgKind::DeleteList || dlg_.kind == DlgKind::DeleteEntry || dlg_.kind == DlgKind::NetworkConfirm)
            hints = {{Action::Left, Str::HkSelect, true}, {Action::Confirm, Str::HkOk}, {Action::Back, Str::HkCancel}};
        else if (dlg_.kind == DlgKind::EntryMenu)
            hints = {{Action::Up, Str::HkSelect, true}, {Action::Confirm, Str::HkOk}, {Action::Back, Str::HkBack}};
        else if (dlg_.kind == DlgKind::Report)
            hints = {{Action::Confirm, Str::HkOk}};
        else {
            hints = {{Action::Confirm, Str::HkOk}, {Action::Back, Str::HkCancel}};
            note = tr(Str::HkTextKbd);
        }
    } else {
        switch (nav.focus()) {
        case Panel::Transport:
            hints = {{Action::Up, Str::HkMove, true}, {Action::Confirm, Str::HkSelect}, {Action::PlayPause, Str::HkPlayPause},
                     {Action::Back, Str::HkStop}, {Action::Menu, Str::HkSettings}, {Action::NextTab, Str::HkPanel}};
            break;
        case Panel::Playlist:
            if (pGrid_.row == 2)
                hints = {{Action::Confirm, Str::HkPlay}, {Action::ToggleSolo, Str::HkRemove}, {Action::Menu, Str::HkEntryMenu},
                         {Action::Up, Str::HkMove, true}, {Action::NextTab, Str::HkPanel}};
            else
                hints = {{Action::Confirm, Str::HkSelect}, {Action::Up, Str::HkMove, true}, {Action::NextTab, Str::HkPanel}};
            break;
        case Panel::Info:
            hints = {{Action::Confirm, Str::HkTagLang}, {Action::Up, Str::HkScroll, true}, {Action::NextTab, Str::HkPanel}};
            break;
        case Panel::Tracker:
            if (kGrid_.row == 0)
                hints = {{Action::Confirm, Str::HkSelect}, {Action::Left, Str::HkMove, true}, {Action::NextTab, Str::HkPanel}};
            else
                hints = {{Action::Confirm, Str::HkMute}, {Action::ToggleSolo, Str::HkSolo}, {Action::Menu, Str::HkResetMix},
                         {Action::Left, Str::HkChannel, true}, {Action::NextTab, Str::HkPanel}};
            break;
        default: break;
        }
        if (focusKbdOnly_) note = tr(Str::HkNeedsKbd);
    }

    const std::vector<input::Binding>& eff = effBind_;
    float cx = x + 6.0f * dpi;
    const float ty = y + (h - lh) * 0.5f;
    const float right = x + w - 6.0f * dpi;
    for (const Hint& hint : hints) {
        std::string glyph;
        for (const input::Binding& b : eff) {
            if (b.action != hint.action || b.source.kind == SourceKind::Keyboard) continue;
            const bool isDpad = b.source.kind == SourceKind::GamepadButton &&
                                (b.source.code >= SDL_GAMEPAD_BUTTON_DPAD_UP && b.source.code <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            if (hint.dpad && !isDpad) continue;
            glyph = hint.dpad ? "D-pad" : sourceLabel(b.source);
            break;
        }
        if (glyph.empty()) continue;   // brak przypisania pada: nie obiecujemy przycisku
        const char* label = tr(hint.label);
        const ImVec2 gs = ImGui::CalcTextSize(glyph.c_str());
        const ImVec2 ls = ImGui::CalcTextSize(label);
        const float gw = std::max(gs.x + 8.0f * dpi, lh);
        if (cx + gw + ls.x + 18.0f * dpi > right) break;
        const ImVec2 a(cx, y + (h - lh) * 0.5f - dpi), b(cx + gw, a.y + lh + 2.0f * dpi);
        if (glyph.size() == 1) {   // przyciski ksztaltu kolka (A/B/X/Y)
            const float r = (b.y - a.y) * 0.5f;
            dl->AddCircle(ImVec2(cx + gw * 0.5f, (a.y + b.y) * 0.5f), r, col32(p.accent), 0, 1.5f * dpi);
        } else {
            dl->AddRect(a, b, col32(p.accent), lh * 0.3f, 0, 1.5f * dpi);
        }
        dl->AddText(ImVec2(cx + (gw - gs.x) * 0.5f, ty), col32(p.accent), glyph.c_str());
        cx += gw + 4.0f * dpi;
        dl->AddText(ImVec2(cx, ty), col32(p.text), label);
        cx += ls.x + 14.0f * dpi;
    }
    if (!note.empty()) {
        const float nw = ImGui::CalcTextSize(note.c_str()).x;
        clippedText(dl, std::max(cx, right - nw), right, ty, col32(noteColor), note.c_str());
    }
}

} // namespace vgmplay::ui
