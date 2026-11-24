// Testy modulu src/input: router na skryptach zdarzen (zasady 1-6 z include/vgmplay/input.hpp),
// nazwy akcji, domyslne wiazania i logika edycji wiazan (konflikty, minimalny zestaw).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "vgmplay/input.hpp"
#include "vgmplay_input/bindings_edit.hpp"

using namespace vgmplay::input;

namespace {

constexpr BindingSource kKeyUp{SourceKind::Keyboard, 82};
constexpr BindingSource kKeyEnter{SourceKind::Keyboard, 40};
constexpr BindingSource kPadA{SourceKind::GamepadButton, 0};
constexpr BindingSource kPadDown{SourceKind::GamepadButton, 12};

// Skrypt zdarzen: kazdy krok to wcisniecie/puszczenie/tick/zmiana ekranu; wynik = lista ActionEvent.
struct Script {
    InputRouter r;
    std::vector<ActionEvent> out;
    Script()
    {
        const Binding b[] = {
            {Action::Up, kKeyUp}, {Action::Confirm, kKeyEnter}, {Action::Confirm, kPadA}, {Action::Down, kPadDown},
            {Action::Menu, kPadA},   // jedno zrodlo, dwie akcje
        };
        r.setBindings(b);
        r.setRepeat(300, 50);
    }
    void down(BindingSource s, std::uint64_t t) { r.consume({s, true, t}, out); }
    void up(BindingSource s, std::uint64_t t) { r.consume({s, false, t}, out); }
    void tick(std::uint64_t t) { r.tick(t, out); }
    [[nodiscard]] int count(Action a, Phase p) const
    {
        int n = 0;
        for (const ActionEvent& e : out)
            if (e.action == a && e.phase == p) ++n;
        return n;
    }
};

} // namespace

TEST_SUITE("input/router")
{
    TEST_CASE("kierunkowe i nazwy")
    {
        CHECK(isDirectional(Action::Up));
        CHECK(isDirectional(Action::PageDown));
        CHECK_FALSE(isDirectional(Action::Confirm));
        CHECK_FALSE(isDirectional(Action::MoveUp));
        CHECK_FALSE(isDirectional(Action::None));
    }

    TEST_CASE("zasada 1: nacisniecie niesie ekran z chwili wcisniecia")
    {
        Script s;
        s.r.setContext(ScreenId::Playlists);
        s.down(kPadA, 10);
        REQUIRE(s.out.size() == 2);   // Confirm i Menu
        CHECK(s.out[0].screen == ScreenId::Playlists);
        CHECK(s.out[1].screen == ScreenId::Playlists);
        CHECK(s.out[0].phase == Phase::Pressed);
    }

    TEST_CASE("zasada 2: zmiana ekranu w trakcie trzymania - zatrzask, Released na ekranie pierwotnym")
    {
        Script s;
        s.down(kKeyUp, 0);
        s.r.setContext(ScreenId::Playlists);
        s.tick(1000);                                        // repeat nie dla nowego ekranu
        CHECK(s.count(Action::Up, Phase::Repeat) == 0);
        s.down(kKeyUp, 1001);                                // ponowny "down" bez puszczenia: ignorowany
        CHECK(s.count(Action::Up, Phase::Pressed) == 1);
        s.up(kKeyUp, 1100);
        REQUIRE(s.count(Action::Up, Phase::Released) == 1);
        CHECK(s.out.back().screen == ScreenId::Player);      // ekran pierwotny
        s.down(kKeyUp, 1200);                                // kolejne wcisniecie nalezy do nowego ekranu
        CHECK(s.out.back().phase == Phase::Pressed);
        CHECK(s.out.back().screen == ScreenId::Playlists);
        s.tick(1200 + 300);
        CHECK(s.out.back().phase == Phase::Repeat);
        CHECK(s.out.back().screen == ScreenId::Playlists);
    }

    TEST_CASE("zasada 3: purgePresses usuwa tylko Pressed i Repeat, zachowuje Released i kolejnosc")
    {
        std::vector<ActionEvent> q = {
            {Action::Up, Phase::Pressed, ScreenId::Player, 1},
            {Action::Down, Phase::Released, ScreenId::Player, 2},
            {Action::Up, Phase::Repeat, ScreenId::Player, 3},
            {Action::Confirm, Phase::Released, ScreenId::Playlists, 4},
        };
        purgePresses(q);
        REQUIRE(q.size() == 2);
        CHECK(q[0].action == Action::Down);
        CHECK(q[1].action == Action::Confirm);
        CHECK(q[1].screen == ScreenId::Playlists);
    }

    TEST_CASE("zasada 3: Released po zmianie ekranu i czyszczeniu kolejki nie ginie")
    {
        Script s;
        s.down(kKeyEnter, 0);
        s.r.setContext(ScreenId::Dialog);
        s.up(kKeyEnter, 50);
        purgePresses(s.out);
        REQUIRE(s.out.size() == 1);
        CHECK(s.out[0].phase == Phase::Released);
        CHECK(s.out[0].action == Action::Confirm);
        CHECK_FALSE(s.r.anyHeld());
    }

    TEST_CASE("zasada 4: auto-repeat tylko kierunkowych, po opoznieniu i co tempo; ustaje po puszczeniu")
    {
        Script s;
        s.down(kKeyUp, 0);
        s.down(kKeyEnter, 0);
        s.tick(299);
        CHECK(s.count(Action::Up, Phase::Repeat) == 0);
        s.tick(300);
        CHECK(s.count(Action::Up, Phase::Repeat) == 1);
        s.tick(349);
        CHECK(s.count(Action::Up, Phase::Repeat) == 1);
        s.tick(350);
        CHECK(s.count(Action::Up, Phase::Repeat) == 2);
        s.tick(5000);
        CHECK(s.count(Action::Up, Phase::Repeat) == 3);      // jedno powtorzenie na tick
        CHECK(s.count(Action::Confirm, Phase::Repeat) == 0); // niekierunkowe nie powtarzaja sie
        s.up(kKeyUp, 5001);
        s.tick(6000);
        CHECK(s.count(Action::Up, Phase::Repeat) == 3);
    }

    TEST_CASE("zasada 6: jedna akcja, wiele zrodel - Released po puszczeniu ostatniego")
    {
        Script s;
        s.down(kKeyEnter, 0);
        s.down(kPadA, 5);                                    // Confirm juz trzymany; Menu nowe
        CHECK(s.count(Action::Confirm, Phase::Pressed) == 1);
        CHECK(s.count(Action::Menu, Phase::Pressed) == 1);
        s.up(kKeyEnter, 10);
        CHECK(s.count(Action::Confirm, Phase::Released) == 0);
        s.up(kPadA, 20);
        CHECK(s.count(Action::Confirm, Phase::Released) == 1);
        CHECK(s.count(Action::Menu, Phase::Released) == 1);
        CHECK_FALSE(s.r.anyHeld());
    }

    TEST_CASE("zasada 6: jedno zrodlo, wiele akcji")
    {
        Script s;
        s.down(kPadA, 0);
        s.up(kPadA, 5);
        CHECK(s.count(Action::Confirm, Phase::Pressed) == 1);
        CHECK(s.count(Action::Menu, Phase::Pressed) == 1);
        CHECK(s.count(Action::Confirm, Phase::Released) == 1);
        CHECK(s.count(Action::Menu, Phase::Released) == 1);
    }

    TEST_CASE("zasada 5: onFocusLost - Released dla wszystkich trzymanych, brak repeatu, pozniejsze puszczenie ignorowane")
    {
        Script s;
        s.down(kKeyUp, 0);
        s.down(kPadA, 0);
        s.out.clear();
        s.r.onFocusLost(100, s.out);
        CHECK(s.count(Action::Up, Phase::Released) == 1);
        CHECK(s.count(Action::Confirm, Phase::Released) == 1);
        CHECK(s.count(Action::Menu, Phase::Released) == 1);
        CHECK_FALSE(s.r.anyHeld());
        s.out.clear();
        s.tick(5000);
        s.up(kKeyUp, 5001);
        CHECK(s.out.empty());
        s.down(kKeyUp, 6000);                                // po odzyskaniu fokusu dziala normalnie
        CHECK(s.count(Action::Up, Phase::Pressed) == 1);
    }

    TEST_CASE("nieprzypisane zrodlo nic nie generuje; zmiana wiazan w trakcie trzymania nie gubi puszczenia")
    {
        Script s;
        s.down({SourceKind::Keyboard, 4}, 0);
        CHECK(s.out.empty());
        s.down(kKeyUp, 0);
        s.r.setBindings({});
        s.up(kKeyUp, 10);
        CHECK(s.count(Action::Up, Phase::Released) == 1);
    }
}

TEST_SUITE("input/bindings")
{
    TEST_CASE("nazwy akcji: round-trip, unikalne, stabilne")
    {
        std::set<std::string> seen;
        for (int i = 1; i < static_cast<int>(Action::Count_); ++i) {
            const Action a = static_cast<Action>(i);
            const std::string_view n = actionName(a);
            REQUIRE_FALSE(n.empty());
            CHECK(seen.insert(std::string(n)).second);
            REQUIRE(actionFromName(n).has_value());
            CHECK(*actionFromName(n) == a);
        }
        CHECK_FALSE(actionFromName("nie_ma_takiej").has_value());
        CHECK_FALSE(actionFromName("").has_value());
        CHECK(actionName(Action::PlayPause) == "playPause");
        CHECK(sourceKindFromName(sourceKindName(SourceKind::GamepadAxisNegative)) == SourceKind::GamepadAxisNegative);
    }

    TEST_CASE("domyslne wiazania zgadzaja sie z kodami SDL3")
    {
        const std::vector<Binding> d = defaultBindings();
        auto has = [&](Action a, SourceKind k, int code) {
            for (const Binding& b : d)
                if (b.action == a && b.source.kind == k && b.source.code == code) return true;
            return false;
        };
        CHECK(has(Action::Up, SourceKind::Keyboard, SDL_SCANCODE_UP));
        CHECK(has(Action::Confirm, SourceKind::Keyboard, SDL_SCANCODE_RETURN));
        CHECK(has(Action::Confirm, SourceKind::Keyboard, SDL_SCANCODE_KP_ENTER));
        CHECK(has(Action::Back, SourceKind::Keyboard, SDL_SCANCODE_ESCAPE));
        CHECK(has(Action::PlayPause, SourceKind::Keyboard, SDL_SCANCODE_SPACE));
        CHECK(has(Action::NextTab, SourceKind::Keyboard, SDL_SCANCODE_TAB));
        CHECK(has(Action::PrevTrack, SourceKind::Keyboard, SDL_SCANCODE_P));
        CHECK(has(Action::NextTrack, SourceKind::Keyboard, SDL_SCANCODE_N));
        CHECK(has(Action::ToggleSettings, SourceKind::Keyboard, SDL_SCANCODE_F10));
        CHECK(has(Action::ToggleDiag, SourceKind::Keyboard, SDL_SCANCODE_F12));
        CHECK(has(Action::Delete, SourceKind::Keyboard, SDL_SCANCODE_DELETE));
        CHECK(has(Action::PatternSlower, SourceKind::Keyboard, SDL_SCANCODE_KP_MINUS));
        CHECK(has(Action::Up, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_DPAD_UP));
        CHECK(has(Action::Right, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
        CHECK(has(Action::Confirm, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_SOUTH));
        CHECK(has(Action::Back, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_EAST));
        CHECK(has(Action::ToggleSolo, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_WEST));
        CHECK(has(Action::Menu, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_NORTH));
        CHECK(has(Action::PlayPause, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_START));
        CHECK(has(Action::ToggleTrackerView, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_BACK));
        CHECK(has(Action::NextTab, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
        CHECK(has(Action::PrevTab, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
        CHECK(has(Action::PrevTrack, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_LEFT_STICK));
        CHECK(has(Action::NextTrack, SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_RIGHT_STICK));
        CHECK(has(Action::Left, SourceKind::GamepadAxisNegative, SDL_GAMEPAD_AXIS_LEFTX));
        CHECK(has(Action::Down, SourceKind::GamepadAxisPositive, SDL_GAMEPAD_AXIS_LEFTY));
        CHECK(has(Action::PageUp, SourceKind::GamepadAxisNegative, SDL_GAMEPAD_AXIS_RIGHTY));
        CHECK(has(Action::NextTrack, SourceKind::GamepadAxisPositive, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    }

    TEST_CASE("domyslne: bez dwoch akcji na jednym zrodle, minimalny zestaw spelniony")
    {
        const std::vector<Binding> d = defaultBindings();
        for (const Binding& a : d)
            for (const Binding& b : d)
                if (&a != &b && a.source == b.source) FAIL("zrodlo przypisane dwa razy: akcje " << actionName(a.action) << ", " << actionName(b.action));
        CHECK(missingCritical(d).empty());
        CHECK(effectiveBindings({}) == d);
    }

    TEST_CASE("assign: zwykle przypisanie zastepuje zrodla tej klasy, druga klasa zostaje")
    {
        std::vector<Binding> b = defaultBindings();
        const BindingSource j{SourceKind::Keyboard, SDL_SCANCODE_J};
        const auto r = assign(b, Action::PlayPause, j);
        CHECK(r.ok);
        CHECK_FALSE(r.swappedWith.has_value());
        CHECK(sourcesOf(b, Action::PlayPause, SourceClass::Keyboard) == std::vector<BindingSource>{j});
        CHECK(sourcesOf(b, Action::PlayPause, SourceClass::Gamepad).size() == 1);   // Start
        CHECK_FALSE(findConflict(b, {SourceKind::Keyboard, SDL_SCANCODE_SPACE}, Action::None).has_value());
    }

    TEST_CASE("konflikt: findConflict wskazuje akcje, assign zamienia zrodla")
    {
        std::vector<Binding> b = defaultBindings();
        const BindingSource spaceKey{SourceKind::Keyboard, SDL_SCANCODE_SPACE};
        const BindingSource sKey{SourceKind::Keyboard, SDL_SCANCODE_S};
        REQUIRE(findConflict(b, sKey, Action::PlayPause) == Action::ToggleSolo);
        const auto r = assign(b, Action::PlayPause, sKey);   // PlayPause: Spacja -> S; Solo dostaje Spacje
        REQUIRE(r.ok);
        CHECK(r.swappedWith == Action::ToggleSolo);
        CHECK(sourcesOf(b, Action::PlayPause, SourceClass::Keyboard) == std::vector<BindingSource>{sKey});
        CHECK(sourcesOf(b, Action::ToggleSolo, SourceClass::Keyboard) == std::vector<BindingSource>{spaceKey});
        // po zamianie zadne zrodlo nie jest dublowane
        for (const Binding& x : b)
            for (const Binding& y : b)
                if (&x != &y) CHECK_FALSE((x.source == y.source && x.action != y.action));
    }

    TEST_CASE("akcje krytyczne: nie wolno zostawic bez pada (assign i clearClass)")
    {
        std::vector<Binding> b = defaultBindings();
        CHECK_FALSE(clearClass(b, Action::Back, SourceClass::Gamepad));
        CHECK_FALSE(clearClass(b, Action::Confirm, SourceClass::Gamepad));
        CHECK(missingCritical(b).empty());
        CHECK(clearClass(b, Action::Back, SourceClass::Keyboard));       // klawiatura moze byc pusta
        CHECK(clearClass(b, Action::ToggleSolo, SourceClass::Gamepad));  // nie-krytyczna
    }

    TEST_CASE("assign odrzuca przejecie jedynego przycisku pada akcji krytycznej bez zwrotu")
    {
        std::vector<Binding> b = defaultBindings();
        std::vector<Binding> copy = b;
        const auto r = assign(b, Action::Shuffle, {SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_EAST});
        CHECK_FALSE(r.ok);
        CHECK(b == copy);
        CHECK(missingCritical(b).empty());
    }

    TEST_CASE("zamiana dwoch krytycznych: kazda zachowuje pad")
    {
        std::vector<Binding> b = defaultBindings();
        const auto r = assign(b, Action::Confirm, {SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_EAST});   // A<->B
        REQUIRE(r.ok);
        CHECK(r.swappedWith == Action::Back);
        CHECK(missingCritical(b).empty());
        CHECK(sourcesOf(b, Action::Back, SourceClass::Gamepad).front() == BindingSource{SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_SOUTH});
    }

    TEST_CASE("effectiveBindings: uzupelnia brakujace przyciski pada akcji krytycznych")
    {
        const std::vector<Binding> only = {{Action::PlayPause, {SourceKind::Keyboard, SDL_SCANCODE_SPACE}}};
        const std::vector<Binding> eff = effectiveBindings(only);
        CHECK(missingCritical(eff).empty());
        CHECK(sourcesOf(eff, Action::PlayPause, SourceClass::Keyboard).size() == 1);
    }

    TEST_CASE("akcje lokalne nie sa przypisywalne")
    {
        std::vector<Binding> b = defaultBindings();
        CHECK_FALSE(assign(b, Action::MuteChannel, {SourceKind::Keyboard, SDL_SCANCODE_J}).ok);
        for (Action a : rebindableActions()) CHECK(actionRebindable(a));
    }
}
