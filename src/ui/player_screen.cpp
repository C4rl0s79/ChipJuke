#include "vgmplay_ui/player_screen.h"

#include "vgmplay_ui/channel_mix.h"
#include "vgmplay_ui/channel_widgets.h"
#include "vgmplay_ui/format.h"
#include "vgmplay_ui/scope_buffer.h"
#include "screen_util.h"
#include "vgmplay_ui/strings.h"
#include "vgmplay_ui/theme.h"

#include <imgui.h>

#include <algorithm>
#include <optional>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vgmplay::ui {
namespace detail {

// Panel w stalym prostokacie. Zwraca true, gdy mozna rysowac.
bool beginPanel(const char* id, float x, float y, float w, float h)
{
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
    return ImGui::Begin(id, nullptr, flags);
}

// Pasek tytulu + obramowanie; zwraca wysokosc paska.
float drawFrame(ImDrawList* dl, const char* title, bool focused)
{
    const Palette& p = palette();
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float lh = ImGui::GetTextLineHeight();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float bar = lh + 4.0f * dpi;
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + bar),
                      col32(focused ? p.focusHeader : p.panelHeader));
    dl->AddText(ImVec2(pos.x + 6.0f * dpi, pos.y + 2.0f * dpi),
                col32(focused ? p.text : p.textDim), title);
    const float th = focused ? 2.0f * dpi : 1.0f;
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                col32(focused ? p.focusBorder : p.border), 0.0f, 0, th);
    return bar;
}

bool clickedInside()
{
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
           ImGui::IsWindowHovered(ImGuiHoveredFlags_None);
}

void drawFocusRing(ImDrawList* dl, ImVec2 a, ImVec2 b, float dpi)
{
    const Palette& p = palette();
    dl->AddRect(ImVec2(a.x - dpi, a.y - dpi), ImVec2(b.x + dpi, b.y + dpi), col32(p.accent), 0.0f, 0, 2.0f * dpi);
    dl->AddRect(a, b, col32(p.focusBorder), 0.0f, 0, dpi);
}

// Tekst przyciety do prostokata [x0, x1).
void clippedText(ImDrawList* dl, float x0, float x1, float y, ImU32 color, const char* text)
{
    if (x1 <= x0) return;
    const float lh = ImGui::GetTextLineHeight();
    dl->PushClipRect(ImVec2(x0, y), ImVec2(x1, y + lh + 2.0f), true);
    dl->AddText(ImVec2(x0, y), color, text);
    dl->PopClipRect();
}



bool ButtonFlow::button(const std::string& label, bool enabled, bool active, const char* tip, bool ring)
{
    const Palette& p = palette();
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    const float bw = ts.x + 12.0f * dpi;
    if (cx > x0 && cx + bw > right) newline();
    const ImVec2 a(cx, cy), b(cx + bw, cy + h);
    const bool hover = enabled && ImGui::IsMouseHoveringRect(a, b);
    dl->AddRectFilled(a, b, col32(active ? withAlpha(p.accent, 0.25f) : p.panelHeader));
    dl->AddRect(a, b, col32(hover ? p.focusBorder : p.border));
    dl->PushClipRect(a, b, true);
    dl->AddText(ImVec2(a.x + 6.0f * dpi, cy + (h - ts.y) * 0.5f),
                col32(!enabled ? withAlpha(p.textDim, 0.5f) : (active ? p.accent : (hover ? p.accent : p.text))),
                label.c_str());
    dl->PopClipRect();
    if (ring) drawFocusRing(dl, a, b, dpi);
    if (hover && tip) ImGui::SetTooltip("%s", tip);
    cx = b.x + 4.0f * dpi;
    return hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

void ButtonFlow::text(const std::string& label, float maxW, ImU32 color)
{
    const float w = std::min(ImGui::CalcTextSize(label.c_str()).x, maxW);
    if (cx > x0 && cx + w > right) newline();
    clippedText(dl, cx, cx + w, cy + (h - ImGui::GetTextLineHeight()) * 0.5f, color, label.c_str());
    cx += w + 6.0f * dpi;
}

} // namespace detail
using namespace detail;

namespace {

using engine::TransportState;

constexpr int kTransportButtons = 6; // prev, play, pause, stop, next, open
constexpr double kSeekStepSeconds = 5.0;
constexpr float kVolumeStep = 0.05f;
constexpr std::size_t kScopeFrames = 1024;
constexpr std::size_t kChannelScopeFrames = 768;   // okno przebiegu kanalu (klatki silnika)
constexpr double kVuWindowSeconds = 0.03;          // okno szczytu dla VU

std::string fileNameUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.filename().u8string();
    return std::string(u8.begin(), u8.end());
}

format::Gd3Language gd3Lang(bool jp) { return jp ? format::Gd3Language::Jp : format::Gd3Language::En; }

std::string trackTitle(const PlayerView& v, bool jp)
{
    if (!v.track) return tr(Str::NoFileTitle);
    if (v.track->gd3.present) {
        const std::string& t = v.track->gd3.track(gd3Lang(jp));
        if (!t.empty()) return sanitizeForDisplay(t);
    }
    return sanitizeForDisplay(fileNameUtf8(v.track->path));
}

int channelCount(const PlayerView& v)
{
    return v.info ? static_cast<int>(v.info->layout.channels.size()) : 0;
}

struct ButtonState {
    bool enabled = false;
    bool active = false;
};

} // namespace

bool PlayerScreen::wantsAnimation(const PlayerView& v) const
{
    return v.transport.state == TransportState::Playing || v.transport.state == TransportState::Loading ||
           v.loading || v.scan.active || v.playlistsLoading || dragging_ || seekDrag_ || volumeDrag_ || seekHold_ > 0 || vu_.active();
}

void PlayerScreen::applyTransport(const NavEvent& ev, const PlayerView& view, PlayerActions& actions)
{
    FocusGrid& g = tGrid_;   // wiersz 0: przyciski, 1: pasek pozycji, 2: glosnosc
    switch (ev.action) {
    case Action::Up: g.moveRow(-1); break;
    case Action::Down: g.moveRow(1); break;
    case Action::Left:
    case Action::Right: {
        const int d = ev.action == Action::Left ? -1 : 1;
        if (g.row == 0) g.moveCol(d);
        else if (g.row == 1) { actions.seekBySeconds(d * kSeekStepSeconds); seekHold_ = kSeekHoldFrames; }
        else actions.setVolume(std::clamp(view.volume + static_cast<float>(d) * kVolumeStep, 0.0f, 1.0f));
        break;
    }
    case Action::Confirm:
        if (ev.repeat) break;
        if (g.row == 0) activate_ = true;
        else if (g.row == 1) actions.togglePlay();
        break;
    case Action::Menu:
        if (!ev.repeat) openSettings();
        break;
    default: break;
    }
}

void PlayerScreen::applyToolbar(const NavEvent& ev, const PlayerView&, PlayerActions&)
{
    switch (ev.action) {
    case Action::Left: kGrid_.moveCol(-1); break;
    case Action::Right: kGrid_.moveCol(1); break;
    case Action::Down: kGrid_.moveRow(1); break;
    case Action::Confirm:
        if (!ev.repeat) activate_ = true;
        break;
    default: break;
    }
}

void PlayerScreen::apply(const NavEvent& ev, Panel focus, const PlayerView& view, PlayerActions& actions)
{
    if (modalOpen()) { applyModal(ev, view, actions); return; }
    switch (ev.action) {
    case Action::PlayPause: if (!ev.repeat) actions.togglePlay(); return;
    case Action::Stop: if (!ev.repeat) actions.stop(); return;
    case Action::SeekBack: actions.seekBySeconds(-kSeekStepSeconds); seekHold_ = kSeekHoldFrames; return;
    case Action::SeekForward: actions.seekBySeconds(kSeekStepSeconds); seekHold_ = kSeekHoldFrames; return;
    case Action::VolumeUp: actions.setVolume(std::clamp(view.volume + kVolumeStep, 0.0f, 1.0f)); return;
    case Action::VolumeDown: actions.setVolume(std::clamp(view.volume - kVolumeStep, 0.0f, 1.0f)); return;
    case Action::ToggleLang: toggleLanguage(view, actions); return;
    case Action::ToggleGd3Lang: toggleGd3(view, actions); return;
    case Action::PrevTrack: if (!ev.repeat) actions.playPrevious(); return;
    case Action::NextTrack: if (!ev.repeat) actions.playNext(); return;
    case Action::Shuffle: if (!ev.repeat) actions.shufflePlaylist(); return;
    case Action::ToggleSettings: if (!ev.repeat) openSettings(); return;
    case Action::NewPlaylist:
    case Action::RenamePlaylist:
    case Action::DeletePlaylist:
        if (ev.repeat || !view.playlists || view.playlists->playlistCount() == 0) return;
        {
            const std::size_t cnt = view.playlists->playlistCount();
            const std::size_t ai = view.playlists->activeIndex();
            if (ev.action == Action::NewPlaylist) {
                char nm[96];
                std::snprintf(nm, sizeof nm, "%s %zu", tr(Str::DefaultListName), cnt + 1);
                openDialog(DlgKind::NewList, 0, nm);
            } else if (ev.action == Action::RenamePlaylist) {
                openDialog(DlgKind::RenameList, ai, view.playlists->playlist(ai).name);
            } else if (cnt > 1) {
                openDialog(DlgKind::DeleteList, ai);
            }
        }
        return;
    case Action::ToggleLoop:
        if (!ev.repeat) {
            using playlist::RepeatMode;
            actions.setRepeat(view.repeat == RepeatMode::Off ? RepeatMode::All
                              : view.repeat == RepeatMode::All ? RepeatMode::One : RepeatMode::Off);
        }
        return;
    case Action::ToggleDiag: if (!ev.repeat) diag_ = !diag_; return;
    case Action::ToggleTrackerView: if (!ev.repeat) cycleStrip(view, actions); return;
    case Action::PatternFaster: actions.setPatternRowTicks(stepRowTicks(view.patternRowTicks, 1)); return;
    case Action::PatternSlower: actions.setPatternRowTicks(stepRowTicks(view.patternRowTicks, -1)); return;
    case Action::Back:
        if (view.error || !view.audioError.empty()) { actions.dismissError(); return; }
        if (ev.repeat) return;
        if (view.notice) { actions.dismissNotice(); return; }
        if (focus == Panel::Transport) actions.stop();
        return;
    default: break;
    }

    const int channels = channelCount(view);
    switch (focus) {
    case Panel::Transport:
        applyTransport(ev, view, actions);
        break;
    case Panel::Playlist:
        applyPlaylist(ev, view, actions);
        break;
    case Panel::Info:
        if (ev.action == Action::Up) infoScroll_ = std::max(0, infoScroll_ - 1);
        else if (ev.action == Action::Down) ++infoScroll_;   // ogranicza rysowanie
        else if (ev.action == Action::PageUp) infoScroll_ = std::max(0, infoScroll_ - 5);
        else if (ev.action == Action::PageDown) infoScroll_ += 5;
        else if (ev.action == Action::Confirm && !ev.repeat) toggleGd3(view, actions);
        break;
    case Panel::Tracker:
        if (kGrid_.row == 0) { applyToolbar(ev, view, actions); break; }
        // Wzorzec przewija sie razem z pozycja slyszana (seek: panel Odtwarzanie); Gora wraca do paska narzedzi,
        // kursor kanalu: Lewo/Prawo.
        if (ev.action == Action::Up) { if (!ev.repeat) kGrid_.moveRow(-1); }
        else if (ev.action == Action::Left) chan_ = std::max(0, chan_ - 1);
        else if (ev.action == Action::Right) chan_ = std::min(std::max(0, channels - 1), chan_ + 1);
        else if (ev.action == Action::Confirm || ev.action == Action::ToggleMute) { if (!ev.repeat) actions.toggleChannelMute(chan_); }
        else if (ev.action == Action::ToggleSolo) { if (!ev.repeat) actions.toggleChannelSolo(chan_); }
        else if (ev.action == Action::UnmuteAll) actions.unmuteAllChannels();
        else if (ev.action == Action::ClearSolo) actions.clearSolo();
        else if (ev.action == Action::ResetMix || ev.action == Action::Menu) {
            if (!ev.repeat) { actions.unmuteAllChannels(); actions.clearSolo(); }
        }
        else if (ev.action == Action::MuteChannel || ev.action == Action::SoloChannel) {
            if (ev.arg >= 0 && ev.arg < channels) {
                chan_ = ev.arg;
                if (ev.action == Action::MuteChannel) actions.toggleChannelMute(ev.arg);
                else actions.toggleChannelSolo(ev.arg);
            }
        }
        break;
    default: break;
    }
}

void PlayerScreen::cycleStrip(const PlayerView& view, PlayerActions& actions)
{
    cycleStrip();
    if (!view.settings) return;
    settings::Settings s = *view.settings;
    s.ui.scopeBar = static_cast<settings::ScopeBar>(strip_);
    actions.applySettings(s);   // zapamietanie wysokosci paska
}

void PlayerScreen::frame(NavInput& nav, const PlayerView& view, PlayerActions& actions,
                         const FrameInfo& info)
{
    nav_ = &nav;
    if (view.settings) {
        setLang(view.settings->ui.language == i18n::Language::Pl ? Lang::Pl : Lang::En);
        const settings::InputSettings& in = view.settings->input;
        if (!bindSynced_ || in.bindings != lastBind_ || in.repeatDelayMs != lastRepeatDelay_ || in.repeatRateMs != lastRepeatRate_) {
            bindSynced_ = true;
            lastBind_ = in.bindings;
            lastRepeatDelay_ = in.repeatDelayMs;
            lastRepeatRate_ = in.repeatRateMs;
            effBind_ = input::effectiveBindings(lastBind_);
            nav.setBindings(lastBind_);
            nav.setRepeat(in.repeatDelayMs, in.repeatRateMs);
        }
    }
    pollCapture(nav, view, actions);
    focusKbdOnly_ = false;
    for (const NavEvent& ev : nav.events()) {
        const bool modalBefore = modalOpen();
        apply(ev, nav.focus(), view, actions);
        if (modalBefore != modalOpen()) break;   // nacisniecia z kolejki nie naleza do nowego okna (purgePresses)
    }
    nav.events().clear();
    if (needDeviceRefresh_ && settingsOpen_) { actions.refreshAudioDevices(); needDeviceRefresh_ = false; }
    if (seekHold_ > 0) --seekHold_;

    const ImGuiIO& io = ImGui::GetIO();
    updateMeters(view, std::clamp(io.DeltaTime, 0.0f, 0.1f));
    actions.setChannelScopesVisible(view.hasTrack() && strip_ != ScopeStrip::Hidden); // pasek oscyloskopow i VU kanalow
    const float W = io.DisplaySize.x;
    const float H = io.DisplaySize.y;
    const float lh = ImGui::GetTextLineHeight();
    const float gap = std::max(2.0f, lh * 0.3f);

    const float transportH = lh * 3.4f;
    const float statusH = lh * 1.5f;
    const float bodyY = transportH + 2 * gap;
    const bool hints = nav.lastInputWasPad() || (view.settings && view.settings->ui.showButtonHints);
    const float hintH = hints ? lh * 1.7f : 0.0f;
    const float hintSpace = hints ? hintH + gap : 0.0f;
    const float bodyH = H - bodyY - statusH - 2 * gap - hintSpace;
    const float leftW = std::clamp(W * 0.32f, lh * 14.0f, lh * 30.0f);
    const float rightX = leftW + 2 * gap;
    const float rightW = W - rightX - gap;
    const float playlistH = std::clamp(bodyH * 0.5f, lh * 9.0f, bodyH * 0.65f);
    const float infoH = bodyH - playlistH - gap;
    const float scopeH = std::clamp(bodyH * 0.25f, lh * 5.0f, lh * 12.0f);
    const float trackerH = bodyH - scopeH - gap;

    // Okno modalne: panele nie dostaja myszy (pozycja poza ekranem, brak klikniec), dialog ja odzyskuje.
    if (view.notice && view.notice->serial != seenNotice_) {
        seenNotice_ = view.notice->serial;
        if (!view.notice->details.empty() || view.notice->needsAck) openDialog(DlgKind::Report);
    }
    const bool modal = modalOpen();
    ImGuiIO& mio = ImGui::GetIO();
    const ImVec2 savedPos = mio.MousePos;
    bool savedClicked[3] = {}, savedDown[3] = {}, savedReleased[3] = {}, savedDouble[3] = {};
    const float savedWheel = mio.MouseWheel;
    if (modal) {
        for (int i = 0; i < 3; ++i) {
            savedClicked[i] = mio.MouseClicked[i]; savedDown[i] = mio.MouseDown[i];
            savedReleased[i] = mio.MouseReleased[i]; savedDouble[i] = mio.MouseDoubleClicked[i];
            mio.MouseClicked[i] = mio.MouseDown[i] = mio.MouseReleased[i] = mio.MouseDoubleClicked[i] = false;
        }
        mio.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        mio.MouseWheel = 0.0f;
    }
    drawTransport(nav, view, actions, gap, gap, W - 2 * gap, transportH);
    drawPlaylist(nav, view, actions, gap, bodyY, leftW, playlistH);
    drawInfo(nav, view, actions, gap, bodyY + playlistH + gap, leftW, infoH);
    drawTracker(nav, view, actions, rightX, bodyY, rightW, trackerH);
    drawScope(view, rightX, bodyY + trackerH + gap, rightW, scopeH);
    drawStatus(view, actions, gap, H - statusH - gap - hintSpace, W - 2 * gap, statusH);
    if (diag_) drawDiagnostics(view, info, rightX + rightW - std::min(rightW, lh * 30.0f), bodyY,
                               std::min(rightW, lh * 30.0f));
    if (modal) {
        for (int i = 0; i < 3; ++i) {
            mio.MouseClicked[i] = savedClicked[i]; mio.MouseDown[i] = savedDown[i];
            mio.MouseReleased[i] = savedReleased[i]; mio.MouseDoubleClicked[i] = savedDouble[i];
        }
        mio.MousePos = savedPos;
        mio.MouseWheel = savedWheel;
    }
    drawModal(nav, view, actions, W, H);
    if (hints) drawHints(nav, view, gap, H - hintH - gap, W - 2 * gap, hintH);
    activate_ = false;
    nav.setOverlay(bindingsOpen_ ? std::optional<input::ScreenId>(input::ScreenId::Bindings)
                   : settingsOpen_ ? std::optional<input::ScreenId>(input::ScreenId::Settings)
                   : dlg_.kind != DlgKind::None ? std::optional<input::ScreenId>(input::ScreenId::Dialog)
                                                : std::nullopt);
}

void PlayerScreen::updateMeters(const PlayerView& view, float dt)
{
    const int nch = channelCount(view);
    vu_.resize(static_cast<std::size_t>(nch));
    vuTargets_.assign(static_cast<std::size_t>(nch), 0.0f);
    float master[2] = {0.0f, 0.0f};
    const engine::TransportSnapshot& t = view.transport;
    if (t.state == TransportState::Playing) { // pauza/stop: wskazniki opadaja do zera
        const std::size_t win = std::max<std::size_t>(
            256, static_cast<std::size_t>(kVuWindowSeconds * static_cast<double>(view.sampleRate)));
        const ChannelScopeBuffer* cs = view.channelScopes;
        if (cs && cs->epoch() == t.epoch && view.info) {
            for (int g = 0; g < nch; ++g) {
                const ChannelRef& r = view.info->layout.channels[static_cast<std::size_t>(g)];
                float pk = 0.0f;
                if (cs->peak(r.deviceIdx, r.channelIdx, t.audibleFrame, win, pk))
                    vuTargets_[static_cast<std::size_t>(g)] = vuLevelFromPeak(pk, kChannelVuTopDb);
            }
        }
        const ScopeBuffer* sb = view.scope;
        float l = 0.0f, r = 0.0f;
        if (sb && sb->epoch() == t.epoch && sb->peak(t.audibleFrame, win, l, r)) {
            master[0] = vuLevelFromPeak(l);
            master[1] = vuLevelFromPeak(r);
        }
    }
    vu_.update(vuTargets_, master, dt);
}

void PlayerScreen::drawTransport(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x,
                                 float y, float w, float h)
{
    const Palette& p = palette();
    if (!beginPanel("##transport", x, y, w, h)) { ImGui::End(); return; }
    if (clickedInside()) nav.setFocus(Panel::Transport);
    tGrid_.begin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool focused = nav.focus() == Panel::Transport;
    const float bar = drawFrame(dl, tr(Str::PanelTransport), focused);
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImGuiIO& io = ImGui::GetIO();

    if (focused) {
        const char* hint = tr(Str::HintTransport);
        const float hw = ImGui::CalcTextSize(hint).x;
        if (hw + lh * 12.0f < w) dl->AddText(ImVec2(wp.x + w - hw - 8.0f * dpi, wp.y + 2.0f * dpi), col32(p.textDim), hint);
    }

    const TransportState st = view.transport.state;
    const bool has = view.hasTrack();
    ButtonState btn[kTransportButtons];
    const bool hasList = view.playlists && view.playlists->playlistCount() > 0 &&
                         !view.playlists->playlist(view.playlists->activeIndex()).entries.empty();
    btn[0] = {hasList || has, false};                                           // prev
    btn[1] = {has && (st == TransportState::Stopped || st == TransportState::Paused ||
                      st == TransportState::Ended),
              st == TransportState::Playing};
    btn[2] = {st == TransportState::Playing, st == TransportState::Paused};
    btn[3] = {has && (st == TransportState::Playing || st == TransportState::Paused ||
                      st == TransportState::Ended),
              false};
    btn[4] = {hasList, false};                                                  // next
    btn[5] = {true, false};                                                     // open
    const char* labels[kTransportButtons] = {tr(Str::BtnPrev), tr(Str::BtnPlay), tr(Str::BtnPause),
                                             tr(Str::BtnStop),  tr(Str::BtnNext), tr(Str::BtnOpen)};

    const float bs = std::min(h - bar - 12.0f * dpi, lh * 1.7f);
    const float by = wp.y + bar + (h - bar - 6.0f * dpi - bs) * 0.5f;
    float bx = wp.x + 8.0f * dpi;
    for (int i = 0; i < kTransportButtons; ++i) {
        const ImVec2 a(bx, by), b(bx + bs, by + bs);
        const bool hover = ImGui::IsMouseHoveringRect(a, b);
        const bool enabled = btn[i].enabled;
        const bool active = btn[i].active;
        dl->AddRectFilled(a, b, col32(active ? withAlpha(p.accent, 0.25f) : p.panelHeader));
        dl->AddRect(a, b, col32(hover && enabled ? p.focusBorder : p.border), 0.0f, 0,
                    hover && enabled ? 2.0f * dpi : 1.0f);
        tGrid_.reg(0, i);
        const bool ring = ringAt(tGrid_, focused, 0, i);
        const bool act = activatedAt(tGrid_, focused, 0, i) && enabled;
        if (ring) {
            drawFocusRing(dl, a, b, dpi);
            if (i == 5) focusKbdOnly_ = true;   // okno wyboru plikow: klawiatura/mysz
        }
        const ImU32 ic = col32(!enabled ? withAlpha(p.textDim, 0.45f) : (active ? p.accent : p.text));
        const float m = bs * 0.28f;
        const float cx = bx + bs * 0.5f, cy = by + bs * 0.5f, r = bs * 0.5f - m;
        switch (i) {
        case 0:
            dl->AddRectFilled(ImVec2(cx - r, cy - r), ImVec2(cx - r + r * 0.35f, cy + r), ic);
            dl->AddTriangleFilled(ImVec2(cx + r, cy - r), ImVec2(cx + r, cy + r), ImVec2(cx - r * 0.5f, cy), ic);
            break;
        case 1:
            dl->AddTriangleFilled(ImVec2(cx - r * 0.7f, cy - r), ImVec2(cx - r * 0.7f, cy + r), ImVec2(cx + r, cy), ic);
            break;
        case 2:
            dl->AddRectFilled(ImVec2(cx - r, cy - r), ImVec2(cx - r * 0.25f, cy + r), ic);
            dl->AddRectFilled(ImVec2(cx + r * 0.25f, cy - r), ImVec2(cx + r, cy + r), ic);
            break;
        case 3:
            dl->AddRectFilled(ImVec2(cx - r, cy - r), ImVec2(cx + r, cy + r), ic);
            break;
        case 4:
            dl->AddRectFilled(ImVec2(cx + r - r * 0.35f, cy - r), ImVec2(cx + r, cy + r), ic);
            dl->AddTriangleFilled(ImVec2(cx - r, cy - r), ImVec2(cx - r, cy + r), ImVec2(cx + r * 0.5f, cy), ic);
            break;
        default: // "wysun": trojkat w gore nad kreska
            dl->AddTriangleFilled(ImVec2(cx - r, cy + r * 0.1f), ImVec2(cx + r, cy + r * 0.1f), ImVec2(cx, cy - r), ic);
            dl->AddRectFilled(ImVec2(cx - r, cy + r * 0.45f), ImVec2(cx + r, cy + r), ic);
            break;
        }
        if (hover) ImGui::SetTooltip("%s", labels[i]);
        {
            if (enabled && ((hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || act)) {
                nav.setFocus(Panel::Transport);
                switch (i) {
                case 0: actions.playPrevious(); break;
                case 4: actions.playNext(); break;
                case 1: actions.play(); break;
                case 2: actions.pause(); break;
                case 3: actions.stop(); break;
                case 5: actions.requestOpenFile(); break;
                default: break;
                }
            }
        }
        bx += bs + 6.0f * dpi + (i == 4 ? 8.0f * dpi : 0.0f);
    }

    // Glosnosc (prawa strona): etykieta + suwak.
    const float volW = lh * 6.0f;
    const float volLabelW = lh * 4.2f;
    const float volX1 = wp.x + w - 10.0f * dpi;
    const float volX0 = volX1 - volW;
    const float vcy = by + bs * 0.5f;
    {
        char vb[32];
        std::snprintf(vb, sizeof vb, "%s %d%%", tr(Str::Volume), static_cast<int>(view.volume * 100.0f + 0.5f));
        const float lw = ImGui::CalcTextSize(vb).x;
        dl->AddText(ImVec2(volX0 - lw - 8.0f * dpi, vcy - lh * 0.5f), col32(p.textDim), vb);
        const ImVec2 a(volX0, vcy - 3.0f * dpi), b(volX1, vcy + 3.0f * dpi);
        const ImVec2 ha(volX0, vcy - 8.0f * dpi), hb(volX1, vcy + 8.0f * dpi);
        if (!volumeDrag_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(ha, hb))
            volumeDrag_ = true;
        if (volumeDrag_) {
            actions.setVolume(std::clamp((io.MousePos.x - volX0) / volW, 0.0f, 1.0f));
            if (!io.MouseDown[0]) volumeDrag_ = false;
        }
        dl->AddRectFilled(a, b, col32(p.border));
        dl->AddRectFilled(a, ImVec2(volX0 + volW * std::clamp(view.volume, 0.0f, 1.0f), b.y), col32(p.accent));
        tGrid_.reg(2, 0);
        if (ringAt(tGrid_, focused, 2, 0)) drawFocusRing(dl, ha, hb, dpi);
    }

    // Czas i tytul.
    const SeekBar sb = [&] {
        const TrackInfo* ti = view.info.get();
        return makeSeekBar(view.transport.audibleFrame, view.transport.totalFrames,
                           ti ? ti->introFrames : 0, ti ? ti->loopFrames : 0,
                           ti ? ti->introFrames + ti->loopFrames : 0);
    }();
    const std::uint64_t shownPos =
        seekDrag_ ? fractionToFrame(seekFraction_, sb.length) : sb.position;
    char tb[64];
    if (view.loading) std::snprintf(tb, sizeof tb, "%s...", tr(Str::Loading));
    else if (sb.length == 0) std::snprintf(tb, sizeof tb, "0:00 / 0:00");
    else
        std::snprintf(tb, sizeof tb, "%s / %s", formatTime(framesToSeconds(shownPos, view.sampleRate)).c_str(),
                      sb.infinite ? "inf" : formatTime(framesToSeconds(sb.length, view.sampleRate)).c_str());
    const float tx = bx + 8.0f * dpi;
    const float ty = by + (bs - lh) * 0.5f;
    dl->AddText(ImVec2(tx, ty), col32(p.text), tb);
    const float titleX = tx + ImGui::CalcTextSize("00:00 / 00:00").x + lh;
    const std::string title = view.loading ? sanitizeForDisplay(view.loadingName) : trackTitle(view, jp(view));
    clippedText(dl, titleX, volX0 - volLabelW - 8.0f * dpi, ty, col32(has ? p.accent : p.textDim), title.c_str());

    // Pasek pozycji (mysz: klik/przeciaganie; klawiatura: Lewo/Prawo przy fokusie panelu).
    const float px = wp.x + 8.0f * dpi;
    const float pw = w - 16.0f * dpi;
    const float py = wp.y + h - 6.0f * dpi;
    const ImVec2 ha(px, py - 6.0f * dpi), hb(px + pw, py + 8.0f * dpi);
    if (!seekDrag_ && sb.length > 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsMouseHoveringRect(ha, hb)) {
        seekDrag_ = true;
        nav.setFocus(Panel::Transport);
    }
    if (seekDrag_) {
        seekFraction_ = std::clamp(static_cast<double>((io.MousePos.x - px) / pw), 0.0, 1.0);
        if (!io.MouseDown[0]) {
            actions.seekToFrame(fractionToFrame(seekFraction_, sb.length));
            seekDrag_ = false;
            seekHold_ = kSeekHoldFrames;
        }
    }
    const double frac = seekDrag_ ? seekFraction_
                                  : (sb.length ? static_cast<double>(sb.position) / static_cast<double>(sb.length) : 0.0);
    tGrid_.reg(1, 0);
    if (ringAt(tGrid_, focused, 1, 0)) drawFocusRing(dl, ImVec2(px - 2.0f * dpi, py - 5.0f * dpi), ImVec2(px + pw + 2.0f * dpi, py + 7.0f * dpi), dpi);
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 3.0f * dpi), col32(p.border));
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw * static_cast<float>(frac), py + 3.0f * dpi), col32(p.accent));
    if (sb.length > 0) {
        const float kx = px + pw * static_cast<float>(frac);
        dl->AddRectFilled(ImVec2(kx - 2.0f * dpi, py - 2.0f * dpi), ImVec2(kx + 2.0f * dpi, py + 5.0f * dpi), col32(p.text));
    }

    tGrid_.end();
    ImGui::End();
}

void PlayerScreen::drawInfo(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x, float y, float w, float h)
{
    const Palette& p = palette();
    if (!beginPanel("##info", x, y, w, h)) { ImGui::End(); return; }
    if (clickedInside()) nav.setFocus(Panel::Info);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool focused = nav.focus() == Panel::Info;
    const float bar = drawFrame(dl, tr(Str::PanelInfo), focused);
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 wp = ImGui::GetWindowPos();

    // Przelacznik jezyka tagow EN/JP (klik lub F3), przy prawej krawedzi paska.
    {
        const char* lbl = jp(view) ? "EN [JP]" : "[EN] JP";
        const ImVec2 ts = ImGui::CalcTextSize(lbl);
        const ImVec2 a(wp.x + w - ts.x - 12.0f * dpi, wp.y + 1.0f), b(wp.x + w - 2.0f * dpi, wp.y + bar);
        const bool hover = ImGui::IsMouseHoveringRect(a, b);
        if (hover) {
            ImGui::SetTooltip("%s", tr(Str::Gd3Toggle));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) toggleGd3(view, actions);
        }
        dl->AddText(ImVec2(wp.x + w - ts.x - 6.0f * dpi, wp.y + 2.0f * dpi),
                    col32(hover ? p.accent : p.text), lbl);
        if (focused && !modalOpen()) drawFocusRing(dl, ImVec2(a.x - 2.0f * dpi, a.y + dpi), ImVec2(b.x, b.y - dpi), dpi);
    }

    struct Row { std::string label, value; bool notice = false; bool wide = false; };
    std::vector<Row> rows;
    if (view.hasTrack()) {
        const loader::LoadedTrack& t = *view.track;
        const format::Gd3Language gl = gd3Lang(jp(view));
        auto add = [&](Str label, const std::string& value, bool notice = false) {
            if (!value.empty()) rows.push_back({tr(label), sanitizeForDisplay(value), notice});
        };
        rows.push_back({tr(Str::GdTitle), trackTitle(view, jp(view)), false});
        if (t.gd3.present) {
            add(Str::GdGame, t.gd3.game(gl));
            add(Str::GdSystem, t.gd3.system(gl));
            add(Str::GdAuthor, t.gd3.author(gl));
            add(Str::GdDate, t.gd3.releaseDate);
        }
        char vb[48];
        std::snprintf(vb, sizeof vb, "VGM %X.%02X%s", (t.header.versionBcd >> 8) & 0xFFu,
                      t.header.versionBcd & 0xFFu, t.wasGzip ? " (gzip)" : "");
        add(Str::InfoFormat, vb);
        const std::uint32_t sr = view.sampleRate;
        add(Str::InfoLength, formatTime(framesToSeconds(view.info->totalFrames, sr)));
        if (t.stream.loopDisabled) {
            rows.push_back({tr(Str::InfoLoop), tr(Str::LoopDisabled), true});
        } else if (view.info->loopFrames > 0) {
            char lb[96];
            std::snprintf(lb, sizeof lb, "%s + %s", formatTime(framesToSeconds(view.info->introFrames, sr)).c_str(),
                          formatTime(framesToSeconds(view.info->loopFrames, sr)).c_str());
            add(Str::InfoLoop, lb);
        } else {
            add(Str::InfoLoop, tr(Str::LoopNone));
        }
        bool firstChip = true;
        for (const format::ChipEntry& c : t.header.chips) {
            std::string v(chipTypeName(c.type));
            if (c.instance > 0) v += " #2";
            v += "  " + formatClock(c.clockHz);
            rows.push_back({firstChip ? tr(Str::InfoChips) : "", v, false});
            firstChip = false;
        }
        if (t.gd3.present) {
            add(Str::GdRipper, t.gd3.converter);
            add(Str::GdNotes, t.gd3.notes);
        }
    } else {
        rows.push_back({"", tr(Str::NoFileHint), false, true});
    }

    const float rowH = lh + 3.0f * dpi;
    const int visible = std::max(1, static_cast<int>((h - bar - 6.0f * dpi) / rowH));
    const int maxScroll = std::max(0, static_cast<int>(rows.size()) - visible);
    if (ImGui::IsWindowHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) infoScroll_ -= static_cast<int>(wheel * 2);
    }
    infoScroll_ = std::clamp(infoScroll_, 0, maxScroll);

    const float labelW = lh * 6.5f;
    float ry = wp.y + bar + 4.0f * dpi;
    for (int i = infoScroll_; i < static_cast<int>(rows.size()); ++i) {
        if (ry + lh > wp.y + h) break;
        const Row& r = rows[static_cast<std::size_t>(i)];
        dl->AddText(ImVec2(wp.x + 6.0f * dpi, ry), col32(p.textDim), r.label.c_str());
        clippedText(dl, wp.x + 6.0f * dpi + (r.wide ? 0.0f : labelW), wp.x + w - 4.0f * dpi, ry,
                    col32(r.notice ? p.notice : p.text), r.value.c_str());
        ry += rowH;
    }
    if (maxScroll > 0) { // wskaznik przewijania
        const float th = h - bar - 6.0f * dpi;
        const float bh = std::max(8.0f * dpi, th * visible / static_cast<float>(rows.size()));
        const float by = wp.y + bar + 3.0f * dpi + (th - bh) * infoScroll_ / static_cast<float>(maxScroll);
        dl->AddRectFilled(ImVec2(wp.x + w - 3.0f * dpi, by), ImVec2(wp.x + w - 1.0f * dpi, by + bh), col32(p.border));
    }
    ImGui::End();
}

void PlayerScreen::drawTracker(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x, float y,
                               float w, float h)
{
    const Palette& p = palette();
    if (!beginPanel("##tracker", x, y, w, h)) { ImGui::End(); return; }
    if (clickedInside()) nav.setFocus(Panel::Tracker);
    kGrid_.begin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool focused = nav.focus() == Panel::Tracker;
    const ImGuiIO& io = ImGui::GetIO();

    const int nch = channelCount(view);
    const chipstate::Pattern* pat = view.pattern.get();
    const bool hasLoop = view.info && view.info->loopFrames > 0;
    PatternTiming timing;
    std::uint64_t cur = 0;
    currentRow_ = -1;
    if (pat) {
        timing = timingOf(*pat, hasLoop);
        cur = rowAtFrame(view.transport.audibleFrame, view.sampleRate, timing);
        currentRow_ = static_cast<long long>(cur);
    }

    char title[128];
    int len = std::snprintf(title, sizeof title, "%s  %d ch", tr(Str::PanelTracker), nch);
    if (visCols_ > 0 && visCols_ < nch && len > 0 && len < static_cast<int>(sizeof title)) // przewijanie poziome (z poprzedniej klatki)
        len += std::snprintf(title + len, sizeof title - static_cast<std::size_t>(len), " (%d-%d)", firstChan_ + 1,
                             firstChan_ + visCols_);
    if (pat && len > 0 && len < static_cast<int>(sizeof title))
        std::snprintf(title + len, sizeof title - static_cast<std::size_t>(len), "  %llX/%llX",
                      static_cast<unsigned long long>(cur),
                      static_cast<unsigned long long>(pat->rowCount ? pat->rowCount - 1 : 0));
    const float titleBar = drawFrame(dl, title, focused);
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const float cw = ImGui::CalcTextSize("0").x;
    const ImVec2 wp = ImGui::GetWindowPos();
    if (focused) { // podpowiedz klawiszy w pasku tytulu (jak w panelu Transport)
        const char* hint = tr(Str::HintChannels);
        const float hw = ImGui::CalcTextSize(hint).x;
        const float titleW = ImGui::CalcTextSize(title).x;
        if (hw + titleW + lh * 4.0f < w)
            dl->AddText(ImVec2(wp.x + w - hw - 8.0f * dpi, wp.y + 2.0f * dpi), col32(p.text), hint);
    }
    const float toolH = lh + 6.0f * dpi;
    drawChannelToolbar(nav, view, actions, wp.x, wp.y + titleBar + dpi, w, toolH, focused);
    const float bar = titleBar + toolH + dpi;

    if (nch == 0) {
        clippedText(dl, wp.x + 8.0f * dpi, wp.x + w - 4.0f * dpi, wp.y + bar + 6.0f * dpi, col32(p.textDim),
                    tr(Str::NoChannels));
        visCols_ = 0;
        kGrid_.reg(1, 0);
        kGrid_.end();
        ImGui::End();
        return;
    }
    const TrackInfo& ti = *view.info;
    chan_ = std::clamp(chan_, 0, nch - 1);
    columnMap_ = mapPatternColumns(ti.layout, pat);

    // ---- geometria: [urzadzenia][pasek oscyloskopow][naglowki][wzorzec], kolumny wspolne ----
    const bool showFx = view.patternHasEffects;
    const float rowH = lh + 2.0f * dpi;
    const float headH = lh + 4.0f * dpi;
    const float devH = lh + 2.0f * dpi;
    const float colW = std::floor(cw * (static_cast<float>(showFx ? kCellCharsFx : kCellCharsNoFx) + 1.5f));
    int digits = 2;
    if (pat && pat->rowCount > 1)
        for (std::uint64_t v = (pat->rowCount - 1) >> 8; v != 0; v >>= 4) ++digits; // 2 cyfry do 0xFF
    if (digits < 2) digits = 2;
    const float numW = std::floor(cw * (static_cast<float>(digits) + 1.5f));
    const float left = wp.x + 4.0f * dpi;

    const int visChans = std::clamp(static_cast<int>((w - numW - 8.0f * dpi) / colW), 1, nch);
    visCols_ = visChans;
    if (ImGui::IsWindowHovered()) { // kolko: przesuwa kursor kanalu (kolumny przewijaja sie za nim)
        const float wheel = io.MouseWheel;
        if (wheel != 0.0f) chan_ = std::clamp(chan_ + (wheel < 0 ? 1 : -1), 0, nch - 1);
    }
    if (chan_ < firstChan_) firstChan_ = chan_;
    if (chan_ >= firstChan_ + visChans) firstChan_ = chan_ - visChans + 1;
    firstChan_ = std::clamp(firstChan_, 0, nch - visChans);

    const float stripWanted = strip_ == ScopeStrip::Hidden ? 0.0f : lh * (strip_ == ScopeStrip::Large ? 8.4f : 4.2f);
    const float stripH = std::min(stripWanted, (h - bar) * 0.4f);
    float yy = wp.y + bar + 2.0f * dpi;
    const float devTop = yy;
    yy += devH;
    const float stripTop = yy;
    yy += stripH > 0.0f ? stripH + 2.0f * dpi : 0.0f;
    const float headTop = yy;
    yy += headH;
    const float gridTop = yy + dpi;
    const float gridBottom = wp.y + h - dpi;
    const int visRows = std::max(1, static_cast<int>((gridBottom - gridTop) / rowH));
    const int cursorIdx = visRows / 2;
    const float rightEdge = wp.x + w - 4.0f * dpi;
    const float gap = std::max(2.0f, 2.0f * dpi);

    // Auto-wzmocnienie przebiegow: stan zerowany przy nowym utworze.
    if (gainTrack_ != view.track.get() || autoGain_.size() != static_cast<std::size_t>(nch)) {
        autoGain_.assign(static_cast<std::size_t>(nch), ScopeAutoGain{});
        gainTrack_ = view.track.get();
    }
    const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
    const ChannelScopeBuffer* cs = view.channelScopes;
    const bool epochOk = cs && cs->epoch() == view.transport.epoch &&
                         view.transport.state != TransportState::Idle &&
                         view.transport.state != TransportState::Failed;

    // ---- kolumny: urzadzenie, oscyloskop + VU, naglowek z nazwa i M/S ----
    for (int c = 0; c < visChans; ++c) {
        const int ch = firstChan_ + c;
        const ChannelRef& ref = ti.layout.channels[static_cast<std::size_t>(ch)];
        const DeviceDesc* dev = ref.deviceIdx < ti.layout.devices.size() ? &ti.layout.devices[ref.deviceIdx] : nullptr;
        const float cx = left + numW + static_cast<float>(c) * colW;
        const float x1 = cx + colW - gap;
        const bool selected = ch == chan_;
        const bool audible = !view.mix || view.mix->audible(ch);
        const ImVec4 chVec = p.channel[ch % kChannelCount];

        // Nazwa ukladu nad pierwsza kolumna ukladu (przycieta do jego widocznych kolumn).
        const bool newDev = c == 0 || ti.layout.channels[static_cast<std::size_t>(ch - 1)].deviceIdx != ref.deviceIdx;
        if (newDev && dev) {
            int run = 1;
            while (c + run < visChans &&
                   ti.layout.channels[static_cast<std::size_t>(ch + run)].deviceIdx == ref.deviceIdx)
                ++run;
            const std::string dn = dev->name.empty() ? std::string(chipTypeName(dev->type)) : dev->name;
            clippedText(dl, cx + cw * 0.5f, std::min(cx + static_cast<float>(run) * colW - gap, rightEdge),
                        devTop + dpi, col32(p.textDim), dn.c_str());
        }

        // Oscyloskop kanalu (rzad na gorze) z VU przy prawej krawedzi ramki.
        if (stripH > 0.0f) {
            const float y0 = stripTop, y1 = stripTop + stripH;
            dl->AddRectFilled(ImVec2(cx, y0), ImVec2(x1, y1), col32(p.scopeCell));
            dl->AddRect(ImVec2(cx, y0), ImVec2(x1, y1),
                        col32(selected ? (focused && kGrid_.row == 1 ? p.focusBorder : p.textDim) : p.border), 0.0f, 0,
                        selected ? 2.0f * dpi : 1.0f);
            const float vuW = std::clamp(colW * 0.07f, 5.0f * dpi, 9.0f * dpi);
            const ImVec2 wa(cx + 2.0f * dpi, y0 + 2.0f * dpi), wb(x1 - vuW - 4.0f * dpi, y1 - 2.0f * dpi);
            const ImVec2 va(x1 - vuW - 2.0f * dpi, y0 + 2.0f * dpi), vb(x1 - 2.0f * dpi, y1 - 2.0f * dpi);
            const bool live = epochOk && cs->live(ref.deviceIdx, ref.channelIdx);
            const bool have = live && cs->extract(ref.deviceIdx, ref.channelIdx, view.transport.audibleFrame,
                                                  kChannelScopeFrames, wave_);
            if (have) {
                float winPeak = 0.0f;
                for (const float v : wave_) winPeak = std::max(winPeak, std::fabs(v));
                const float gain = autoGain_[static_cast<std::size_t>(ch)].update(winPeak, dt);
                drawWave(dl, wa, wb, wave_, gain, audible ? chVec : withAlpha(chVec, 0.30f), wavePts_);
            } else {
                drawWave(dl, wa, wb, {}, 1.0f, chVec, wavePts_); // sama linia srodkowa
                if (!live) drawCenteredTag(dl, wa, wb, tr(Str::ScopeNa), withAlpha(p.textDim, 0.8f), false);
            }
            if (!audible) drawCenteredTag(dl, wa, wb, tr(Str::MuteTag), p.muteOn, true);
            if (vb.y - va.y > 6.0f && static_cast<std::size_t>(ch) < vu_.size())
                drawVuBar(dl, va, vb, vu_.channel(static_cast<std::size_t>(ch)), !audible);
        }

        // Naglowek kolumny wzorca: nazwa kanalu (kolor kanalu) + M / S.
        char hd[40];
        if (dev && ref.channelIdx < dev->channelNames.size())
            std::snprintf(hd, sizeof hd, "%s", dev->channelNames[ref.channelIdx].c_str());
        else std::snprintf(hd, sizeof hd, "CH%d", ch + 1);
        dl->AddRectFilled(ImVec2(cx, headTop), ImVec2(x1, headTop + headH),
                          col32(withAlpha(chVec, selected ? 0.35f : 0.15f)));
        const float bs = headH - 4.0f * dpi;
        const ImVec2 sPos(x1 - bs - 2.0f * dpi, headTop + 2.0f * dpi);
        const ImVec2 mPos(sPos.x - bs - 2.0f * dpi, sPos.y);
        const bool isMuted = view.mix && view.mix->muted(ch);
        const bool isSolo = view.mix && view.mix->solo(ch);
        bool boxClick = false;
        if (drawToggleBox(dl, mPos, bs, "M", isMuted, p.muteOn, tr(Str::TipMute))) { actions.toggleChannelMute(ch); boxClick = true; }
        if (drawToggleBox(dl, sPos, bs, "S", isSolo, p.soloOn, tr(Str::TipSolo))) { actions.toggleChannelSolo(ch); boxClick = true; }
        clippedText(dl, cx + cw * 0.5f, mPos.x - 2.0f * dpi, headTop + dpi * 2.0f,
                    col32(audible ? chVec : withAlpha(chVec, 0.45f)), hd);

        // Klik w kolumne (poza M/S): wybor kanalu.
        if (!boxClick && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImGui::IsMouseHoveringRect(ImVec2(cx, stripH > 0.0f ? stripTop : headTop), ImVec2(x1, gridBottom))) {
            chan_ = ch;
            nav.setFocus(Panel::Tracker);
        }
    }

    // ---- wzorzec: kursor odtwarzania w srodku, wiersze przesuwaja sie razem z pozycja slyszana ----
    dl->PushClipRect(ImVec2(wp.x, gridTop), ImVec2(wp.x + w, gridBottom + dpi), true);
    if (!pat) {
        clippedText(dl, left + cw, rightEdge, gridTop + rowH * static_cast<float>(cursorIdx), col32(p.textDim),
                    tr(view.patternBuilding || view.loading ? Str::PatternBuilding : Str::PatternNone));
    }
    static const char* const kDots = "\xC2\xB7\xC2\xB7\xC2\xB7";
    const ImU32 dimEmpty = col32(withAlpha(p.textDim, 0.40f));
    for (int i = 0; pat && i < visRows; ++i) {
        const float ry = gridTop + static_cast<float>(i) * rowH;
        const bool isCursor = i == cursorIdx;
        const std::optional<std::uint64_t> dr = displayRow(static_cast<std::int64_t>(cur) + (i - cursorIdx), timing);
        const ImVec2 a(left, ry), b(rightEdge, ry + rowH);
        if (dr) {
            if (*dr % 16 == 0) dl->AddRectFilled(a, b, col32(p.gridBar));
            else if (*dr % 4 == 0) dl->AddRectFilled(a, b, col32(p.gridBeat));
        }
        if (isCursor) dl->AddRectFilled(a, b, col32(p.cursorRow));
        if (!dr) continue;

        char rn[24];
        std::snprintf(rn, sizeof rn, "%0*llX", digits, static_cast<unsigned long long>(*dr));
        dl->AddText(ImVec2(left + cw * 0.5f, ry + dpi), col32(isCursor || *dr % 4 == 0 ? p.text : p.textDim), rn);

        for (int c = 0; c < visChans; ++c) {
            const int ch = firstChan_ + c;
            const float cx = left + numW + static_cast<float>(c) * colW;
            const float tx = cx + cw * 0.75f;
            const bool sel = isCursor && ch == chan_;
            if (sel)
                dl->AddRectFilled(ImVec2(cx, ry), ImVec2(cx + colW - gap, ry + rowH),
                                  col32(focused ? p.cursorCell : withAlpha(p.cursorCell, 0.45f)));
            const bool audible = !view.mix || view.mix->audible(ch);
            const ImVec4 chVec = p.channel[ch % kChannelCount];
            const int col = columnMap_[static_cast<std::size_t>(ch)];
            const std::size_t idx = col >= 0 ? static_cast<std::size_t>(*dr) * pat->channels.size() +
                                                   static_cast<std::size_t>(col)
                                             : 0;
            if (col < 0 || idx >= pat->cells.size()) { // kanal bez dekodera (lub brak w danych): n/a
                dl->AddText(ImVec2(tx, ry + dpi), sel ? col32(p.cursorText) : dimEmpty, kDots);
                if (isCursor)
                    dl->AddText(ImVec2(tx + cw * 4.0f, ry + dpi), sel ? col32(p.cursorText) : col32(p.textDim),
                                tr(Str::ScopeNa));
                continue;
            }
            const chipstate::Cell& cell = pat->cells[idx];
            if (!cellHasEvent(cell)) {
                const ImU32 ec = sel ? col32(p.cursorText) : dimEmpty;
                dl->AddText(ImVec2(tx, ry + dpi), ec, showFx ? "--- .. .. ..." : "--- .. ..");
                continue;
            }
            const CellText t = formatCell(cell);
            const float a_ = audible ? 1.0f : 0.4f;
            const ImU32 noteC = sel ? col32(p.cursorText)
                                    : (cell.note == chipstate::kNoteNone ? dimEmpty : col32(withAlpha(chVec, a_)));
            auto field = [&](float off, const char* s, bool present, const ImVec4& color) {
                dl->AddText(ImVec2(tx + cw * off, ry + dpi),
                            sel ? col32(p.cursorText) : (present ? col32(withAlpha(color, a_)) : dimEmpty), s);
            };
            dl->AddText(ImVec2(tx, ry + dpi), noteC, t.note);
            field(4.0f, t.inst, cell.instrument != chipstate::kValueNone, p.text);
            field(7.0f, t.vol, cell.volume != chipstate::kValueNone, p.textDim);
            if (showFx) field(10.0f, t.fx, cell.effect != 0 || cell.effectParam != 0, p.accent);
        }
    }
    // Pionowe separatory kolumn (w osi z ramkami oscyloskopow).
    for (int c = 0; c <= visChans; ++c) {
        const float sx = left + numW + static_cast<float>(c) * colW - gap * 0.5f;
        dl->AddLine(ImVec2(sx, gridTop), ImVec2(sx, gridBottom + dpi), col32(p.border));
    }
    dl->PopClipRect();
    kGrid_.reg(1, 0);
    kGrid_.end();
    ImGui::End();
}

void PlayerScreen::drawChannelToolbar(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x,
                                      float y, float w, float h, bool focused)
{
    const Palette& p = palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const float ty = y + (h - lh) * 0.5f;
    const bool hasCh = view.mix && view.mix->channelCount() > 0;

    // Przycisk-ramka (kolumna `col` w wierszu 0 mapy fokusu). Zwraca: -1 = nie miesci sie (nic nie rysuje),
    // 0 = rysowany, 1 = klikniety myszka lub potwierdzony padem/klawiatura.
    auto button = [&](float& cx, float right, const std::string& label, bool enabled, const char* tip, int col) {
        const char* text = label.c_str();
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 a(cx, y + dpi), b(cx + ts.x + 12.0f * dpi, y + h - dpi);
        if (b.x > right) return -1;
        const bool hover = enabled && ImGui::IsMouseHoveringRect(a, b);
        kGrid_.reg(0, col);
        const bool ring = ringAt(kGrid_, focused, 0, col);
        dl->AddRectFilled(a, b, col32(p.panelHeader));
        dl->AddRect(a, b, col32(hover ? p.focusBorder : p.border));
        dl->AddText(ImVec2(a.x + 6.0f * dpi, ty),
                    col32(!enabled ? withAlpha(p.textDim, 0.5f) : (hover ? p.accent : p.text)), text);
        if (ring) drawFocusRing(dl, a, b, dpi);
        if (hover && tip) ImGui::SetTooltip("%s", tip);
        cx = b.x + 6.0f * dpi;
        const bool act = enabled && activatedAt(kGrid_, focused, 0, col);
        return (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || act ? 1 : 0;
    };

    // Prawa strona: dlugosc wiersza wzorca  [-] Row 1/60 s [+]  (krotszy wiersz = szybciej).
    const std::string rowLabel = std::string(tr(Str::RowSpeed)) + " " + rowTicksLabel(view.patternRowTicks);
    const float rw = ImGui::CalcTextSize(rowLabel.c_str()).x;
    const float btnW = ImGui::CalcTextSize("+").x + 12.0f * dpi;
    const float rowGroupW = btnW * 2.0f + rw + 18.0f * dpi;
    float right = x + w - 6.0f * dpi;
    float rx = right - rowGroupW;
    if (rx > x + lh * 8.0f) {
        const bool slowerOk = view.patternRowTicks < kRowTickPresets[kRowTickPresetCount - 1];
        const bool fasterOk = view.patternRowTicks > kRowTickPresets[0];
        if (button(rx, right, "-", slowerOk, tr(Str::RowSpeedTip), 4) == 1) {
            actions.setPatternRowTicks(stepRowTicks(view.patternRowTicks, -1));
            nav.setFocus(Panel::Tracker);
        }
        dl->AddText(ImVec2(rx + 2.0f * dpi, ty), col32(p.text), rowLabel.c_str());
        rx += rw + 10.0f * dpi;
        if (button(rx, right + 1.0f, "+", fasterOk, tr(Str::RowSpeedTip), 5) == 1) {
            actions.setPatternRowTicks(stepRowTicks(view.patternRowTicks, 1));
            nav.setFocus(Panel::Tracker);
        }
        right = x + w - 6.0f * dpi - rowGroupW - 8.0f * dpi;
    }

    float cx = x + 6.0f * dpi;
    // Wysokosc paska oscyloskopow (klik lub F4): maly -> duzy -> ukryty.
    {
        const Str st = strip_ == ScopeStrip::Small ? Str::StripSmall
                       : strip_ == ScopeStrip::Large ? Str::StripLarge : Str::StripHidden;
        const std::string lbl = std::string(tr(Str::ViewScopes)) + ": " + tr(st);
        if (button(cx, right, lbl, true, tr(Str::ViewToggle), 0) == 1) {
            cycleStrip(view, actions);
            nav.setFocus(Panel::Tracker);
        }
    }

    // Ogolne wylaczniki.
    struct Btn { Str label; bool enabled; int id; };
    const Btn btns[] = {
        {Str::BtnUnmuteAll, hasCh && view.mix->anyMuted(), 0},
        {Str::BtnClearSolo, hasCh && view.mix->anySolo(), 1},
        {Str::BtnMuteAll, hasCh, 2},
    };
    for (const Btn& bt : btns) {
        const int r = button(cx, right, tr(bt.label), bt.enabled, nullptr, 1 + bt.id);
        if (r < 0) break;
        if (r == 1) {
            nav.setFocus(Panel::Tracker);
            if (bt.id == 0) actions.unmuteAllChannels();
            else if (bt.id == 1) actions.clearSolo();
            else actions.muteAllChannels();
        }
    }
}

void PlayerScreen::drawScope(const PlayerView& view, float x, float y, float w, float h)
{
    const Palette& p = palette();
    if (!beginPanel("##scope", x, y, w, h)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bar = drawFrame(dl, tr(Str::PanelScope), false);
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 wp = ImGui::GetWindowPos();

    const float ax = wp.x + 4.0f * dpi, ay = wp.y + bar + 4.0f * dpi;
    const float lh = ImGui::GetTextLineHeight();
    const float vuBarW = 9.0f * dpi;
    const float vuRegion = 2.0f * vuBarW + 14.0f * dpi; // dwa slupki master L/R po prawej
    const float aw = w - 8.0f * dpi - vuRegion, ah = h - bar - 8.0f * dpi;
    if (aw < 8.0f || ah < 8.0f) { ImGui::End(); return; }
    const float cy = ay + ah * 0.5f;

    for (int i = 1; i < 8; ++i) {
        const float gx = ax + aw * i / 8.0f;
        dl->AddLine(ImVec2(gx, ay), ImVec2(gx, ay + ah), col32(p.scopeGrid));
    }
    for (int i = 0; i <= 4; ++i) {
        const float gy = ay + ah * i / 4.0f;
        dl->AddLine(ImVec2(ax, gy), ImVec2(ax + aw, gy), col32(p.scopeGrid));
    }

    // Dane: okno zaczynajace sie od pozycji slyszanej (nie renderowanej) i triggera na zerze.
    // Bloki z innej epoki niz transport (po seeku/stopie) nie sa rysowane.
    const ScopeBuffer* sb = view.scope;
    bool have = false;
    if (sb && sb->epoch() == view.transport.epoch && view.transport.state != TransportState::Idle &&
        view.transport.state != TransportState::Failed)
        have = sb->extract(view.transport.audibleFrame, kScopeFrames, scopeL_, scopeR_);

    static std::vector<ImVec2> pts;
    const int n = std::clamp(static_cast<int>(aw / (2.0f * dpi)), 64, 1024);
    pts.resize(static_cast<std::size_t>(n));
    dl->PushClipRect(ImVec2(ax, ay), ImVec2(ax + aw, ay + ah), true);
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<float>& src = pass == 0 ? scopeR_ : scopeL_; // L na wierzchu
        for (int i = 0; i < n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n - 1);
            float v = 0.0f;
            if (have) v = std::clamp(src[static_cast<std::size_t>(u * static_cast<float>(kScopeFrames - 1))], -1.0f, 1.0f);
            pts[static_cast<std::size_t>(i)] = ImVec2(ax + aw * u, cy - v * ah * 0.5f * 0.95f);
        }
        dl->AddPolyline(pts.data(), n, col32(pass == 0 ? p.scopeLineR : p.scopeLine), ImDrawFlags_None, 1.5f * dpi);
    }
    dl->PopClipRect();

    // Master VU (L/R), jak ProTracker: pionowe segmenty + litery pod slupkami.
    {
        const float vx = ax + aw + 8.0f * dpi;
        const float vy1 = ay + ah - lh;
        const ImVec2 la(vx, ay), lb(vx + vuBarW, vy1), ra(vx + vuBarW + 4.0f * dpi, ay), rb(vx + 2.0f * vuBarW + 4.0f * dpi, vy1);
        drawVuBar(dl, la, lb, vu_.masterL(), false);
        drawVuBar(dl, ra, rb, vu_.masterR(), false);
        dl->AddText(ImVec2(la.x + (vuBarW - ImGui::CalcTextSize("L").x) * 0.5f, vy1 + dpi), col32(p.scopeLine), "L");
        dl->AddText(ImVec2(ra.x + (vuBarW - ImGui::CalcTextSize("R").x) * 0.5f, vy1 + dpi), col32(p.scopeLineR), "R");
    }
    ImGui::End();
}

void PlayerScreen::drawStatus(const PlayerView& view, PlayerActions& actions, float x, float y, float w, float h)
{
    const Palette& p = palette();
    if (!beginPanel("##status", x, y, w, h)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddRect(wp, ImVec2(wp.x + w, wp.y + h), col32(p.border));
    const float ty = wp.y + (h - lh) * 0.5f;
    const float x0 = wp.x + 6.0f * dpi, x1 = wp.x + w - 6.0f * dpi;

    std::string text;
    ImVec4 color = p.textDim;
    if (view.error) {
        const Error& e = *view.error;
        text = std::string(tr(Str::ErrorPrefix)) + " [" + errorCodeName(e.code) + "]: " + trError(e.code);
        if (!view.errorFile.empty()) text += "  " + view.errorFile;
        if (!e.message.empty()) text += "  | " + e.message;
        if (e.byteOffset != kNoOffset) {
            char ob[32];
            std::snprintf(ob, sizeof ob, " @0x%llX", static_cast<unsigned long long>(e.byteOffset));
            text += ob;
        }
        color = p.error;
    } else if (!view.audioError.empty()) {
        text = std::string(tr(Str::ErrorPrefix)) + ": " + tr(Str::AudioUnavailable) + " (" +
               tr(Str::AudioSilentFallback) + ")  | " + view.audioError;
        color = p.error;
    } else if (view.notice) {
        text = view.notice->text;
        color = view.notice->error ? p.error : p.notice;
    } else if (view.loading) {
        text = std::string(tr(Str::Loading)) + " " + view.loadingName + "...";
        color = p.text;
    } else if (view.track && !view.track->warnings.empty()) {
        std::vector<WarningCode> seen;
        text = tr(Str::WarningPrefix);
        text += ": ";
        for (const Warning& wn : view.track->warnings) {
            if (std::find(seen.begin(), seen.end(), wn.code) != seen.end()) continue;
            if (!seen.empty()) text += "; ";
            seen.push_back(wn.code);
            text += trWarning(wn.code);
        }
        color = p.notice;
    }
    text = sanitizeForDisplay(text);
    clippedText(dl, x0, x1, ty, col32(color), text.c_str());
    if (!text.empty() && (view.error || !view.audioError.empty() || view.notice) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
        if (view.error || !view.audioError.empty()) actions.dismissError();
        else actions.dismissNotice();
    }
    ImGui::End();
}

void PlayerScreen::drawDiagnostics(const PlayerView& view, const FrameInfo& info, float x, float y, float w)
{
    const Palette& p = palette();
    const float lh = ImGui::GetTextLineHeight();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    static const char* const kStates[] = {"Idle", "Loading", "Stopped", "Playing", "Paused", "Ended", "Failed"};
    const engine::TransportSnapshot& t = view.transport;

    char l[8][160];
    int n = 0;
    auto line = [&](const char* fmt, auto... args) { std::snprintf(l[n++], sizeof l[0], fmt, args...); };
    if (info.idle) line("%s %.1f [%s]", tr(Str::Fps), info.fps, tr(Str::Idle));
    else line("%s %.0f", tr(Str::Fps), info.fps);
    line("state %s  epoch %llu  loop %u%s", kStates[std::min<std::size_t>(static_cast<std::size_t>(t.state), 6)],
         static_cast<unsigned long long>(t.epoch), t.loopIndex, t.fadeActive ? "  fade" : "");
    line("audible %llu  rendered %llu  total %llu", static_cast<unsigned long long>(t.audibleFrame),
         static_cast<unsigned long long>(t.renderedFrame), static_cast<unsigned long long>(t.totalFrames));
    line("audio %s %u Hz  buf %.1f ms", view.audio.backend.c_str(), view.audio.sampleRate, view.audio.bufferMs);
    line("%s", view.audio.device.c_str());
    line("ring %lld frames (%.0f ms)  underruns pcm %u / engine %u",
         static_cast<long long>(view.audio.queuedFrames),
         view.audio.sampleRate ? 1000.0 * static_cast<double>(view.audio.queuedFrames) / view.audio.sampleRate : 0.0,
         view.audio.underruns, t.underruns);
    if (view.scope) line("scope %llu..%llu  epoch %llu", static_cast<unsigned long long>(view.scope->beginFrame()),
                         static_cast<unsigned long long>(view.scope->endFrame()),
                         static_cast<unsigned long long>(view.scope->epoch()));

    const float h = (lh + 2.0f * dpi) * (n + 1) + 6.0f * dpi;
    // Nakladka na liscie "foreground": zawsze na wierzchu, bez okna ImGui (kolejnosc okien jest
    // nieistotna).
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 a(x, y), b(x + w, y + h);
    const float bar = lh + 4.0f * dpi;
    dl->AddRectFilled(a, b, col32(withAlpha(p.panel, 0.94f)));
    dl->AddRectFilled(a, ImVec2(b.x, a.y + bar), col32(p.focusHeader));
    dl->AddText(ImVec2(a.x + 6.0f * dpi, a.y + 2.0f * dpi), col32(p.text), tr(Str::Diagnostics));
    dl->AddRect(a, b, col32(p.focusBorder));
    float ry = a.y + bar + 3.0f * dpi;
    for (int i = 0; i < n; ++i) {
        clippedText(dl, a.x + 6.0f * dpi, b.x - 4.0f * dpi, ry, col32(p.text), l[i]);
        ry += lh + 2.0f * dpi;
    }
}

} // namespace vgmplay::ui
