// Glowny ekran odtwarzacza (E1/E1.5): transport, panel playlisty (jeden plik), GD3/info, panel
// kanalow w ukladzie FastTrackera 2 (pasek oscyloskopow kanalow NA GORZE, pod nim ZAWSZE wzorzec
// z kursorem odtwarzania w srodku, kolumny wyrownane z ramkami oscyloskopow) i oscyloskop ogolny
// L/R z VU mastera. Czyta PlayerView, steruje wylacznie przez PlayerActions; nie zna silnika ani
// watkow.
#pragma once

#include "vgmplay_ui/channel_scopes.h"
#include "vgmplay_ui/nav_input.h"
#include "vgmplay_ui/pattern_view.h"
#include "vgmplay_ui/player_view.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace vgmplay::ui {

struct FrameInfo {
    float fps = 0.0f;   // zmierzone renderowane klatki/s
    bool idle = false;  // petla jest w trybie bezczynnosci
};

// Wysokosc paska oscyloskopow kanalow nad wzorcem (F4 przechodzi Small -> Large -> Hidden).
// Hidden: brak paska i VU kanalow, silnik nie liczy strumieni kanalow.
enum class ScopeStrip { Small, Large, Hidden };

class PlayerScreen {
public:
    // Wolac miedzy ImGui::NewFrame() a ImGui::Render(). Zuzywa nav.events().
    void frame(NavInput& nav, const PlayerView& view, PlayerActions& actions, const FrameInfo& info);

    // true => potrzebne ciagle klatki (odtwarzanie, ladowanie, przeciaganie suwaka).
    bool wantsAnimation(const PlayerView& view) const;

    // --- Playlista / okna (E2); akcesory glownie dla testow ---
    enum class DlgKind { None, NewList, RenameList, DeleteList, DeleteEntry, EntryMenu, Report };
    DlgKind dialog() const { return dlg_.kind; }
    bool settingsOpen() const { return settingsOpen_; }
    void openSettings() { settingsOpen_ = true; setSel_ = 0; }
    long long playlistSelection() const { return plSel_; }
    bool modalOpen() const { return dlg_.kind != DlgKind::None || settingsOpen_; }

    bool diagnosticsVisible() const { return diag_; }
    void setDiagnosticsVisible(bool v) { diag_ = v; }
    void setTagsJapanese(bool jp) { gd3Jp_ = jp; }
    ScopeStrip scopeStrip() const { return strip_; }
    void setScopeStrip(ScopeStrip s) { strip_ = s; }
    // Wiersz wzorca pod kursorem odtwarzania w ostatniej narysowanej klatce (testy); -1 = brak wzorca.
    long long currentRow() const { return currentRow_; }
    // Liczba kolumn widocznych w ostatniej klatce (testy) i pierwsza widoczna.
    int visibleColumns() const { return visCols_; }
    int firstColumn() const { return firstChan_; }

private:
    void cycleStrip()
    {
        strip_ = strip_ == ScopeStrip::Small ? ScopeStrip::Large
                 : strip_ == ScopeStrip::Large ? ScopeStrip::Hidden : ScopeStrip::Small;
    }
    void apply(const NavEvent& ev, Panel focus, const PlayerView& view, PlayerActions& actions);
    void drawTransport(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x, float y,
                       float w, float h);
    void drawPlaylist(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x, float y,
                      float w, float h);
    void applyPlaylist(const NavEvent& ev, const PlayerView& view, PlayerActions& actions);
    void applyModal(const NavEvent& ev, const PlayerView& view, PlayerActions& actions);
    void drawModal(const PlayerView& view, PlayerActions& actions, float W, float H);
    void drawSettings(const PlayerView& view, PlayerActions& actions, float W, float H);
    void drawDialog(const PlayerView& view, PlayerActions& actions, float W, float H);
    void changeSetting(int row, int dir, const PlayerView& view, PlayerActions& actions);
    void openDialog(DlgKind k, std::size_t index = 0, std::string text = {});
    void closeDialog();
    void confirmDialog(const PlayerView& view, PlayerActions& actions);
    [[nodiscard]] bool jp(const PlayerView& v) const
    {
        return v.settings ? v.settings->ui.gd3Language == format::Gd3Language::Jp : gd3Jp_;
    }
    void toggleLanguage(const PlayerView& view, PlayerActions& actions);
    void toggleGd3(const PlayerView& view, PlayerActions& actions);
    void drawInfo(const PlayerView& view, PlayerActions& actions, float x, float y, float w, float h);
    void drawTracker(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x, float y,
                     float w, float h);
    void drawChannelToolbar(NavInput& nav, const PlayerView& view, PlayerActions& actions, float x, float y,
                            float w, float h, bool focused);
    void updateMeters(const PlayerView& view, float dtSeconds);
    void drawScope(const PlayerView& view, float x, float y, float w, float h);
    void drawStatus(const PlayerView& view, PlayerActions& actions, float x, float y, float w, float h);
    void drawDiagnostics(const PlayerView& view, const FrameInfo& info, float x, float y, float w);

    struct Dialog {
        DlgKind kind = DlgKind::None;
        int sel = 0;
        std::size_t index = 0;
        std::string text;
        bool focusText = false;
        int age = 0;
    };
    Dialog dlg_;
    bool settingsOpen_ = false;
    int setSel_ = 0;
    std::uint64_t seenNotice_ = 0;
    long long plSel_ = 0;
    long long plLastCursor_ = -2;
    std::size_t plLastList_ = static_cast<std::size_t>(-1);
    int plScroll_ = 0;
    int plVisible_ = 1;
    int dragFrom_ = -1;
    bool dragging_ = false;
    ImVec2 dragStart_;

    bool gd3Jp_ = false;
    bool diag_ = false;
    bool seekDrag_ = false;
    double seekFraction_ = 0.0;
    bool volumeDrag_ = false;
    static constexpr int kSeekHoldFrames = 12; // klatki ciaglego rysowania po seeku (czekamy na silnik)
    int seekHold_ = 0;
    int chan_ = 0;
    int firstChan_ = 0;
    int visCols_ = 0;
    long long currentRow_ = -1;
    int infoScroll_ = 0;
    std::vector<float> scopeL_, scopeR_;
    ScopeStrip strip_ = ScopeStrip::Small;
    VuBank vu_;
    std::vector<float> vuTargets_, wave_;
    std::vector<ScopeAutoGain> autoGain_;      // per kanal; zerowane przy nowym utworze
    const void* gainTrack_ = nullptr;
    std::vector<int> columnMap_;               // kanal globalny -> kolumna wzorca (-1 = n/a)
    std::vector<ImVec2> wavePts_;
};

} // namespace vgmplay::ui
