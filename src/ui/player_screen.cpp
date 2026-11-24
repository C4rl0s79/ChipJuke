#include "vgmplay_ui/player_screen.h"

#include "vgmplay_ui/channel_mix.h"
#include "vgmplay_ui/channel_widgets.h"
#include "vgmplay_ui/format.h"
#include "vgmplay_ui/scope_buffer.h"
#include "vgmplay_ui/strings.h"
#include "vgmplay_ui/theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vgmplay::ui {
namespace {

using engine::TransportState;

constexpr int kPatternRows = 64;
constexpr int kTransportButtons = 6; // prev, play, pause, stop, next, open
constexpr double kSeekStepSeconds = 5.0;
constexpr float kVolumeStep = 0.05f;
constexpr std::size_t kScopeFrames = 1024;
constexpr std::size_t kChannelScopeFrames = 768;   // okno przebiegu kanalu (klatki silnika)
constexpr double kVuWindowSeconds = 0.03;          // okno szczytu dla VU

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

// Tekst przyciety do prostokata [x0, x1).
void clippedText(ImDrawList* dl, float x0, float x1, float y, ImU32 color, const char* text)
{
    if (x1 <= x0) return;
    const float lh = ImGui::GetTextLineHeight();
    dl->PushClipRect(ImVec2(x0, y), ImVec2(x1, y + lh + 2.0f), true);
    dl->AddText(ImVec2(x0, y), color, text);
    dl->PopClipRect();
}

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

const char* const kNoteNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                    "F#", "G-", "G#", "A-", "A#", "B-"};

// Deterministyczne dane wzorca (PODGLAD, nie dekodowanie): nuta (-1 = pusto) i glosnosc.
void cellData(int row, int ch, int& note, int& vol)
{
    static const int period[kChannelCount] = {4, 2, 8, 4, 16, 2};
    static const int scale[7] = {0, 2, 3, 5, 7, 8, 10};
    const int p = period[ch % kChannelCount];
    if (row % p != 0) {
        note = -1;
        vol = 0;
        return;
    }
    const int step = row / p;
    const int idx = (step + ch * 2) % 7;
    const int octave = 2 + (ch % kChannelCount) / 2 + ((step / 7) & 1);
    note = scale[idx] + 12 * octave;
    vol = 0x40 - ((step * 5) % 0x20);
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
           v.loading || seekDrag_ || volumeDrag_ || seekHold_ > 0 || vu_.active();
}

void PlayerScreen::apply(const NavEvent& ev, Panel focus, const PlayerView& view, PlayerActions& actions)
{
    switch (ev.action) {
    case Action::TogglePlay: actions.togglePlay(); return;
    case Action::ToggleLang: setLang(lang() == Lang::En ? Lang::Pl : Lang::En); return;
    case Action::ToggleGd3Lang: gd3Jp_ = !gd3Jp_; return;
    case Action::ToggleDiag: diag_ = !diag_; return;
    case Action::ToggleChannelsView: scopesView_ = !scopesView_; return;
    case Action::Back:
        if (view.error || !view.audioError.empty()) { actions.dismissError(); return; }
        if (focus == Panel::Transport && !ev.repeat) actions.stop();
        return;
    default: break;
    }

    const int channels = channelCount(view);
    switch (focus) {
    case Panel::Transport:
        if (ev.action == Action::Left) { actions.seekBySeconds(-kSeekStepSeconds); seekHold_ = kSeekHoldFrames; }
        else if (ev.action == Action::Right) { actions.seekBySeconds(kSeekStepSeconds); seekHold_ = kSeekHoldFrames; }
        else if (ev.action == Action::Up)
            actions.setVolume(std::clamp(view.volume + kVolumeStep, 0.0f, 1.0f));
        else if (ev.action == Action::Down)
            actions.setVolume(std::clamp(view.volume - kVolumeStep, 0.0f, 1.0f));
        else if (ev.action == Action::Accept && !ev.repeat) actions.togglePlay();
        break;
    case Panel::Playlist:
        if (ev.action == Action::Accept && !ev.repeat) actions.requestOpenFile();
        break;
    case Panel::Tracker:
        if (ev.action == Action::Up) {
            if (scopesView_) chan_ = moveGridCursor(chan_, 0, -1, gridCols_, channels);
            else row_ = std::max(0, row_ - 1);
        } else if (ev.action == Action::Down) {
            if (scopesView_) chan_ = moveGridCursor(chan_, 0, 1, gridCols_, channels);
            else row_ = std::min(kPatternRows - 1, row_ + 1);
        }
        else if (ev.action == Action::Left) chan_ = std::max(0, chan_ - 1);
        else if (ev.action == Action::Right) chan_ = std::min(std::max(0, channels - 1), chan_ + 1);
        else if (ev.action == Action::Accept || ev.action == Action::ToggleMute) actions.toggleChannelMute(chan_);
        else if (ev.action == Action::ToggleSolo) actions.toggleChannelSolo(chan_);
        else if (ev.action == Action::UnmuteAll) actions.unmuteAllChannels();
        else if (ev.action == Action::ClearSolo) actions.clearSolo();
        else if (ev.action == Action::ResetMix) { actions.unmuteAllChannels(); actions.clearSolo(); }
        else if (ev.action == Action::MuteChannel || ev.action == Action::SoloChannel) {
            if (ev.arg >= 0 && ev.arg < channels) {
                chan_ = ev.arg;
                if (ev.action == Action::MuteChannel) actions.toggleChannelMute(ev.arg);
                else actions.toggleChannelSolo(ev.arg);
            }
        }
        else if (ev.action == Action::PageUp) row_ = std::max(0, row_ - trackerVisibleRows_);
        else if (ev.action == Action::PageDown)
            row_ = std::min(kPatternRows - 1, row_ + trackerVisibleRows_);
        break;
    default: break;
    }
}

void PlayerScreen::frame(NavInput& nav, const PlayerView& view, PlayerActions& actions,
                         const FrameInfo& info)
{
    for (const NavEvent& ev : nav.events()) apply(ev, nav.focus(), view, actions);
    nav.events().clear();
    if (seekHold_ > 0) --seekHold_;

    const ImGuiIO& io = ImGui::GetIO();
    updateMeters(view, std::clamp(io.DeltaTime, 0.0f, 0.1f));
    actions.setChannelScopesVisible(view.hasTrack()); // oscyloskopy i VU kanalow widoczne w obu widokach
    const float W = io.DisplaySize.x;
    const float H = io.DisplaySize.y;
    const float lh = ImGui::GetTextLineHeight();
    const float gap = std::max(2.0f, lh * 0.3f);

    const float transportH = lh * 3.4f;
    const float statusH = lh * 1.5f;
    const float bodyY = transportH + 2 * gap;
    const float bodyH = H - bodyY - statusH - 2 * gap;
    const float leftW = std::clamp(W * 0.32f, lh * 14.0f, lh * 30.0f);
    const float rightX = leftW + 2 * gap;
    const float rightW = W - rightX - gap;
    const float playlistH = std::clamp(bodyH * 0.22f, lh * 4.5f, lh * 8.0f);
    const float infoH = bodyH - playlistH - gap;
    const float scopeH = std::clamp(bodyH * 0.25f, lh * 5.0f, lh * 12.0f);
    const float trackerH = bodyH - scopeH - gap;

    drawTransport(nav, view, actions, gap, gap, W - 2 * gap, transportH);
    drawPlaylist(nav, view, gap, bodyY, leftW, playlistH);
    drawInfo(view, gap, bodyY + playlistH + gap, leftW, infoH);
    drawTracker(nav, view, actions, rightX, bodyY, rightW, trackerH);
    drawScope(view, rightX, bodyY + trackerH + gap, rightW, scopeH);
    drawStatus(view, actions, gap, H - statusH - gap, W - 2 * gap, statusH);
    if (diag_) drawDiagnostics(view, info, rightX + rightW - std::min(rightW, lh * 30.0f), bodyY,
                               std::min(rightW, lh * 30.0f));
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
                    vuTargets_[static_cast<std::size_t>(g)] = vuLevelFromPeak(pk);
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
    btn[0] = {false, false};                                                    // prev: playlista (E2)
    btn[1] = {has && (st == TransportState::Stopped || st == TransportState::Paused ||
                      st == TransportState::Ended),
              st == TransportState::Playing};
    btn[2] = {st == TransportState::Playing, st == TransportState::Paused};
    btn[3] = {has && (st == TransportState::Playing || st == TransportState::Paused ||
                      st == TransportState::Ended),
              false};
    btn[4] = {false, false};                                                    // next: playlista (E2)
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
        if (hover) {
            ImGui::SetTooltip("%s", labels[i]);
            if (enabled && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                nav.setFocus(Panel::Transport);
                switch (i) {
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
    const std::string title = view.loading ? sanitizeForDisplay(view.loadingName) : trackTitle(view, gd3Jp_);
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
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 3.0f * dpi), col32(p.border));
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw * static_cast<float>(frac), py + 3.0f * dpi), col32(p.accent));
    if (sb.length > 0) {
        const float kx = px + pw * static_cast<float>(frac);
        dl->AddRectFilled(ImVec2(kx - 2.0f * dpi, py - 2.0f * dpi), ImVec2(kx + 2.0f * dpi, py + 5.0f * dpi), col32(p.text));
    }

    ImGui::End();
}

void PlayerScreen::drawPlaylist(NavInput& nav, const PlayerView& view, float x, float y, float w, float h)
{
    const Palette& p = palette();
    if (!beginPanel("##playlist", x, y, w, h)) { ImGui::End(); return; }
    if (clickedInside()) nav.setFocus(Panel::Playlist);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool focused = nav.focus() == Panel::Playlist;
    const float bar = drawFrame(dl, tr(Str::PanelPlaylist), focused);
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const float rowH = lh + 2.0f * dpi;
    const ImVec2 wp = ImGui::GetWindowPos();

    float ry = wp.y + bar + 2.0f * dpi;
    if (view.hasTrack()) {
        const ImVec2 a(wp.x + 2.0f * dpi, ry), b(wp.x + w - 2.0f * dpi, ry + rowH);
        dl->AddRectFilled(a, b, col32(focused ? p.cursorCell : p.gridBar));
        const ImU32 tc = col32(focused ? p.cursorText : p.accent);
        dl->AddText(ImVec2(a.x + 4.0f * dpi, ry + dpi), tc, ">01");
        const std::string len = formatTime(framesToSeconds(view.info->totalFrames, view.sampleRate));
        const float lenW = lh * 3.2f;
        clippedText(dl, a.x + 4.0f * dpi + lh * 2.2f, b.x - lenW, ry + dpi, tc, trackTitle(view, gd3Jp_).c_str());
        dl->AddText(ImVec2(b.x - lenW + 4.0f * dpi, ry + dpi), tc, len.c_str());
        ry += rowH + 2.0f * dpi;
        clippedText(dl, wp.x + 6.0f * dpi, wp.x + w - 4.0f * dpi, ry, col32(p.textDim), tr(Str::PlaylistNote));
    } else {
        clippedText(dl, wp.x + 6.0f * dpi, wp.x + w - 4.0f * dpi, ry + dpi, col32(p.textDim), tr(Str::NoFileHint));
        ry += rowH + 2.0f * dpi;
        clippedText(dl, wp.x + 6.0f * dpi, wp.x + w - 4.0f * dpi, ry, col32(p.textDim), tr(Str::PlaylistNote));
    }
    ImGui::End();
}

void PlayerScreen::drawInfo(const PlayerView& view, float x, float y, float w, float h)
{
    const Palette& p = palette();
    if (!beginPanel("##info", x, y, w, h)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bar = drawFrame(dl, tr(Str::PanelInfo), false);
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 wp = ImGui::GetWindowPos();

    // Przelacznik jezyka tagow EN/JP (klik lub F3), przy prawej krawedzi paska.
    {
        const char* lbl = gd3Jp_ ? "EN [JP]" : "[EN] JP";
        const ImVec2 ts = ImGui::CalcTextSize(lbl);
        const ImVec2 a(wp.x + w - ts.x - 12.0f * dpi, wp.y + 1.0f), b(wp.x + w - 2.0f * dpi, wp.y + bar);
        const bool hover = ImGui::IsMouseHoveringRect(a, b);
        if (hover) {
            ImGui::SetTooltip("%s", tr(Str::Gd3Toggle));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) gd3Jp_ = !gd3Jp_;
        }
        dl->AddText(ImVec2(wp.x + w - ts.x - 6.0f * dpi, wp.y + 2.0f * dpi),
                    col32(hover ? p.accent : p.text), lbl);
    }

    struct Row { std::string label, value; bool notice = false; bool wide = false; };
    std::vector<Row> rows;
    if (view.hasTrack()) {
        const loader::LoadedTrack& t = *view.track;
        const format::Gd3Language gl = gd3Lang(gd3Jp_);
        auto add = [&](Str label, const std::string& value, bool notice = false) {
            if (!value.empty()) rows.push_back({tr(label), sanitizeForDisplay(value), notice});
        };
        rows.push_back({tr(Str::GdTitle), trackTitle(view, gd3Jp_), false});
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
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool focused = nav.focus() == Panel::Tracker;

    const int nch = channelCount(view);
    char title[96];
    if (scopesView_)
        std::snprintf(title, sizeof title, "%s  %d ch", tr(Str::PanelChannels), nch);
    else
        std::snprintf(title, sizeof title, "%s (%s)  %02X/%02X  %d ch", tr(Str::PanelTracker), tr(Str::Preview),
                      row_, kPatternRows - 1, nch);
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
        ImGui::End();
        return;
    }
    const TrackInfo& ti = *view.info;
    chan_ = std::clamp(chan_, 0, nch - 1);
    if (scopesView_) {
        drawChannelGrid(nav, view, actions, wp.x, wp.y + bar, w, h - bar, focused);
        ImGui::End();
        return;
    }

    if (ImGui::IsWindowHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) row_ = std::clamp(row_ - static_cast<int>(wheel * 3), 0, kPatternRows - 1);
    }

    const float rowH = lh + 2.0f * dpi;
    const float headH = rowH + 2.0f * dpi;
    const float vuH = headH * 1.6f;   // pasek VU nad naglowkami kanalow (jak w ProTrackerze)
    const float numW = cw * 4.0f;
    const float chanW = cw * 10.0f;
    const float left = wp.x + 4.0f * dpi;
    const float vuTop = wp.y + bar + 2.0f * dpi;
    const float top = vuTop + vuH + 2.0f * dpi;

    trackerVisibleRows_ = std::max(1, static_cast<int>((h - bar - vuH - 2 * headH - 8.0f * dpi) / rowH));
    const int visChans = std::clamp(static_cast<int>((w - numW - 8.0f * dpi) / chanW), 1, nch);
    if (chan_ < firstChan_) firstChan_ = chan_;
    if (chan_ >= firstChan_ + visChans) firstChan_ = chan_ - visChans + 1;
    firstChan_ = std::clamp(firstChan_, 0, nch - visChans);

    // Kursor pozostaje w srodku widoku, gdy tylko to mozliwe.
    const int firstRow = std::clamp(row_ - trackerVisibleRows_ / 2, 0,
                                    std::max(0, kPatternRows - trackerVisibleRows_));

    // Naglowki: wiersz ukladow (nazwa przy pierwszym kanale ukladu) i wiersz kanalow.
    for (int c = 0; c < visChans; ++c) {
        const int ch = firstChan_ + c;
        const ChannelRef& ref = ti.layout.channels[static_cast<std::size_t>(ch)];
        const DeviceDesc* dev = ref.deviceIdx < ti.layout.devices.size() ? &ti.layout.devices[ref.deviceIdx] : nullptr;
        const bool newDev = c == 0 || ti.layout.channels[static_cast<std::size_t>(ch - 1)].deviceIdx != ref.deviceIdx;
        const float cx = left + numW + c * chanW;
        if (newDev && dev) {
            std::string dn = dev->name.empty() ? std::string(chipTypeName(dev->type)) : dev->name;
            const float ex = cx + chanW * 4.0f;
            clippedText(dl, cx + cw, std::min(ex, wp.x + w - 4.0f * dpi), top + dpi, col32(p.textDim), dn.c_str());
        }
        char hd[32];
        if (dev && ref.channelIdx < dev->channelNames.size())
            std::snprintf(hd, sizeof hd, "%s", dev->channelNames[ref.channelIdx].c_str());
        else std::snprintf(hd, sizeof hd, "CH%d", ch + 1);
        const bool audible = !view.mix || view.mix->audible(ch);
        const ImVec4 chVec = p.channel[ch % kChannelCount];
        const ImU32 chc = col32(audible ? chVec : withAlpha(chVec, 0.4f));
        const float hy = top + headH;
        dl->AddRectFilled(ImVec2(cx, hy), ImVec2(cx + chanW - 2.0f * dpi, hy + headH - 2.0f * dpi),
                          col32(withAlpha(chVec, ch == chan_ ? 0.35f : 0.15f)));
        // VU nad naglowkiem.
        if (static_cast<std::size_t>(ch) < vu_.size())
            drawVuBar(dl, ImVec2(cx + chanW * 0.28f, vuTop), ImVec2(cx + chanW * 0.72f, vuTop + vuH),
                      vu_.channel(static_cast<std::size_t>(ch)), !audible);
        // M / S w naglowku.
        const float bs = headH - 4.0f * dpi;
        const float bx1 = cx + chanW - 2.0f * dpi - 2.0f;
        const ImVec2 sPos(bx1 - bs, hy + dpi), mPos(bx1 - 2.0f * bs - 2.0f * dpi, hy + dpi);
        const bool isMuted = view.mix && view.mix->muted(ch);
        const bool isSolo = view.mix && view.mix->solo(ch);
        bool boxClick = false;
        if (drawToggleBox(dl, mPos, bs, "M", isMuted, p.muteOn, tr(Str::TipMute))) { actions.toggleChannelMute(ch); boxClick = true; }
        if (drawToggleBox(dl, sPos, bs, "S", isSolo, p.soloOn, tr(Str::TipSolo))) { actions.toggleChannelSolo(ch); boxClick = true; }
        if (!boxClick && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImGui::IsMouseHoveringRect(ImVec2(cx, hy), ImVec2(mPos.x - 2.0f, hy + headH)))
            chan_ = ch;
        clippedText(dl, cx + cw, mPos.x - 2.0f * dpi, hy + dpi, chc, hd);
    }

    const float gy = top + 2 * headH;
    dl->PushClipRect(ImVec2(wp.x, gy), ImVec2(wp.x + w, wp.y + h - dpi), true);
    for (int i = 0; i < trackerVisibleRows_; ++i) {
        const int row = firstRow + i;
        if (row >= kPatternRows) break;
        const float ry = gy + i * rowH;
        const ImVec2 a(left, ry), b(wp.x + w - 4.0f * dpi, ry + rowH);
        if (row % 16 == 0) dl->AddRectFilled(a, b, col32(p.gridBar));
        else if (row % 4 == 0) dl->AddRectFilled(a, b, col32(p.gridBeat));
        if (row == row_) dl->AddRectFilled(a, b, col32(p.cursorRow));

        char rn[8];
        std::snprintf(rn, sizeof rn, "%02X", row);
        dl->AddText(ImVec2(left + cw, ry + dpi), col32(row % 4 == 0 ? p.text : p.textDim), rn);

        for (int c = 0; c < visChans; ++c) {
            const int ch = firstChan_ + c;
            const float cx = left + numW + c * chanW;
            int note, vol;
            cellData(row, ch, note, vol);
            char ns[8], vs[8];
            if (note < 0) {
                std::snprintf(ns, sizeof ns, "---");
                std::snprintf(vs, sizeof vs, "..");
            } else {
                std::snprintf(ns, sizeof ns, "%s%d", kNoteNames[note % 12], note / 12);
                std::snprintf(vs, sizeof vs, "%02X", vol);
            }
            const bool cursor = row == row_ && ch == chan_;
            if (cursor)
                dl->AddRectFilled(ImVec2(cx, ry), ImVec2(cx + chanW - 2.0f * dpi, ry + rowH),
                                  col32(focused ? p.cursorCell : withAlpha(p.cursorCell, 0.45f)));
            const ImVec4 chCol = withAlpha(p.channel[ch % kChannelCount], 0.6f); // przygaszone: podglad
            const ImU32 nc = cursor ? col32(p.cursorText) : col32(note < 0 ? p.textDim : chCol);
            const ImU32 vc = cursor ? col32(p.cursorText) : col32(p.textDim);
            dl->AddText(ImVec2(cx + cw, ry + dpi), nc, ns);
            dl->AddText(ImVec2(cx + cw * 6.0f, ry + dpi), vc, vs);
        }
    }
    // Pionowe separatory kanalow.
    for (int c = 0; c <= visChans; ++c) {
        const float cx = left + numW + c * chanW - dpi;
        dl->AddLine(ImVec2(cx, gy), ImVec2(cx, wp.y + h), col32(p.border));
    }
    dl->PopClipRect();
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
    float cx = x + 6.0f * dpi;
    const float right = x + w - 6.0f * dpi;
    (void)focused;

    // Przelacznik widoku (klik lub F4): [Oscyloskopy] Wzorzec.
    {
        char lbl[64];
        if (scopesView_) std::snprintf(lbl, sizeof lbl, "[%s] %s", tr(Str::ViewScopes), tr(Str::ViewPattern));
        else std::snprintf(lbl, sizeof lbl, "%s [%s]", tr(Str::ViewScopes), tr(Str::ViewPattern));
        const ImVec2 ts = ImGui::CalcTextSize(lbl);
        const ImVec2 a(cx, y), b(cx + ts.x + 8.0f * dpi, y + h);
        const bool hover = ImGui::IsMouseHoveringRect(a, b);
        dl->AddText(ImVec2(cx + 4.0f * dpi, ty), col32(hover ? p.accent : p.text), lbl);
        if (hover) {
            ImGui::SetTooltip("%s", tr(Str::ViewToggle));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                scopesView_ = !scopesView_;
                nav.setFocus(Panel::Tracker);
            }
        }
        cx = b.x + 10.0f * dpi;
    }

    // Ogolne wylaczniki.
    struct Btn { Str label; bool enabled; int id; };
    const Btn btns[] = {
        {Str::BtnUnmuteAll, hasCh && view.mix->anyMuted(), 0},
        {Str::BtnClearSolo, hasCh && view.mix->anySolo(), 1},
        {Str::BtnMuteAll, hasCh, 2},
    };
    for (const Btn& bt : btns) {
        const char* label = tr(bt.label);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 a(cx, y + dpi), b(cx + ts.x + 12.0f * dpi, y + h - dpi);
        if (b.x > right) break;
        const bool hover = bt.enabled && ImGui::IsMouseHoveringRect(a, b);
        dl->AddRectFilled(a, b, col32(p.panelHeader));
        dl->AddRect(a, b, col32(hover ? p.focusBorder : p.border));
        dl->AddText(ImVec2(a.x + 6.0f * dpi, ty),
                    col32(!bt.enabled ? withAlpha(p.textDim, 0.5f) : (hover ? p.accent : p.text)), label);
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            nav.setFocus(Panel::Tracker);
            if (bt.id == 0) actions.unmuteAllChannels();
            else if (bt.id == 1) actions.clearSolo();
            else actions.muteAllChannels();
        }
        cx = b.x + 6.0f * dpi;
    }
}

void PlayerScreen::drawChannelGrid(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x,
                                   float y, float w, float h, bool focused)
{
    const Palette& p = palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const TrackInfo& ti = *view.info;
    const int nch = channelCount(view);

    const float margin = 4.0f * dpi;
    const float gap = std::max(2.0f, 3.0f * dpi);
    const GridLayout grid = chooseScopeGrid(nch, w - 2.0f * margin, h - margin - 2.0f * dpi, gap);
    if (grid.cols == 0) return;
    gridCols_ = grid.cols;

    // Mysz: kolko przesuwa kursor kanalu po siatce.
    if (ImGui::IsWindowHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) chan_ = moveGridCursor(chan_, wheel < 0 ? 1 : -1, 0, gridCols_, nch);
    }

    const ChannelScopeBuffer* cs = view.channelScopes;
    const bool epochOk = cs && cs->epoch() == view.transport.epoch &&
                         view.transport.state != TransportState::Idle &&
                         view.transport.state != TransportState::Failed;
    const float headH = lh + 2.0f * dpi;
    const float boxS = lh * 0.95f;

    for (int g = 0; g < nch; ++g) {
        const ChannelRef& ref = ti.layout.channels[static_cast<std::size_t>(g)];
        const DeviceDesc* dev = ref.deviceIdx < ti.layout.devices.size() ? &ti.layout.devices[ref.deviceIdx] : nullptr;
        const float x0 = x + margin + grid.cellX(g), y0 = y + grid.cellY(g);
        const float x1 = x0 + grid.cellW, y1 = y0 + grid.cellH;
        const bool selected = g == chan_;
        const bool audible = !view.mix || view.mix->audible(g);
        const bool isMuted = view.mix && view.mix->muted(g);
        const bool isSolo = view.mix && view.mix->solo(g);
        const ImVec4 chVec = p.channel[g % kChannelCount];

        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col32(p.scopeCell));
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + headH), col32(withAlpha(chVec, selected ? 0.32f : 0.14f)));
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), col32(selected ? (focused ? p.focusBorder : p.textDim) : p.border),
                    0.0f, 0, selected ? 2.0f * dpi : 1.0f);

        // Naglowek: nazwa kanalu (+ uklad, gdy jest miejsce) i przelaczniki M / S.
        const bool showBoxes = grid.cellW >= lh * 5.0f && grid.cellH >= headH + 6.0f * dpi;
        const float bx1 = x1 - 3.0f * dpi;
        const ImVec2 sPos(bx1 - boxS, y0 + (headH - boxS) * 0.5f);
        const ImVec2 mPos(sPos.x - boxS - 3.0f * dpi, sPos.y);
        bool boxClick = false;
        if (showBoxes) {
            if (drawToggleBox(dl, mPos, boxS, "M", isMuted, p.muteOn, tr(Str::TipMute))) {
                actions.toggleChannelMute(g);
                boxClick = true;
            }
            if (drawToggleBox(dl, sPos, boxS, "S", isSolo, p.soloOn, tr(Str::TipSolo))) {
                actions.toggleChannelSolo(g);
                boxClick = true;
            }
        }
        char nm[40];
        if (dev && ref.channelIdx < dev->channelNames.size())
            std::snprintf(nm, sizeof nm, "%s", dev->channelNames[ref.channelIdx].c_str());
        else std::snprintf(nm, sizeof nm, "CH%d", g + 1);
        const float textR = showBoxes ? mPos.x - 3.0f * dpi : x1 - 3.0f * dpi;
        const ImU32 nameCol = col32(audible ? chVec : withAlpha(chVec, 0.45f));
        clippedText(dl, x0 + 4.0f * dpi, textR, y0 + dpi, nameCol, nm);
        if (dev) {
            const float nw = ImGui::CalcTextSize(nm).x;
            const float dx = x0 + 4.0f * dpi + nw + 6.0f * dpi;
            if (textR - dx > lh * 3.0f) {
                std::string dn = dev->name.empty() ? std::string(chipTypeName(dev->type)) : dev->name;
                clippedText(dl, dx, textR, y0 + dpi, col32(withAlpha(p.textDim, 0.8f)), dn.c_str());
            }
        }

        // Pole przebiegu + VU po prawej.
        const float vuW = std::clamp(grid.cellW * 0.07f, 5.0f * dpi, 11.0f * dpi);
        const float wy0 = y0 + headH + dpi, wy1 = y1 - 2.0f * dpi;
        const ImVec2 wa(x0 + 2.0f * dpi, wy0), wb(x1 - vuW - 5.0f * dpi, wy1);
        const ImVec2 va(x1 - vuW - 2.0f * dpi, wy0), vb(x1 - 2.0f * dpi, wy1);
        if (wb.x - wa.x > 8.0f && wb.y - wa.y > 6.0f) {
            const bool live = epochOk && cs->live(ref.deviceIdx, ref.channelIdx);
            const bool have = live && cs->extract(ref.deviceIdx, ref.channelIdx, view.transport.audibleFrame,
                                                  kChannelScopeFrames, wave_);
            if (have) {
                drawWave(dl, wa, wb, wave_, kChannelScopeGain, audible ? chVec : withAlpha(chVec, 0.30f), wavePts_);
            } else {
                drawWave(dl, wa, wb, {}, 1.0f, chVec, wavePts_); // sama linia srodkowa
                if (!live) drawCenteredTag(dl, wa, wb, tr(Str::ScopeNa), withAlpha(p.textDim, 0.8f), false);
            }
            if (!audible) drawCenteredTag(dl, wa, wb, tr(Str::MuteTag), p.muteOn, true);
        }
        if (vb.y - va.y > 6.0f && static_cast<std::size_t>(g) < vu_.size())
            drawVuBar(dl, va, vb, vu_.channel(static_cast<std::size_t>(g)), !audible);

        // Klik w ramke (poza przelacznikami): wybor kanalu.
        if (!boxClick && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImGui::IsMouseHoveringRect(ImVec2(x0, y0), ImVec2(x1, y1))) {
            chan_ = g;
            nav.setFocus(Panel::Tracker);
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
    if (!text.empty() && (view.error || !view.audioError.empty()) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsWindowHovered())
        actions.dismissError();
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
