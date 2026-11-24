// Testy IoWorker (E5-sec): eksport M3U wybiera wolna nazwe w chwili zapisu i nigdy nie nadpisuje istniejacego
// pliku; rozpoznawanie plik/katalog odbywa sie na watku roboczym (UI nie dotyka dysku).
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "io_worker.h"

namespace fs = std::filesystem;
using namespace vgmplay::app;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        static std::atomic<int> n{0};
        path = fs::temp_directory_path() /
               (L"vgmplay_e5_io_" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"_" +
                std::to_wstring(n++));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeAll(const fs::path& p, const std::string& s)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Czeka (bez sztywnego limitu czasu rzeczywistego - do 60 s) na wynik, ktory spelnia `ready`.
template <class Ready>
IoResults waitFor(IoWorker& io, Ready ready)
{
    IoResults all;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) {
        IoResults r = io.take();
        for (auto& e : r.exports) all.exports.push_back(std::move(e));
        for (auto& c : r.classified) all.classified.push_back(std::move(c));
        if (ready(all)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return all;
}

std::size_t countFiles(const fs::path& dir)
{
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) { (void)e; ++n; }
    return n;
}

} // namespace

TEST_SUITE("app/io_worker")
{
    TEST_CASE("eksport M3U: wolna nazwa zostaje, zajeta -> kolejna (n), istniejacy plik nietkniety, bez smieci .tmp")
    {
        TempDir td;
        IoWorker io(IoWorker::Paths{}, [] {});
        const fs::path want = td.path / L"Lista.m3u8";

        io.exportM3u(want, "PIERWSZY");
        auto r1 = waitFor(io, [](const IoResults& r) { return r.exports.size() >= 1; });
        REQUIRE(r1.exports.size() == 1);
        CHECK_FALSE(r1.exports[0].error);
        CHECK(r1.exports[0].file == want);
        CHECK(r1.exports[0].written == want);
        CHECK(readAll(want) == "PIERWSZY");

        io.exportM3u(want, "DRUGI");   // te sama proponowana nazwa: plik juz istnieje
        io.exportM3u(want, "TRZECI");
        auto r2 = waitFor(io, [](const IoResults& r) { return r.exports.size() >= 2; });
        REQUIRE(r2.exports.size() == 2);
        CHECK_FALSE(r2.exports[0].error);
        CHECK_FALSE(r2.exports[1].error);
        CHECK(r2.exports[0].written == td.path / L"Lista (2).m3u8");
        CHECK(r2.exports[1].written == td.path / L"Lista (3).m3u8");
        CHECK(readAll(want) == "PIERWSZY");   // nigdy nie nadpisany
        CHECK(readAll(td.path / L"Lista (2).m3u8") == "DRUGI");
        CHECK(readAll(td.path / L"Lista (3).m3u8") == "TRZECI");
        CHECK(countFiles(td.path) == 3);      // tylko trzy pliki: bez resztek pliku tymczasowego
    }

    TEST_CASE("eksport M3U: nazwa zajeta po 'zapowiedzi' (wyscig) - worker wybiera wolna w chwili zapisu")
    {
        TempDir td;
        IoWorker io(IoWorker::Paths{}, [] {});
        const fs::path want = td.path / L"Wyscig.m3u8";
        // UI zapowiedzialo "Wyscig.m3u8" gdy plik nie istnial; zanim worker zapisal, ktos go utworzyl.
        writeAll(want, "CUDZY");
        io.exportM3u(want, "MOJ");
        auto r = waitFor(io, [](const IoResults& x) { return !x.exports.empty(); });
        REQUIRE(r.exports.size() == 1);
        CHECK_FALSE(r.exports[0].error);
        CHECK(r.exports[0].written == td.path / L"Wyscig (2).m3u8");
        CHECK(readAll(want) == "CUDZY");
        CHECK(readAll(td.path / L"Wyscig (2).m3u8") == "MOJ");
    }

    TEST_CASE("eksport M3U: limit 999 nazw - blad zamiast nadpisania, plik tymczasowy usuniety")
    {
        TempDir td;
        writeAll(td.path / L"X.m3u8", "x");
        for (int i = 2; i <= 999; ++i) writeAll(td.path / (L"X (" + std::to_wstring(i) + L").m3u8"), "x");
        IoWorker io(IoWorker::Paths{}, [] {});
        io.exportM3u(td.path / L"X.m3u8", "NOWY");
        auto r = waitFor(io, [](const IoResults& x) { return !x.exports.empty(); });
        REQUIRE(r.exports.size() == 1);
        REQUIRE(r.exports[0].error);
        CHECK(r.exports[0].written.empty());
        CHECK(readAll(td.path / L"X.m3u8") == "x");
        CHECK(readAll(td.path / L"X (999).m3u8") == "x");
        CHECK(countFiles(td.path) == 999);   // bez pliku tymczasowego i bez (1000)
    }

    TEST_CASE("eksport M3U: overwrite (systemowe okno po potwierdzeniu) zastepuje plik, bez (2) i bez smieci")
    {
        TempDir td;
        const fs::path want = td.path / L"Zastap.m3u8";
        writeAll(want, "STARY");
        IoWorker io(IoWorker::Paths{}, [] {});
        io.exportM3u(want, "NOWY", true);
        auto r = waitFor(io, [](const IoResults& x) { return !x.exports.empty(); });
        REQUIRE(r.exports.size() == 1);
        CHECK_FALSE(r.exports[0].error);
        CHECK(r.exports[0].written == want);
        CHECK(readAll(want) == "NOWY");
        CHECK(countFiles(td.path) == 1);
    }

    TEST_CASE("eksport M3U: propozycja juz ponumerowana nie dostaje drugiego numeru (Mix (2) -> Mix (3), nie Mix (2) (2))")
    {
        TempDir td;
        writeAll(td.path / L"Mix.m3u8", "0");
        writeAll(td.path / L"Mix (2).m3u8", "2");
        IoWorker io(IoWorker::Paths{}, [] {});
        io.exportM3u(td.path / L"Mix (2).m3u8", "A");   // zapowiedz przegladarki, ktora zdazyla sie zajac
        io.exportM3u(td.path / L"Mix (2).m3u8", "B");
        auto r = waitFor(io, [](const IoResults& x) { return x.exports.size() >= 2; });
        REQUIRE(r.exports.size() == 2);
        CHECK(r.exports[0].written == td.path / L"Mix (3).m3u8");
        CHECK(r.exports[1].written == td.path / L"Mix (4).m3u8");
        CHECK(readAll(td.path / L"Mix (2).m3u8") == "2");
        CHECK_FALSE(fs::exists(td.path / L"Mix (2) (2).m3u8"));
        // Wolna ponumerowana propozycja zostaje bez zmian; nazwa z rokiem w nawiasie nie jest numerem kolejnym.
        io.exportM3u(td.path / L"Nowy (7).m3u8", "C");
        io.exportM3u(td.path / L"Raport (2019).m3u8", "D");
        auto r2 = waitFor(io, [](const IoResults& x) { return x.exports.size() >= 2; });
        REQUIRE(r2.exports.size() == 2);
        CHECK(r2.exports[0].written == td.path / L"Nowy (7).m3u8");
        CHECK(r2.exports[1].written == td.path / L"Raport (2019).m3u8");
    }

    TEST_CASE("classifyPaths: tag wraca razem z wynikiem (ten sam indeks)")
    {
        TempDir td;
        writeAll(td.path / L"a.vgm", "x");
        IoWorker io(IoWorker::Paths{}, [] {});
        io.classifyPaths({td.path / L"a.vgm"}, "Lista A");
        io.classifyPaths({td.path / L"a.vgm"}, "Lista B");
        IoResults all;
        const auto t0 = std::chrono::steady_clock::now();
        while (all.classified.size() < 2 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) {
            IoResults r = io.take();
            for (auto& c : r.classified) all.classified.push_back(std::move(c));
            for (auto& t : r.classifiedTags) all.classifiedTags.push_back(std::move(t));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(all.classifiedTags.size() == 2);
        CHECK(all.classifiedTags[0] == "Lista A");
        CHECK(all.classifiedTags[1] == "Lista B");
    }

    TEST_CASE("eksport M3U: nieistniejacy katalog -> blad (katalogow nie tworzymy)")
    {
        TempDir td;
        IoWorker io(IoWorker::Paths{}, [] {});
        io.exportM3u(td.path / L"nie_ma" / L"a.m3u8", "x");
        auto r = waitFor(io, [](const IoResults& x) { return !x.exports.empty(); });
        REQUIRE(r.exports.size() == 1);
        CHECK(r.exports[0].error.has_value());
        CHECK_FALSE(fs::exists(td.path / L"nie_ma"));
    }

    TEST_CASE("eksport M3U: istniejace dowiazanie symboliczne pod docelowa nazwa nie jest nadpisywane ani sledzone")
    {
        TempDir td;
        const fs::path victim = td.path / L"ofiara.txt";
        writeAll(victim, "NIETKNIETY");
        std::error_code ec;
        fs::create_symlink(victim, td.path / L"L.m3u8", ec);
        if (ec) {
            MESSAGE("brak uprawnien do dowiazan symbolicznych (" << ec.message() << ") - test pominiety");
            return;
        }
        IoWorker io(IoWorker::Paths{}, [] {});
        io.exportM3u(td.path / L"L.m3u8", "TRESC");
        auto r = waitFor(io, [](const IoResults& x) { return !x.exports.empty(); });
        REQUIRE(r.exports.size() == 1);
        CHECK_FALSE(r.exports[0].error);
        CHECK(r.exports[0].written == td.path / L"L (2).m3u8");
        CHECK(readAll(victim) == "NIETKNIETY");
    }

    TEST_CASE("classifyPaths: plik / katalog / brak rozpoznawane na watku roboczym, kolejnosc zachowana")
    {
        TempDir td;
        fs::create_directories(td.path / L"folder");
        writeAll(td.path / L"a.vgm", "x");
        IoWorker io(IoWorker::Paths{}, [] {});
        io.classifyPaths({td.path / L"folder", td.path / L"a.vgm", td.path / L"brak.vgm"});
        auto r = waitFor(io, [](const IoResults& x) { return !x.classified.empty(); });
        REQUIRE(r.classified.size() == 1);
        const auto& v = r.classified[0];
        REQUIRE(v.size() == 3);
        CHECK(v[0].path == td.path / L"folder");
        CHECK(v[0].isDirectory);
        CHECK(v[1].path == td.path / L"a.vgm");
        CHECK_FALSE(v[1].isDirectory);
        CHECK_FALSE(v[2].isDirectory);
    }

    TEST_CASE("classifyPaths: UNC bez zgody nie jest sprawdzany w sieci (nigdy katalog), URL i urzadzenia tez")
    {
        IoWorker io(IoWorker::Paths{}, [] {});   // allowNetworkPaths = false
        io.classifyPaths({fs::path(L"\\\\srv.invalid\\share\\x"), fs::path(L"\\\\?\\C:\\"), fs::path(L"http://x/y")});
        auto r = waitFor(io, [](const IoResults& x) { return !x.classified.empty(); });
        REQUIRE(r.classified.size() == 1);
        REQUIRE(r.classified[0].size() == 3);
        for (const auto& c : r.classified[0]) CHECK_FALSE(c.isDirectory);
    }
}
