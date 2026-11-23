#include "vgmplay_ui/mockup.h"

#include "vgmplay_ui/strings.h"
#include "vgmplay_ui/theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace vgmplay::ui {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kPatternRows = 64;
constexpr int kTransportButtons = 5; // prev, play, pause, stop, next

struct Track {
    const char* title;
    const char* length;
};

// Dane przykladowe (nie tlumaczone - to dane, nie interfejs).
const Track kPlaylist[] = {
    {"Green Hill Zone - Act 1", "2:14"},
    {"Streets of Rage - Fighting in the Street", "3:02"},
    {"グリーンヒルゾーン 1面", "2:14"},
    {"Gunstar Heroes - Fire", "1:48"},
    {"Ys Book I&II - Feena", "3:41"},
    {"Rainbow Islands - Theme", "2:27"},
    {"Gradius - Aircraft Carrier", "2:56"},
    {"ダライアス - Boss", "1:33"},
    {"Out Run - Magical Sound Shower", "3:25"},
    {"Thunder Force III - Stage 1", "2:49"},
    {"R-Type - Stage 1", "2:05"},
    {"Contra - Jungle", "2:38"},
};
constexpr int kPlaylistCount = static_cast<int>(sizeof(kPlaylist) / sizeof(kPlaylist[0]));

const char* const kChipLine = "YM2612 + SN76489";

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

const char* const kNoteNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                    "F#", "G-", "G#", "A-", "A#", "B-"};

// Deterministyczne dane wzorca: nuta (-1 = pusto) i glosnosc.
void cellData(int row, int ch, int& note, int& vol)
{
    static const int period[kChannelCount] = {4, 2, 8, 4, 16, 2};
    static const int scale[7] = {0, 2, 3, 5, 7, 8, 10};
    const int p = period[ch];
    if (row % p != 0) {
        note = -1;
        vol = 0;
        return;
    }
    const int step = row / p;
    const int idx = (step + ch * 2) % 7;
    const int octave = 2 + ch / 2 + ((step / 7) & 1);
    note = scale[idx] + 12 * octave;
    vol = 0x40 - ((step * 5) % 0x20);
}

} // namespace

void Mockup::apply(const NavEvent& ev, Panel focus)
{
    switch (ev.action) {
    case Action::TogglePlay: playing_ = !playing_; return;
    case Action::ToggleLang:
        setLang(lang() == Lang::En ? Lang::Pl : Lang::En);
        return;
    default: break;
    }

    switch (focus) {
    case Panel::Transport:
        if (ev.action == Action::Left) transportSel_ = std::max(0, transportSel_ - 1);
        else if (ev.action == Action::Right)
            transportSel_ = std::min(kTransportButtons - 1, transportSel_ + 1);
        else if (ev.action == Action::Accept) {
            if (transportSel_ == 1) playing_ = true;
            else if (transportSel_ == 2 || transportSel_ == 3) playing_ = false;
            // prev/next: bez dzialania w makiecie
        }
        break;
    case Panel::Playlist:
        if (ev.action == Action::Up) playlistSel_ = std::max(0, playlistSel_ - 1);
        else if (ev.action == Action::Down)
            playlistSel_ = std::min(kPlaylistCount - 1, playlistSel_ + 1);
        else if (ev.action == Action::PageUp)
            playlistSel_ = std::max(0, playlistSel_ - playlistVisibleRows_);
        else if (ev.action == Action::PageDown)
            playlistSel_ = std::min(kPlaylistCount - 1, playlistSel_ + playlistVisibleRows_);
        else if (ev.action == Action::Accept) playlistCur_ = playlistSel_;
        break;
    case Panel::Tracker:
        if (ev.action == Action::Up) row_ = std::max(0, row_ - 1);
        else if (ev.action == Action::Down) row_ = std::min(kPatternRows - 1, row_ + 1);
        else if (ev.action == Action::Left) chan_ = std::max(0, chan_ - 1);
        else if (ev.action == Action::Right) chan_ = std::min(kChannelCount - 1, chan_ + 1);
        else if (ev.action == Action::PageUp) row_ = std::max(0, row_ - trackerVisibleRows_);
        else if (ev.action == Action::PageDown)
            row_ = std::min(kPatternRows - 1, row_ + trackerVisibleRows_);
        break;
    default: break;
    }
}

void Mockup::frame(NavInput& nav, const FrameInfo& info, double t)
{
    for (const NavEvent& ev : nav.events()) apply(ev, nav.focus());
    nav.events().clear();

    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x;
    const float H = io.DisplaySize.y;
    const float lh = ImGui::GetTextLineHeight();
    const float gap = std::max(2.0f, lh * 0.3f);

    const float transportH = lh * 3.4f;
    const float bodyY = transportH + 2 * gap;
    const float bodyH = H - bodyY - gap;
    const float leftW = std::clamp(W * 0.32f, lh * 14.0f, lh * 30.0f);
    const float rightX = leftW + 2 * gap;
    const float rightW = W - rightX - gap;
    const float playlistH = bodyH * 0.55f - gap * 0.5f;
    const float infoH = bodyH - playlistH - gap;
    const float scopeH = std::clamp(bodyH * 0.25f, lh * 5.0f, lh * 12.0f);
    const float trackerH = bodyH - scopeH - gap;

    drawTransport(nav, info, gap, gap, W - 2 * gap, transportH);
    drawPlaylist(nav, gap, bodyY, leftW, playlistH);
    drawInfo(gap, bodyY + playlistH + gap, leftW, infoH);
    drawTracker(nav, rightX, bodyY, rightW, trackerH);
    drawScope(rightX, bodyY + trackerH + gap, rightW, scopeH, t);
}

void Mockup::drawTransport(NavInput& nav, const FrameInfo& info, float x, float y, float w, float h)
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

    // Przyciski: ikony rysowane ksztaltami (bez zaleznosci od glifow fontu).
    const float bs = std::min(h - bar - 6.0f * dpi, lh * 1.7f);
    const float by = wp.y + bar + (h - bar - bs) * 0.5f;
    float bx = wp.x + 8.0f * dpi;
    const char* labels[kTransportButtons] = {tr(Str::BtnPrev), tr(Str::BtnPlay), tr(Str::BtnPause),
                                             tr(Str::BtnStop), tr(Str::BtnNext)};
    for (int i = 0; i < kTransportButtons; ++i) {
        const ImVec2 a(bx, by), b(bx + bs, by + bs);
        const bool sel = focused && i == transportSel_;
        const bool active = (i == 1 && playing_) || (i == 2 && !playing_);
        dl->AddRectFilled(a, b, col32(active ? withAlpha(p.accent, 0.25f) : p.panelHeader));
        dl->AddRect(a, b, col32(sel ? p.focusBorder : p.border), 0.0f, 0, sel ? 2.0f * dpi : 1.0f);
        const ImU32 ic = col32(active ? p.accent : p.text);
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
        default:
            dl->AddRectFilled(ImVec2(cx + r - r * 0.35f, cy - r), ImVec2(cx + r, cy + r), ic);
            dl->AddTriangleFilled(ImVec2(cx - r, cy - r), ImVec2(cx - r, cy + r), ImVec2(cx + r * 0.5f, cy), ic);
            break;
        }
        // Mysz: klik w przycisk.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(a, b)) {
            nav.setFocus(Panel::Transport);
            transportSel_ = i;
            NavEvent ev{Action::Accept, false};
            apply(ev, Panel::Transport);
        }
        if (sel) {
            // Podpis aktualnego przycisku (czytelnosc bez znajomosci ikon).
            dl->AddText(ImVec2(wp.x + 8.0f * dpi + (bs + 6.0f * dpi) * kTransportButtons + 4.0f * dpi,
                               by + (bs - lh) * 0.5f),
                        col32(p.accent), labels[i]);
        }
        bx += bs + 6.0f * dpi;
    }

    // Czas, tytul i pasek postepu (makieta).
    char buf[160];
    const int sec = static_cast<int>(std::fmod(ImGui::GetTime(), 134.0));
    std::snprintf(buf, sizeof buf, "%d:%02d / 2:14   %s", sec / 60, sec % 60,
                  kPlaylist[playlistCur_].title);
    const float tx = bx + lh * 9.0f;
    dl->AddText(ImVec2(tx, by + (bs - lh) * 0.5f), col32(p.text), buf);
    const float py = wp.y + h - 5.0f * dpi;
    const float pw = w - 16.0f * dpi;
    dl->AddRectFilled(ImVec2(wp.x + 8.0f * dpi, py), ImVec2(wp.x + 8.0f * dpi + pw, py + 2.0f * dpi),
                      col32(p.border));
    dl->AddRectFilled(ImVec2(wp.x + 8.0f * dpi, py),
                      ImVec2(wp.x + 8.0f * dpi + pw * (sec / 134.0f), py + 2.0f * dpi), col32(p.accent));

    // Statystyki renderowania (diagnostyka prototypu), przy prawej krawedzi.
    if (info.idle) std::snprintf(buf, sizeof buf, "%s %.1f [%s]", tr(Str::Fps), info.fps, tr(Str::Idle));
    else std::snprintf(buf, sizeof buf, "%s %.0f", tr(Str::Fps), info.fps);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(wp.x + w - ts.x - 8.0f * dpi, wp.y + 2.0f * dpi), col32(p.textDim), buf);

    ImGui::End();
}

void Mockup::drawPlaylist(NavInput& nav, float x, float y, float w, float h)
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

    playlistVisibleRows_ = std::max(1, static_cast<int>((h - bar - 4.0f * dpi) / rowH));
    if (ImGui::IsWindowHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) playlistSel_ = std::clamp(playlistSel_ - static_cast<int>(wheel * 3), 0, kPlaylistCount - 1);
    }
    playlistTop_ = std::clamp(playlistTop_, std::max(0, playlistSel_ - playlistVisibleRows_ + 1),
                              playlistSel_);

    for (int i = 0; i < playlistVisibleRows_; ++i) {
        const int idx = playlistTop_ + i;
        if (idx >= kPlaylistCount) break;
        const float ry = wp.y + bar + 2.0f * dpi + i * rowH;
        const ImVec2 a(wp.x + 2.0f * dpi, ry), b(wp.x + w - 2.0f * dpi, ry + rowH);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(a, b)) {
            nav.setFocus(Panel::Playlist);
            playlistSel_ = idx;
        }
        if (idx == playlistSel_)
            dl->AddRectFilled(a, b, col32(focused ? p.cursorCell : p.gridBar));
        else if (idx % 2) dl->AddRectFilled(a, b, col32(p.gridBeat));
        const bool cur = idx == playlistCur_;
        const ImU32 tc = col32(idx == playlistSel_ && focused ? p.cursorText : (cur ? p.accent : p.text));
        char num[16];
        std::snprintf(num, sizeof num, "%s%02d", cur ? ">" : " ", idx + 1);
        dl->AddText(ImVec2(a.x + 4.0f * dpi, ry + dpi), tc, num);
        const float tx = a.x + 4.0f * dpi + lh * 2.2f;
        const float lenW = lh * 2.6f;
        dl->PushClipRect(ImVec2(tx, ry), ImVec2(b.x - lenW, b.y), true);
        dl->AddText(ImVec2(tx, ry + dpi), tc, kPlaylist[idx].title);
        dl->PopClipRect();
        dl->AddText(ImVec2(b.x - lenW + 4.0f * dpi, ry + dpi), tc, kPlaylist[idx].length);
    }
    ImGui::End();
}

void Mockup::drawInfo(float x, float y, float w, float h)
{
    const Palette& p = palette();
    if (!beginPanel("##info", x, y, w, h)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bar = drawFrame(dl, tr(Str::PanelInfo), false);
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 wp = ImGui::GetWindowPos();

    struct Field { Str label; const char* value; };
    const Field fields[] = {
        {Str::GdTitle, "Green Hill Zone - Act 1"},
        {Str::GdTitleJp, "グリーンヒルゾーン 1面"},
        {Str::GdGame, "Sonic the Hedgehog"},
        {Str::GdSystem, "Sega Mega Drive / Genesis"},
        {Str::GdAuthor, "Masato Nakamura"},
        {Str::GdDate, "1991-06-23"},
        {Str::GdRipper, "sample"},
        {Str::GdNotes, kChipLine},
    };
    const float labelW = lh * 6.5f;
    float ry = wp.y + bar + 4.0f * dpi;
    for (const Field& f : fields) {
        if (ry + lh > wp.y + h) break;
        dl->AddText(ImVec2(wp.x + 6.0f * dpi, ry), col32(p.textDim), tr(f.label));
        dl->PushClipRect(ImVec2(wp.x + 6.0f * dpi + labelW, ry), ImVec2(wp.x + w - 4.0f * dpi, ry + lh + 2.0f * dpi), true);
        dl->AddText(ImVec2(wp.x + 6.0f * dpi + labelW, ry), col32(p.text), f.value);
        dl->PopClipRect();
        ry += lh + 3.0f * dpi;
    }
    ImGui::End();
}

void Mockup::drawTracker(NavInput& nav, float x, float y, float w, float h)
{
    const Palette& p = palette();
    if (!beginPanel("##tracker", x, y, w, h)) { ImGui::End(); return; }
    if (clickedInside()) nav.setFocus(Panel::Tracker);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool focused = nav.focus() == Panel::Tracker;
    char title[64];
    std::snprintf(title, sizeof title, "%s  %02X/%02X", tr(Str::PanelTracker), row_, kPatternRows - 1);
    const float bar = drawFrame(dl, title, focused);
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float lh = ImGui::GetTextLineHeight();
    const float cw = ImGui::CalcTextSize("0").x;
    const ImVec2 wp = ImGui::GetWindowPos();

    if (ImGui::IsWindowHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) row_ = std::clamp(row_ - static_cast<int>(wheel * 3), 0, kPatternRows - 1);
    }

    const float rowH = lh + 2.0f * dpi;
    const float headH = rowH + 2.0f * dpi;
    const float numW = cw * 4.0f;
    const float chanW = cw * 10.0f;
    const float left = wp.x + 4.0f * dpi;
    const float top = wp.y + bar + 2.0f * dpi;

    trackerVisibleRows_ = std::max(1, static_cast<int>((h - bar - headH - 6.0f * dpi) / rowH));
    const int visChans = std::clamp(static_cast<int>((w - numW - 8.0f * dpi) / chanW), 1, kChannelCount);
    if (chan_ < firstChan_) firstChan_ = chan_;
    if (chan_ >= firstChan_ + visChans) firstChan_ = chan_ - visChans + 1;
    firstChan_ = std::clamp(firstChan_, 0, kChannelCount - visChans);

    // Kursor pozostaje w srodku widoku, gdy tylko to mozliwe.
    const int firstRow = std::clamp(row_ - trackerVisibleRows_ / 2, 0,
                                    std::max(0, kPatternRows - trackerVisibleRows_));

    // Naglowki kanalow.
    for (int c = 0; c < visChans; ++c) {
        const int ch = firstChan_ + c;
        char hd[16];
        std::snprintf(hd, sizeof hd, "CH%d", ch + 1);
        const float cx = left + numW + c * chanW;
        dl->AddRectFilled(ImVec2(cx, top), ImVec2(cx + chanW - 2.0f * dpi, top + headH - 2.0f * dpi),
                          col32(withAlpha(p.channel[ch], ch == chan_ ? 0.35f : 0.15f)));
        dl->AddText(ImVec2(cx + cw, top + dpi), col32(p.channel[ch]), hd);
    }

    const float gy = top + headH;
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
            const ImU32 nc = cursor ? col32(p.cursorText) : col32(note < 0 ? p.textDim : p.channel[ch]);
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

void Mockup::drawScope(float x, float y, float w, float h, double t)
{
    const Palette& p = palette();
    if (!beginPanel("##scope", x, y, w, h)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bar = drawFrame(dl, tr(Str::PanelScope), false);
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 wp = ImGui::GetWindowPos();

    const float ax = wp.x + 4.0f * dpi, ay = wp.y + bar + 4.0f * dpi;
    const float aw = w - 8.0f * dpi, ah = h - bar - 8.0f * dpi;
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

    // Syntetyczny "miks": suma sinusow o roznych czestotliwosciach i fazach.
    static std::vector<ImVec2> pts;
    const int n = std::clamp(static_cast<int>(aw / (2.0f * dpi)), 64, 1024);
    pts.resize(static_cast<size_t>(n));
    const float tt = playing_ ? static_cast<float>(t) : 0.0f;
    for (int i = 0; i < n; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(n - 1);
        float v = 0.45f * std::sin(2.0f * kPi * (3.0f * u + tt * 1.1f)) +
                  0.25f * std::sin(2.0f * kPi * (7.0f * u - tt * 1.7f)) +
                  0.15f * std::sin(2.0f * kPi * (13.0f * u + tt * 2.3f));
        pts[static_cast<size_t>(i)] = ImVec2(ax + aw * u, cy - v * ah * 0.5f * 0.95f);
    }
    dl->PushClipRect(ImVec2(ax, ay), ImVec2(ax + aw, ay + ah), true);
    dl->AddPolyline(pts.data(), n, col32(p.scopeLine), ImDrawFlags_None, 1.5f * dpi);
    dl->PopClipRect();
    ImGui::End();
}

} // namespace vgmplay::ui
