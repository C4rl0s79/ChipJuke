// Glowny ekran odtwarzacza (E1): transport, panel playlisty (jeden plik), GD3/info, siatka
// trackera (makieta z prawdziwa lista kanalow) i oscyloskop. Czyta PlayerView, steruje wylacznie
// przez PlayerActions; nie zna silnika ani watkow.
#pragma once

#include "vgmplay_ui/nav_input.h"
#include "vgmplay_ui/player_view.h"

#include <vector>

namespace vgmplay::ui {

struct FrameInfo {
    float fps = 0.0f;   // zmierzone renderowane klatki/s
    bool idle = false;  // petla jest w trybie bezczynnosci
};

class PlayerScreen {
public:
    // Wolac miedzy ImGui::NewFrame() a ImGui::Render(). Zuzywa nav.events().
    void frame(NavInput& nav, const PlayerView& view, PlayerActions& actions, const FrameInfo& info);

    // true => potrzebne ciagle klatki (odtwarzanie, ladowanie, przeciaganie suwaka).
    bool wantsAnimation(const PlayerView& view) const;

    bool diagnosticsVisible() const { return diag_; }
    void setDiagnosticsVisible(bool v) { diag_ = v; }
    void setTagsJapanese(bool jp) { gd3Jp_ = jp; }

private:
    void apply(const NavEvent& ev, Panel focus, const PlayerView& view, PlayerActions& actions);
    void drawTransport(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x, float y,
                       float w, float h);
    void drawPlaylist(NavInput& nav, const PlayerView& view, float x, float y, float w, float h);
    void drawInfo(const PlayerView& view, float x, float y, float w, float h);
    void drawTracker(NavInput& nav, const PlayerView& view, float x, float y, float w, float h);
    void drawScope(const PlayerView& view, float x, float y, float w, float h);
    void drawStatus(const PlayerView& view, PlayerActions& actions, float x, float y, float w, float h);
    void drawDiagnostics(const PlayerView& view, const FrameInfo& info, float x, float y, float w);

    bool gd3Jp_ = false;
    bool diag_ = false;
    bool seekDrag_ = false;
    double seekFraction_ = 0.0;
    bool volumeDrag_ = false;
    static constexpr int kSeekHoldFrames = 12; // klatki ciaglego rysowania po seeku (czekamy na silnik)
    int seekHold_ = 0;
    int row_ = 0;
    int chan_ = 0;
    int firstChan_ = 0;
    int trackerVisibleRows_ = 16;
    int infoScroll_ = 0;
    std::vector<float> scopeL_, scopeR_;
};

} // namespace vgmplay::ui
