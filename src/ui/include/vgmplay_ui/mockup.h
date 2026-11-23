// Ekran makiety (prototyp E0): transport, playlista, GD3, siatka trackera, oscyloskop.
// Dane przykladowe, brak powiazania z logika - sluzy do sprawdzenia stosu UI.
#pragma once

#include "vgmplay_ui/nav_input.h"

namespace vgmplay::ui {

struct FrameInfo {
    float fps = 0.0f;   // zmierzone renderowane klatki/s
    bool idle = false;  // petla jest w trybie bezczynnosci
};

class Mockup {
public:
    // Wolac miedzy ImGui::NewFrame() a ImGui::Render(). Zuzywa nav.events().
    void frame(NavInput& nav, const FrameInfo& info, double timeSec);

    // true => potrzebne ciagle klatki (animacja oscyloskopu).
    bool wantsAnimation() const { return playing_; }
    void setPlaying(bool p) { playing_ = p; }

private:
    void apply(const NavEvent& ev, Panel focus);
    void drawTransport(NavInput& nav, const FrameInfo& info, float x, float y, float w, float h);
    void drawPlaylist(NavInput& nav, float x, float y, float w, float h);
    void drawInfo(float x, float y, float w, float h);
    void drawTracker(NavInput& nav, float x, float y, float w, float h);
    void drawScope(float x, float y, float w, float h, double t);

    bool playing_ = true;
    int transportSel_ = 1;   // 0 prev, 1 play, 2 pause, 3 stop, 4 next
    int playlistSel_ = 0;
    int playlistCur_ = 0;
    int row_ = 0;
    int chan_ = 0;
    int firstChan_ = 0;
    int trackerVisibleRows_ = 16;
    int playlistVisibleRows_ = 8;
    int playlistTop_ = 0;
};

} // namespace vgmplay::ui
