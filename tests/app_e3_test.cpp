// Testy E3 po stronie AppController: zapamietywanie trybu powtarzania, wysokosci paska i wiazan,
// tytul/dlugosc wpisow przez PlaylistModel::updateEntryMeta, sciezki sieciowe (efektywne), urzadzenie audio.
// Trwalosc wylacznie w katalogach tymczasowych.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "app_controller.h"
#include "vgmplay/settings.hpp"

using namespace vgmplay;
using namespace vgmplay::app;
namespace fs = std::filesystem;

namespace {

const fs::path kC = fs::path(VGMPLAY_TEST_DATA_DIR) / L"valid" / L"ay8910_melody_loop_v151_gd3en.vgm";

template <class Pred>
bool pumpUntil(AppController& c, Pred p, int timeoutMs = 10000)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(timeoutMs)) {
        c.pump();
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

struct TempDir {
    fs::path path;
    TempDir()
    {
        static std::atomic<int> n{0};
        path = fs::temp_directory_path() /
               (L"vgmplay_e3_test_" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) +
                L"_" + std::to_wstring(n++));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

ControllerOptions persistOpts(const fs::path& dir)
{
    ControllerOptions o;
    o.nullAudio = true;
    o.autoplay = false;
    o.settingsFile = dir / L"settings.json";
    o.saveDelayMs = 50;
    return o;
}

} // namespace

TEST_SUITE("app/e3")
{
    TEST_CASE("tryb powtarzania, pasek oscyloskopow i wiazania przezywaja restart")
    {
        TempDir td;
        {
            AppController c(persistOpts(td.path));
            REQUIRE(c.start());
            c.setRepeat(playlist::RepeatMode::One);
            settings::Settings s = c.settings();
            s.ui.scopeBar = settings::ScopeBar::Hidden;
            s.input.bindings = {{input::Action::Confirm, {input::SourceKind::Keyboard, 4}},
                                {input::Action::Confirm, {input::SourceKind::GamepadButton, 0}}};
            c.applySettings(s);
            REQUIRE(pumpUntil(c, [&] { return false; }, 400) == false);   // debounce zapisu
        }
        AppController c2(persistOpts(td.path));
        REQUIRE(c2.start());
        CHECK(c2.repeat() == playlist::RepeatMode::One);
        CHECK(c2.view().repeat == playlist::RepeatMode::One);
        CHECK(c2.settings().ui.scopeBar == settings::ScopeBar::Hidden);
        REQUIRE(c2.settings().input.bindings.size() == 2);
        CHECK(c2.settings().input.bindings[0].source.code == 4);
    }

    TEST_CASE("tytul i dlugosc zaladowanego utworu trafiaja do wpisu playlisty (updateEntryMeta)")
    {
        ControllerOptions o;
        o.nullAudio = true;
        o.autoplay = false;
        AppController c(o);
        REQUIRE(c.start());
        c.submitPaths({kC});
        REQUIRE(pumpUntil(c, [&] {
            const auto& m = c.playlists();
            return m.playlistCount() > 0 && !m.playlist(m.activeIndex()).entries.empty() &&
                   !m.playlist(m.activeIndex()).entries[0].title.empty() &&
                   m.playlist(m.activeIndex()).entries[0].durationMs > 0;
        }));
    }

    TEST_CASE("sciezki sieciowe: efektywne = ustawienie || flaga; flaga jest oznaczona")
    {
        {
            ControllerOptions o;
            o.nullAudio = true;
            AppController c(o);
            REQUIRE(c.start());
            c.pump();
            CHECK_FALSE(c.view().allowNetworkPaths);
            CHECK_FALSE(c.view().networkForcedByFlag);
            settings::Settings s = c.settings();
            s.library.allowNetworkPaths = true;
            c.applySettings(s);
            c.pump();
            CHECK(c.view().allowNetworkPaths);
            CHECK_FALSE(c.view().networkForcedByFlag);
        }
        ControllerOptions o;
        o.nullAudio = true;
        o.allowNetworkPaths = true;
        AppController c(o);
        REQUIRE(c.start());
        c.pump();
        CHECK(c.view().allowNetworkPaths);
        CHECK(c.view().networkForcedByFlag);
    }

    TEST_CASE("urzadzenie audio: lista na zadanie, zmiana otwiera wyjscie ponownie bez bledu")
    {
        ControllerOptions o;
        o.nullAudio = true;
        AppController c(o);
        REQUIRE(c.start());
        c.pump();
        CHECK_FALSE(c.view().audioDevicesLoaded);
        c.refreshAudioDevices();
        c.pump();
        REQUIRE(c.view().audioDevicesLoaded);
        REQUIRE_FALSE(c.view().audioDevices.empty());
        const std::string id = c.view().audioDevices[0].id;
        settings::Settings s = c.settings();
        s.audio.device = id;
        c.applySettings(s);
        c.pump();
        CHECK(c.view().audioError.empty());
        CHECK(c.settings().audio.device == id);
        s.audio.device = "nie-ma-takiego-urzadzenia";   // nieznane id: domyslne, bez bledu
        c.applySettings(s);
        c.pump();
        CHECK(c.view().audioError.empty());
        s.audio.device.clear();
        c.applySettings(s);
        c.pump();
        CHECK(c.view().audioError.empty());
    }
}
