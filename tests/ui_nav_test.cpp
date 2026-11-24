// Testy E3 (ImGui bez backendu): mapa fokusu paneli i okien obslugiwana samym padem, NavInput na InputRouter,
// ekran przypisan (przechwytywanie, konflikty, minimalny zestaw), podpowiedzi pada.
#include <doctest/doctest.h>

#include <imgui.h>

#include <string>
#include <vector>

#include "vgmplay_input/bindings_edit.hpp"
#include "vgmplay_ui/nav_input.h"
#include "vgmplay_ui/player_screen.h"
#include "vgmplay_ui/player_view.h"
#include "vgmplay_ui/track_meta.h"

using namespace vgmplay;
using namespace vgmplay::ui;

namespace {

struct Rec final : PlayerActions {
    std::vector<std::string> log;
    settings::Settings* target = nullptr;   // symuluje kontroler: przyjete ustawienia trafiaja do view.settings
    void requestOpenFile() override { log.push_back("open"); }
    void togglePlay() override { log.push_back("toggle"); }
    void play() override {}
    void pause() override {}
    void stop() override { log.push_back("stop"); }
    void seekToFrame(std::uint64_t) override {}
    void seekBySeconds(double s) override { log.push_back(s > 0 ? "seek+" : "seek-"); }
    void setVolume(float) override { log.push_back("volume"); }
    void dismissError() override {}
    void toggleChannelMute(int) override {}
    void toggleChannelSolo(int) override {}
    void muteAllChannels() override { log.push_back("muteall"); }
    void unmuteAllChannels() override { log.push_back("unmuteall"); }
    void clearSolo() override {}
    void setChannelScopesVisible(bool) override {}
    void setPatternRowTicks(std::uint32_t) override { log.push_back("rowticks"); }
    void playEntry(std::size_t i) override { log.push_back("play" + std::to_string(i)); }
    void playNext() override { log.push_back("next"); }
    void playPrevious() override { log.push_back("prev"); }
    void removeEntry(std::size_t i) override { log.push_back("remove" + std::to_string(i)); }
    void moveEntry(std::size_t, std::size_t) override {}
    void shufflePlaylist() override { log.push_back("shuffle"); }
    void setRepeat(playlist::RepeatMode m) override { log.push_back("repeat" + std::to_string(static_cast<int>(m))); }
    void selectPlaylist(std::size_t i) override { log.push_back("select" + std::to_string(i)); }
    void createPlaylist(const std::string&) override {}
    void renamePlaylist(std::size_t, const std::string&) override {}
    void deletePlaylist(std::size_t i) override { log.push_back("delete" + std::to_string(i)); }
    void requestAddFolder() override { log.push_back("folder"); }
    void applySettings(const settings::Settings& s) override
    {
        if (target) *target = s;
        log.push_back("settings");
    }
    void refreshAudioDevices() override { log.push_back("refresh"); }
    [[nodiscard]] bool has(const std::string& s) const
    {
        for (const std::string& l : log)
            if (l == s) return true;
        return false;
    }
};

struct Headless {
    Headless()
    {
        ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    ~Headless() { ImGui::DestroyContext(ctx); }
    ImGuiContext* ctx;
};

SDL_Event keyEv(SDL_Scancode sc, bool down = true)
{
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    return e;
}

SDL_Event padEv(Uint8 button, bool down = true)
{
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = 3;
    e.gbutton.button = button;
    return e;
}

struct Fixture {
    Headless g;
    playlist::PlaylistModel model;
    TrackMetaStore meta;
    settings::Settings settings;
    PlayerView view;
    Rec act;
    PlayerScreen screen;
    NavInput nav;
    std::uint64_t now = 1000;

    Fixture()
    {
        REQUIRE(model.addPlaylist("Jeden").ok());
        REQUIRE(model.addPlaylist("Dwa").ok());
        (void)model.setActive(0);
        std::vector<playlist::Entry> e(3);
        e[0].path = "C:/m/a.vgm"; e[1].path = "C:/m/b.vgm"; e[2].path = "C:/m/c.vgm";
        REQUIRE(model.insert(0, 0, e).ok());
        view.playlists = &model;
        view.meta = &meta;
        view.settings = &settings;
        act.target = &settings;
        run();
    }
    void run()
    {
        ImGui::NewFrame();
        screen.frame(nav, view, act, {});
        ImGui::Render();
    }
    void key(Action a)
    {
        nav.events().push_back({a, false});
        run();
    }
    void keys(std::initializer_list<Action> l)
    {
        for (Action a : l) key(a);
    }
    // Pelna sciezka SDL -> router -> NavEvent -> panel (wcisniecie + puszczenie).
    void tapKey(SDL_Scancode sc)
    {
        nav.handleEvent(keyEv(sc, true), now);
        run();
        nav.handleEvent(keyEv(sc, false), now + 10);
        run();
        now += 100;
    }
    void tapPad(Uint8 b)
    {
        nav.handleEvent(padEv(b, true), now);
        run();
        nav.handleEvent(padEv(b, false), now + 10);
        run();
        now += 100;
    }
};

} // namespace

TEST_SUITE("ui/nav")
{
    TEST_CASE("NavInput: LB/RB i Tab/Shift+Tab przechodza po czterech panelach, Info wlacznie")
    {
        Fixture f;
        CHECK(f.nav.focus() == Panel::Transport);
        f.tapPad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        CHECK(f.nav.focus() == Panel::Playlist);
        f.tapPad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        CHECK(f.nav.focus() == Panel::Info);
        f.tapPad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        CHECK(f.nav.focus() == Panel::Tracker);
        f.tapPad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        CHECK(f.nav.focus() == Panel::Transport);
        f.tapPad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        CHECK(f.nav.focus() == Panel::Tracker);
        f.tapKey(SDL_SCANCODE_TAB);
        CHECK(f.nav.focus() == Panel::Transport);
        SDL_Event shiftTab = keyEv(SDL_SCANCODE_TAB);
        shiftTab.key.mod = SDL_KMOD_LSHIFT;
        f.nav.handleEvent(shiftTab, f.now);
        CHECK(f.nav.focus() == Panel::Tracker);
    }

    TEST_CASE("NavInput: zmiana panelu w trakcie trzymania kierunku zatrzaskuje repeat; puszczenie nie ginie")
    {
        Fixture f;
        f.nav.setFocus(Panel::Playlist);
        f.nav.handleEvent(padEv(SDL_GAMEPAD_BUTTON_DPAD_DOWN), 0);
        REQUIRE(f.nav.events().size() == 1);
        CHECK(f.nav.events()[0].screen == input::ScreenId::Playlists);
        f.nav.events().clear();
        f.nav.handleEvent(padEv(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), 10);   // inny panel (Info), D-pad trzymany
        f.nav.update(2000);
        CHECK(f.nav.events().empty());                                       // brak repeatu na nowym ekranie
        f.nav.handleEvent(padEv(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false), 2010);
        f.nav.handleEvent(padEv(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false), 2010);
        CHECK_FALSE(f.nav.anyHeld());
        f.nav.handleEvent(padEv(SDL_GAMEPAD_BUTTON_DPAD_DOWN), 3000);       // nowe nacisniecie nalezy do Info
        REQUIRE(f.nav.events().size() == 1);
        CHECK(f.nav.events()[0].screen == input::ScreenId::Info);
        f.nav.events().clear();
        f.nav.update(3000 + 400);
        REQUIRE(f.nav.events().size() == 1);
        CHECK(f.nav.events()[0].repeat);
    }

    TEST_CASE("NavInput: utrata fokusu okna czysci trzymane, pad w tle nie dziala, ostatnie wejscie (pad/klawiatura)")
    {
        Fixture f;
        f.nav.handleEvent(padEv(SDL_GAMEPAD_BUTTON_DPAD_UP), 0);
        CHECK(f.nav.lastInputWasPad());
        CHECK(f.nav.anyHeld());
        SDL_Event lost{};
        lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        f.nav.handleEvent(lost, 50);
        CHECK_FALSE(f.nav.anyHeld());
        f.nav.handleEvent(padEv(SDL_GAMEPAD_BUTTON_SOUTH), 60);
        f.nav.update(5000);
        CHECK(f.nav.events().empty());
        SDL_Event gained{};
        gained.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        f.nav.handleEvent(gained, 100);
        f.nav.handleEvent(keyEv(SDL_SCANCODE_DOWN), 110);
        CHECK_FALSE(f.nav.lastInputWasPad());
    }

    TEST_CASE("Transport: pad dosiega przyciski, pasek pozycji i glosnosc; Open oznaczony jako wymagajacy klawiatury")
    {
        Fixture f;
        f.nav.setFocus(Panel::Transport);
        f.run();
        CHECK(f.screen.focusRow(Panel::Transport) == 1);          // domyslnie pasek pozycji (jak dawne Lewo/Prawo)
        f.key(Action::Right);
        CHECK(f.act.has("seek+"));
        f.key(Action::Left);
        CHECK(f.act.has("seek-"));
        f.key(Action::Confirm);
        CHECK(f.act.has("toggle"));
        f.key(Action::Down);                                       // glosnosc
        CHECK(f.screen.focusRow(Panel::Transport) == 2);
        f.key(Action::Left);
        CHECK(f.act.has("volume"));
        f.keys({Action::Up, Action::Up});                          // przyciski
        CHECK(f.screen.focusRow(Panel::Transport) == 0);
        f.key(Action::Confirm);                                    // Prev (kolumna 0)
        CHECK(f.act.has("prev"));
        for (int i = 0; i < 4; ++i) f.key(Action::Right);
        f.key(Action::Confirm);                                    // Next
        CHECK(f.act.has("next"));
        f.key(Action::Right);                                      // Open
        CHECK(f.screen.focusCol(Panel::Transport) == 5);
        CHECK(f.screen.focusNeedsKeyboard());
        f.key(Action::Right);                                      // brak pulapki: kursor zostaje na koncu
        CHECK(f.screen.focusCol(Panel::Transport) == 5);
        f.key(Action::Left);
        CHECK_FALSE(f.screen.focusNeedsKeyboard());
    }

    TEST_CASE("Playlista: z listy w gore do przyciskow, kazdy przycisk dostepny padem, Down wraca do listy")
    {
        Fixture f;
        f.nav.setFocus(Panel::Playlist);
        f.run();
        CHECK(f.screen.focusRow(Panel::Playlist) == 2);
        f.key(Action::Confirm);
        CHECK(f.act.has("play0"));
        f.key(Action::Up);                                         // sel 0 -> wiersz przyciskow 1
        CHECK(f.screen.focusRow(Panel::Playlist) == 1);
        f.key(Action::Confirm);                                    // Losuj
        CHECK(f.act.has("shuffle"));
        f.key(Action::Right);
        f.key(Action::Confirm);                                    // Powtarzanie
        CHECK(f.act.has("repeat2"));
        f.key(Action::Right);                                      // Dodaj pliki: okno systemowe
        CHECK(f.screen.focusNeedsKeyboard());
        f.key(Action::Right);
        f.key(Action::Confirm);                                    // Dodaj folder (wywola zadanie, okno zalezy od aplikacji)
        CHECK(f.act.has("folder"));
        f.key(Action::Up);                                         // wiersz 0: lista playlist
        CHECK(f.screen.focusRow(Panel::Playlist) == 0);
        f.keys({Action::Left, Action::Left});                      // kolumna 3 -> 1: ">"
        f.key(Action::Confirm);
        CHECK(f.act.has("select1"));
        f.keys({Action::Down, Action::Down});
        CHECK(f.screen.focusRow(Panel::Playlist) == 2);
        f.key(Action::Menu);                                       // menu wpisu
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::EntryMenu);
        f.key(Action::Back);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
    }

    TEST_CASE("Info: pad przelacza jezyk tagow GD3 i przewija")
    {
        Fixture f;
        f.nav.setFocus(Panel::Info);
        f.run();
        f.key(Action::Confirm);
        REQUIRE(f.act.has("settings"));
        CHECK(f.settings.ui.gd3Language == format::Gd3Language::Jp);
        f.key(Action::Down);   // przewijanie nie wiesza
        f.key(Action::Up);
    }

    TEST_CASE("Tracker: pasek narzedzi osiagalny padem; zmiana wysokosci paska jest zapamietywana w ustawieniach")
    {
        Fixture f;
        f.nav.setFocus(Panel::Tracker);
        f.run();
        CHECK(f.screen.focusRow(Panel::Tracker) == 1);
        f.key(Action::Up);
        CHECK(f.screen.focusRow(Panel::Tracker) == 0);
        f.key(Action::Confirm);                                    // kolumna 0: Oscyloskopy
        CHECK(f.screen.scopeStrip() == ScopeStrip::Large);
        CHECK(f.settings.ui.scopeBar == settings::ScopeBar::Large);
        f.key(Action::Right);
        f.key(Action::Down);
        CHECK(f.screen.focusRow(Panel::Tracker) == 1);
    }

    TEST_CASE("Dialog potwierdzenia: Lewo/Prawo wybiera Nie/Tak, B zamyka; brak pulapki")
    {
        Fixture f;
        f.nav.setFocus(Panel::Playlist);
        f.key(Action::DeletePlaylist);
        REQUIRE(f.screen.dialog() == PlayerScreen::DlgKind::DeleteList);
        f.key(Action::Left);                                       // Nie
        f.key(Action::Confirm);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
        CHECK_FALSE(f.act.has("delete0"));
        f.key(Action::DeletePlaylist);
        f.key(Action::Left);
        f.key(Action::Right);                                      // Tak
        f.key(Action::Confirm);
        CHECK(f.act.has("delete0"));
        f.key(Action::DeletePlaylist);
        f.key(Action::Back);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
    }

    TEST_CASE("Ustawienia: urzadzenie audio, sciezki sieciowe, podpowiedzi - pad; B zamyka")
    {
        Fixture f;
        f.view.audioDevices = {{"id-1", "Glosniki", true}, {"id-2", "Sluchawki", false}};
        f.view.audioDevicesLoaded = true;
        f.key(Action::ToggleSettings);
        REQUIRE(f.screen.settingsOpen());
        CHECK(f.act.has("refresh"));                               // lista odswiezana przy otwarciu
        CHECK(f.nav.overlayActive());
        f.key(Action::Right);                                      // wiersz 0: pierwsze urzadzenie z listy
        CHECK(f.settings.audio.device == "id-1");
        f.key(Action::Right);
        CHECK(f.settings.audio.device == "id-2");
        f.key(Action::Right);                                      // zawija do domyslnego
        CHECK(f.settings.audio.device.empty());
        f.key(Action::Left);
        CHECK(f.settings.audio.device == "id-2");
        for (int i = 0; i < 8; ++i) f.key(Action::Down);
        CHECK(f.screen.settingsSelection() == 8);
        // [E3-sec] WLACZENIE tylko przez Confirm + dialog potwierdzenia; strzalki nie wlaczaja.
        f.key(Action::Right);
        f.key(Action::Left);
        CHECK_FALSE(f.settings.library.allowNetworkPaths);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
        f.key(Action::Confirm);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::NetworkConfirm);
        CHECK_FALSE(f.settings.library.allowNetworkPaths);
        f.key(Action::Confirm);                                    // domyslnie "Nie": zamyka bez zmiany
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
        CHECK_FALSE(f.settings.library.allowNetworkPaths);
        CHECK(f.screen.settingsOpen());
        f.key(Action::Confirm);
        f.key(Action::Back);                                       // Esc/B anuluje
        CHECK_FALSE(f.settings.library.allowNetworkPaths);
        f.key(Action::Confirm);
        f.key(Action::Right);                                      // "Tak"
        f.key(Action::Confirm);
        CHECK(f.settings.library.allowNetworkPaths);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
        f.key(Action::Left);                                       // wylaczenie bez potwierdzenia
        CHECK_FALSE(f.settings.library.allowNetworkPaths);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
        f.key(Action::Down);
        const bool hints = f.settings.ui.showButtonHints;
        f.key(Action::Confirm);
        CHECK(f.settings.ui.showButtonHints == !hints);
        f.key(Action::Back);
        CHECK_FALSE(f.screen.settingsOpen());
        CHECK_FALSE(f.nav.overlayActive());
    }

    TEST_CASE("Przypisania: przechwycenie klawisza, konflikt z zamiana, anulowanie i limit czasu")
    {
        Fixture f;
        f.key(Action::ToggleSettings);
        for (int i = 0; i < 10; ++i) f.key(Action::Down);
        f.key(Action::Confirm);                                    // wiersz "Przypisania"
        REQUIRE(f.screen.bindingsOpen());
        CHECK(f.nav.overlayActive());
        // Wiersz 0 = Gora; kolumna klawiatury. Przechwyc "J".
        f.key(Action::Confirm);
        CHECK(f.nav.captureState() == CaptureState::Waiting);
        f.nav.handleEvent(keyEv(SDL_SCANCODE_J), f.now);
        f.run();
        CHECK(f.nav.captureState() == CaptureState::Idle);
        const auto kb = input::sourcesOf(f.screen.bindingsEdit(), Action::Up, input::SourceClass::Keyboard);
        REQUIRE(kb.size() == 1);
        CHECK(kb[0].code == SDL_SCANCODE_J);
        REQUIRE(f.act.has("settings"));
        CHECK_FALSE(f.settings.input.bindings.empty());            // zapisane (rozne od domyslnych)
        // Konflikt: przypisz "J" jeszcze raz do akcji "Dol" (wiersz 1) -> ostrzezenie, Confirm = zamiana.
        f.key(Action::Down);
        f.key(Action::Confirm);
        f.nav.handleEvent(keyEv(SDL_SCANCODE_J, false), f.now);
        f.nav.handleEvent(keyEv(SDL_SCANCODE_J), f.now + 1);
        f.run();
        CHECK(input::sourcesOf(f.screen.bindingsEdit(), Action::Down, input::SourceClass::Keyboard)[0].code == SDL_SCANCODE_DOWN);   // jeszcze bez zmiany
        f.key(Action::Confirm);                                    // zamien
        CHECK(input::sourcesOf(f.screen.bindingsEdit(), Action::Down, input::SourceClass::Keyboard)[0].code == SDL_SCANCODE_J);
        CHECK(input::sourcesOf(f.screen.bindingsEdit(), Action::Up, input::SourceClass::Keyboard)[0].code == SDL_SCANCODE_DOWN);
        // Anulowanie Esc.
        f.key(Action::Confirm);
        f.nav.handleEvent(keyEv(SDL_SCANCODE_ESCAPE), f.now + 2);
        f.run();
        CHECK(f.nav.captureState() == CaptureState::Idle);
        CHECK(input::sourcesOf(f.screen.bindingsEdit(), Action::Down, input::SourceClass::Keyboard)[0].code == SDL_SCANCODE_J);
        // Limit czasu.
        f.key(Action::Confirm);
        f.nav.update(f.now + 1000000);
        f.run();
        CHECK(f.nav.captureState() == CaptureState::Idle);
        // B zamyka tylko ekran przypisan, nie ustawienia.
        f.key(Action::Back);
        CHECK_FALSE(f.screen.bindingsOpen());
        CHECK(f.screen.settingsOpen());
    }

    TEST_CASE("Przypisania: pad - B przytrzymane anuluje, krotkie B przypisuje; krytycznych nie da sie oproznic")
    {
        NavInput n;
        n.beginCapture(CaptureKind::Gamepad, 1000);
        n.handleEvent(padEv(SDL_GAMEPAD_BUTTON_EAST), 1000);
        n.update(1000 + NavInput::kCaptureCancelHoldMs + 1);
        CHECK(n.captureState() == CaptureState::Cancelled);
        n.endCapture();
        n.beginCapture(CaptureKind::Gamepad, 5000);
        n.handleEvent(padEv(SDL_GAMEPAD_BUTTON_EAST), 5000);
        n.handleEvent(padEv(SDL_GAMEPAD_BUTTON_EAST, false), 5100);
        REQUIRE(n.captureState() == CaptureState::Done);
        CHECK(n.captureResult() == input::BindingSource{input::SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_EAST});
        n.endCapture();
        n.beginCapture(CaptureKind::Gamepad, 9000);
        n.handleEvent(padEv(SDL_GAMEPAD_BUTTON_NORTH), 9000);
        CHECK(n.captureResult() == input::BindingSource{input::SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_NORTH});
        n.endCapture();
        CHECK(n.events().empty());                                 // przechwytywanie nie wyciekalo do routera

        Fixture f;
        f.key(Action::ToggleSettings);
        for (int i = 0; i < 10; ++i) f.key(Action::Down);
        f.key(Action::Confirm);
        REQUIRE(f.screen.bindingsOpen());
        f.keys({Action::Down, Action::Down, Action::Down, Action::Down, Action::Down, Action::Down, Action::Down});   // wiersz 7 = Back
        f.key(Action::Right);                                      // kolumna pada
        const auto before = f.screen.bindingsEdit();
        f.key(Action::ToggleSolo);                                 // X = wyczysc
        CHECK(f.screen.bindingsEdit() == before);                  // odrzucone: Confirm musi miec pad
        f.key(Action::Back);
        f.key(Action::Back);
        CHECK_FALSE(f.screen.settingsOpen());
    }

    TEST_CASE("Przywroc domyslne: ostatni wiersz")
    {
        Fixture f;
        f.key(Action::ToggleSettings);
        for (int i = 0; i < 10; ++i) f.key(Action::Down);
        f.key(Action::Confirm);
        f.key(Action::Confirm);
        f.nav.handleEvent(keyEv(SDL_SCANCODE_J), f.now);
        f.run();
        REQUIRE_FALSE(f.settings.input.bindings.empty());
        f.key(Action::PageDown);
        for (int i = 0; i < 40; ++i) f.key(Action::Down);
        f.key(Action::Confirm);
        CHECK(f.settings.input.bindings.empty());                  // domyslne = pusty zestaw
        CHECK(f.screen.bindingsEdit() == input::defaultBindings());
    }

    TEST_CASE("Przypisania z ustawien sa stosowane przez router (klawisz J zamiast Gory)")
    {
        Fixture f;
        f.settings.input.bindings = {{Action::Down, {input::SourceKind::Keyboard, SDL_SCANCODE_J}}};
        f.run();
        f.nav.setFocus(Panel::Playlist);
        f.nav.handleEvent(keyEv(SDL_SCANCODE_J), f.now);
        REQUIRE(f.nav.events().size() == 1);
        CHECK(f.nav.events()[0].action == Action::Down);
        f.nav.events().clear();
        f.nav.handleEvent(keyEv(SDL_SCANCODE_J, false), f.now + 5);
        f.nav.handleEvent(keyEv(SDL_SCANCODE_DOWN), f.now + 10);   // domyslna Strzalka nie jest juz przypisana
        CHECK(f.nav.events().empty());
        // pad dziala mimo niepelnego zestawu (uzupelnione akcje krytyczne)
        f.nav.handleEvent(padEv(SDL_GAMEPAD_BUTTON_SOUTH), f.now + 20);
        REQUIRE(f.nav.events().size() == 1);
        CHECK(f.nav.events()[0].action == Action::Confirm);
    }

    TEST_CASE("Skroty z modyfikatorami sa lokalne: Ctrl+Gora/Dol = przenies wpis, Shift+Delete = usun playliste")
    {
        NavInput n;
        SDL_Event e = keyEv(SDL_SCANCODE_UP);
        e.key.mod = SDL_KMOD_LCTRL;
        n.handleEvent(e, 0);
        e = keyEv(SDL_SCANCODE_DELETE);
        e.key.mod = SDL_KMOD_LSHIFT;
        n.handleEvent(e, 1);
        REQUIRE(n.events().size() == 2);
        CHECK(n.events()[0].action == Action::MoveUp);
        CHECK(n.events()[1].action == Action::DeletePlaylist);
    }
}
