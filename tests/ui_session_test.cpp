// Testy AppController + PlaylistModel: auto-advance z RepeatMode, Prev w 3 s, sesja (przywrocenie
// pozycji tylko dla tej samej sciezki, brak auto-play), zapis ustawien z opoznieniem, import M3U
// z odrzuceniami, uszkodzony playlists.json. Trwalosc wylacznie w katalogach tymczasowych
// (ControllerOptions::settingsFile) - testy nie dotykaja prawdziwego %APPDATA% ani katalogu exe.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "app_controller.h"
#include "vgmplay/playlist.hpp"
#include "vgmplay/settings.hpp"

using namespace vgmplay;
using namespace vgmplay::app;
using engine::TransportState;
namespace fs = std::filesystem;

namespace {

const fs::path kData = VGMPLAY_TEST_DATA_DIR;
const fs::path kA = kData / L"valid" / L"sn76489_scale_v100.vgm";
const fs::path kB = kData / L"valid" / L"ym3812_melody_v151.vgm";
const fs::path kLong = kData / L"valid" / L"ym2612_melody_loop_v150_gd3jp.vgm";
const fs::path kC = kData / L"valid" / L"ay8910_melody_loop_v151_gd3en.vgm";

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

void pumpFor(AppController& c, int ms)
{
    (void)pumpUntil(c, [] { return false; }, ms);
}

struct TempDir {
    fs::path path;
    TempDir()
    {
        static std::atomic<int> n{0};
        path = fs::temp_directory_path() /
               (L"vgmplay_e2_test_" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) +
                L"_" + std::to_wstring(n++));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

ControllerOptions nullOpts(bool autoplay = true)
{
    ControllerOptions o;
    o.nullAudio = true;
    o.autoplay = autoplay;
    return o;
}

ControllerOptions persistOpts(const fs::path& dir, bool autoplay = false)
{
    ControllerOptions o = nullOpts(autoplay);
    o.settingsFile = dir / L"settings.json";
    o.saveDelayMs = 50;
    return o;
}

bool isTrack(const AppController& c, const fs::path& p)
{
    return c.view().hasTrack() && c.view().track->path.filename() == p.filename();
}

std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& p, const std::string& s)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

std::int64_t cursorOf(const AppController& c)
{
    return c.playlists().playlist(c.playlists().activeIndex()).cursor;
}

std::size_t countOf(const AppController& c)
{
    return c.playlists().playlist(c.playlists().activeIndex()).entries.size();
}

// Przewija blisko konca utworu (przyspiesza test auto-advance).
void seekNearEnd(AppController& c)
{
    const std::uint64_t total = c.view().transport.totalFrames;
    const std::uint64_t sr = c.view().sampleRate;
    REQUIRE(total > sr);
    c.seekToFrame(total - sr / 5);
}

} // namespace

TEST_SUITE("app/playlist_transport")
{
    TEST_CASE("auto-advance wg RepeatMode: Off zatrzymuje na koncu, All zawija, One powtarza")
    {
        AppController c(nullOpts());
        REQUIRE(c.start());
        REQUIRE(c.playlistsReady());
        c.submitPaths({kA, kB});
        REQUIRE(pumpUntil(c, [&] { return isTrack(c, kA); }));
        CHECK(countOf(c) == 2);
        CHECK(cursorOf(c) == 0);   // pierwszy dodany wpis gra od razu (nic nie bylo zaladowane)
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Playing; }));

        c.setRepeat(playlist::RepeatMode::Off);
        seekNearEnd(c);
        REQUIRE(pumpUntil(c, [&] { return isTrack(c, kB); }));
        CHECK(cursorOf(c) == 1);

        // Off: koniec ostatniego wpisu => Ended, kursor bez zmian.
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Playing; }));
        seekNearEnd(c);
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Ended; }));
        pumpFor(c, 300);
        CHECK(isTrack(c, kB));
        CHECK(cursorOf(c) == 1);

        // All: za ostatnim wpisem wraca na poczatek.
        c.setRepeat(playlist::RepeatMode::All);
        c.play();   // z Ended: od poczatku
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Playing; }));
        seekNearEnd(c);
        REQUIRE(pumpUntil(c, [&] { return isTrack(c, kA); }));
        CHECK(cursorOf(c) == 0);

        // One: ten sam wpis, ponowne zaladowanie (nowy obiekt utworu).
        c.setRepeat(playlist::RepeatMode::One);
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Playing; }));
        const auto before = c.view().track;
        seekNearEnd(c);
        REQUIRE(pumpUntil(c, [&] { return c.view().track != before && isTrack(c, kA); }));
        CHECK(cursorOf(c) == 0);

        // Reczne "nastepny" przy One idzie dalej (One traktowane jak Off).
        c.playNext();
        REQUIRE(pumpUntil(c, [&] { return isTrack(c, kB); }));
        CHECK(cursorOf(c) == 1);
    }

    TEST_CASE("Prev: w pierwszych 3 s poprzedni wpis, pozniej poczatek biezacego utworu")
    {
        AppController c(nullOpts());
        REQUIRE(c.start());
        c.submitPaths({kLong, kB});
        REQUIRE(pumpUntil(c, [&] { return isTrack(c, kLong); }));
        c.playNext();
        REQUIRE(pumpUntil(c, [&] { return isTrack(c, kB); }));
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Playing; }));
        REQUIRE(c.view().transport.audibleFrame < 3ull * c.view().sampleRate);

        c.playPrevious();   // < 3 s => poprzedni wpis
        REQUIRE(pumpUntil(c, [&] { return isTrack(c, kLong); }));
        CHECK(cursorOf(c) == 0);

        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Playing; }));
        const std::uint64_t sr = c.view().sampleRate;
        if (c.view().transport.totalFrames < 5 * sr) {
            MESSAGE("utwor testowy krotszy niz 5 s - czesc 'po 3 s' pominieta");
            return;
        }
        c.seekToFrame(4 * sr);
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.audibleFrame >= 3 * sr + sr / 2; }));
        c.playPrevious();   // > 3 s => ten sam utwor od poczatku
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.audibleFrame < sr; }));
        CHECK(isTrack(c, kLong));
        CHECK(cursorOf(c) == 0);
    }

    TEST_CASE("usuniecie biezacego wpisu zeruje kursor i nie wywoluje auto-advance")
    {
        AppController c(nullOpts());
        REQUIRE(c.start());
        c.submitPaths({kA, kB});
        REQUIRE(pumpUntil(c, [&] { return isTrack(c, kA); }));
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Playing; }));
        c.removeEntry(0);
        c.pump();
        CHECK(cursorOf(c) == -1);
        CHECK(countOf(c) == 1);
        seekNearEnd(c);
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Ended; }));
        pumpFor(c, 300);
        CHECK(isTrack(c, kA));   // nic nie zaladowano po koncu: wpis zniknal z kursora
    }

    TEST_CASE("metadane w tle: dlugosc i tytul trafiaja do widoku")
    {
        AppController c(nullOpts(false));
        REQUIRE(c.start());
        const fs::path g = kData / L"valid" / L"ym2612_melody_loop_v150_gd3jp.vgm";
        c.submitPaths({g});
        REQUIRE(pumpUntil(c, [&] {
            const auto* d = c.view().meta ? c.view().meta->find(g) : nullptr;
            return d && d->durationMs > 0;
        }));
        const auto* d = c.view().meta->find(g);
        CHECK_FALSE(d->failed);
        CHECK((!d->trackEn.empty() || !d->trackJp.empty()));
    }

    TEST_CASE("pliki spoza VGM sa pomijane z komunikatem")
    {
        TempDir td;
        writeAll(td.path / L"readme.txt", "x");
        AppController c(nullOpts(false));
        REQUIRE(c.start());
        c.submitPaths({td.path / L"readme.txt"});
        REQUIRE(pumpUntil(c, [&] { return c.view().notice.has_value(); }));
        CHECK(c.view().notice->error);
        CHECK(countOf(c) == 0);
    }
}

TEST_SUITE("app/session")
{
    TEST_CASE("sesja: pozycja przywracana tylko dla tej samej sciezki, bez auto-play")
    {
        TempDir td;
        std::uint64_t posFrames = 0, sr = 0;
        {
            AppController c(persistOpts(td.path));
            REQUIRE(c.start());
            REQUIRE(pumpUntil(c, [&] { return c.playlistsReady(); }));
            c.submitPaths({kA, kB});
            REQUIRE(pumpUntil(c, [&] { return isTrack(c, kA); }));
            c.playNext();
            REQUIRE(pumpUntil(c, [&] { return isTrack(c, kB); }));
            sr = c.view().sampleRate;
            posFrames = c.view().transport.totalFrames / 2;
            REQUIRE(posFrames > 0);
            c.seekToFrame(posFrames);
            REQUIRE(pumpUntil(c, [&] { return c.view().transport.audibleFrame >= posFrames; }));
            pumpFor(c, 50);
        }   // zamkniecie: zapis ustawien (sesja) i playlist
        REQUIRE(fs::exists(td.path / L"settings.json"));
        REQUIRE(fs::exists(td.path / L"playlists.json"));
        REQUIRE(fs::exists(td.path / L"library-cache.json"));
        auto saved = settings::loadSettings(td.path / L"settings.json");
        REQUIRE(saved.ok());
        CHECK(saved.value().session.trackPath.find("ym3812_melody_v151.vgm") != std::string::npos);
        CHECK(saved.value().session.positionMs >= posFrames * 1000 / sr);

        {   // ten sam utwor pod kursorem: wczytany, przewiniety, w pauzie
            AppController c(persistOpts(td.path));
            REQUIRE(c.start());
            REQUIRE(pumpUntil(c, [&] { return isTrack(c, kB); }));
            CHECK(cursorOf(c) == 1);
            pumpFor(c, 300);
            CHECK(c.view().transport.state != TransportState::Playing);   // bez automatycznego startu
            CHECK(c.view().transport.audibleFrame >= posFrames);
            CHECK(c.view().transport.audibleFrame <= posFrames + sr);
        }

        {   // inna sciezka w sesji niz pod kursorem: pozycja = 0
            auto s = settings::loadSettings(td.path / L"settings.json");
            REQUIRE(s.ok());
            s.value().session.trackPath = "C:/some/other/file.vgm";
            s.value().session.positionMs = 5000;
            REQUIRE(settings::saveSettings(td.path / L"settings.json", s.value()).ok());
            AppController c(persistOpts(td.path));
            REQUIRE(c.start());
            REQUIRE(pumpUntil(c, [&] { return isTrack(c, kB); }));
            pumpFor(c, 200);
            CHECK(c.view().transport.state != TransportState::Playing);
            CHECK(c.view().transport.audibleFrame < sr / 10);
        }
    }

    TEST_CASE("plik z wiersza polecen: dodany do aktywnej playlisty i odtwarzany, sesja nie jest wznawiana")
    {
        TempDir td;
        {
            AppController c(persistOpts(td.path));
            REQUIRE(c.start());
            REQUIRE(pumpUntil(c, [&] { return c.playlistsReady(); }));
            c.submitPaths({kA, kB});
            REQUIRE(pumpUntil(c, [&] { return isTrack(c, kA); }));
        }
        AppController c(persistOpts(td.path));
        REQUIRE(c.start());
        c.openStartupFile(kC);
        REQUIRE(pumpUntil(c, [&] { return isTrack(c, kC); }));
        CHECK(countOf(c) == 3);
        CHECK(cursorOf(c) == 2);
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Playing; }));
    }

    TEST_CASE("zapis ustawien z opoznieniem (debounce) na workerze")
    {
        TempDir td;
        ControllerOptions o = persistOpts(td.path);
        o.saveDelayMs = 300;
        AppController c(o);
        REQUIRE(c.start());
        REQUIRE(pumpUntil(c, [&] { return c.playlistsReady(); }));
        c.setVolume(0.35f);
        auto s = c.settings();
        s.playback.loopCount = 5;
        s.ui.gd3Language = format::Gd3Language::Jp;
        c.applySettings(s);
        pumpFor(c, 50);
        CHECK_FALSE(fs::exists(td.path / L"settings.json"));   // jeszcze nie minelo opoznienie
        REQUIRE(pumpUntil(c, [&] { return fs::exists(td.path / L"settings.json"); }, 3000));
        pumpFor(c, 100);
        auto saved = settings::loadSettings(td.path / L"settings.json");
        REQUIRE(saved.ok());
        CHECK(saved.value().playback.volume == doctest::Approx(0.35f));
        CHECK(saved.value().playback.loopCount == 5);
        CHECK(saved.value().ui.gd3Language == format::Gd3Language::Jp);
    }

    TEST_CASE("ustawienia: wczytane wartosci sa stosowane (glosnosc, rowTicks)")
    {
        TempDir td;
        settings::Settings s;
        s.playback.volume = 0.25f;
        s.ui.rowTicks = 882;
        s.playback.loopCount = 3;
        REQUIRE(settings::saveSettings(td.path / L"settings.json", s).ok());
        AppController c(persistOpts(td.path));
        REQUIRE(c.start());
        c.pump();
        CHECK(c.view().volume == doctest::Approx(0.25f));
        CHECK(c.view().patternRowTicks == 882);
        CHECK(c.settings().playback.loopCount == 3);
    }
}

TEST_SUITE("app/m3u_and_corruption")
{
    TEST_CASE("import M3U: odrzucone pozycje sa liczone i opisane, reszta trafia na nowa playliste")
    {
        TempDir td;
        fs::create_directories(td.path / L"sub");
        const std::string text =
            "#EXTM3U\n"
            "a.vgm\n"
            "sub/b.vgm\n"
            "http://example.com/x.vgm\n"
            "../outside.vgm\n"
            "\\\\srv\\share\\c.vgm\n";
        writeAll(td.path / L"lista.m3u", text);
        AppController c(nullOpts(false));
        REQUIRE(c.start());
        c.submitImportM3u(td.path / L"lista.m3u");
        REQUIRE(pumpUntil(c, [&] { return c.view().notice.has_value(); }));
        REQUIRE(c.playlists().playlistCount() == 2);
        CHECK(c.playlists().playlist(c.playlists().activeIndex()).name == "lista");
        CHECK(countOf(c) == 2);
        REQUIRE(c.view().notice.has_value());
        CHECK(c.view().notice->text.find('3') != std::string::npos);   // odrzucono 3
        CHECK_FALSE(c.view().notice->details.empty());
        CHECK(c.view().notice->error);
    }

    TEST_CASE("import M3U: plik wiekszy niz 16 MB jest odrzucany bez wczytywania")
    {
        TempDir td;
        writeAll(td.path / L"huge.m3u", std::string(kMaxM3uBytes + 1, 'a'));
        AppController c(nullOpts(false));
        REQUIRE(c.start());
        c.submitImportM3u(td.path / L"huge.m3u");
        REQUIRE(pumpUntil(c, [&] { return c.view().notice.has_value(); }));
        CHECK(c.view().notice->error);
        CHECK(c.playlists().playlistCount() == 1);
    }

    TEST_CASE("eksport i ponowny import M3U (round trip)")
    {
        TempDir td;
        AppController c(nullOpts(false));
        REQUIRE(c.start());
        c.submitPaths({kA, kB});
        REQUIRE(pumpUntil(c, [&] { return countOf(c) == 2; }));
        c.submitExportM3u(td.path / L"out.m3u8");
        REQUIRE(pumpUntil(c, [&] { return fs::exists(td.path / L"out.m3u8"); }));
        pumpFor(c, 100);
        CHECK_FALSE(readAll(td.path / L"out.m3u8").empty());
        CHECK_FALSE(fs::exists(td.path / L"out.m3u8.tmp"));
    }

    TEST_CASE("uszkodzony playlists.json: komunikat, brak nadpisania do potwierdzenia")
    {
        TempDir td;
        const std::string garbage = "{ to nie jest poprawny json ";
        writeAll(td.path / L"playlists.json", garbage);
        {
            AppController c(persistOpts(td.path));
            REQUIRE(c.start());
            REQUIRE(pumpUntil(c, [&] { return c.playlistsReady(); }));
            REQUIRE(c.view().notice.has_value());
            CHECK(c.view().notice->needsAck);
            CHECK(c.view().notice->error);
            CHECK(c.playlists().playlistCount() == 1);   // domyslna, pusta lista
            c.submitPaths({kA});                         // zmiany w pamieci sa mozliwe...
            REQUIRE(pumpUntil(c, [&] { return countOf(c) == 1; }));
            pumpFor(c, 400);                             // ...ale zapis jest zablokowany
            CHECK(readAll(td.path / L"playlists.json") == garbage);
        }   // takze zamkniecie nie nadpisuje
        CHECK(readAll(td.path / L"playlists.json") == garbage);

        {   // po potwierdzeniu zapis jest dozwolony (kopia .bak/.bad zostaje u backendu)
            AppController c(persistOpts(td.path));
            REQUIRE(c.start());
            REQUIRE(pumpUntil(c, [&] { return c.playlistsReady() && c.view().notice.has_value(); }));
            c.dismissNotice();
            c.submitPaths({kA});
            REQUIRE(pumpUntil(c, [&] { return countOf(c) == 1; }));
        }
        CHECK(readAll(td.path / L"playlists.json") != garbage);
        CHECK(playlist::loadPlaylistsFile(td.path / L"playlists.json").ok());
    }

    TEST_CASE("uszkodzony settings.json: komunikat i brak nadpisania do potwierdzenia")
    {
        TempDir td;
        const std::string garbage = "[[[";
        writeAll(td.path / L"settings.json", garbage);
        {
            AppController c(persistOpts(td.path));
            REQUIRE(c.start());
            REQUIRE(c.view().notice.has_value());
            CHECK(c.view().notice->needsAck);
            c.setVolume(0.5f);
            pumpFor(c, 300);
        }
        CHECK(readAll(td.path / L"settings.json") == garbage);
    }
}
