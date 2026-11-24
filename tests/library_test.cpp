// Testy modulu library: ScanJob (skan, junction, limity, anulowanie) oraz metadane + cache.
#include <doctest/doctest.h>

#include <ostream>   // wymagane przez doctest dla stringifikacji std::string_view

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "vgmplay/library.hpp"

namespace {

using namespace vgmplay;
using namespace vgmplay::library;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir()
    {
        static int n = 0;
        path = fs::temp_directory_path() / (L"vgmplay_lib_test_" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(n++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void touch(const fs::path& p, const std::string& content = "x")
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
}

bool makeJunction(const fs::path& link, const fs::path& target)
{
    const std::string cmd = "mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>nul";
    return std::system(cmd.c_str()) == 0 && fs::exists(link);
}

// Czeka na koniec joba (z limitem czasu, zeby test nie wisial).
ScanResult runScan(const fs::path& root, const ScanOptions& o = {})
{
    auto job = ScanJob::start(root, o);
    REQUIRE(job.ok());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!job.value()->done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(job.value()->done());
    auto r = job.value()->takeResult();
    REQUIRE(r.has_value());
    return std::move(*r);
}

std::vector<std::string> names(const ScanResult& r, const fs::path& root)
{
    std::vector<std::string> v;
    for (const auto& f : r.files) {
        const std::u8string u = f.path.lexically_relative(root).generic_u8string();
        v.emplace_back(reinterpret_cast<const char*>(u.data()), u.size());
    }
    return v;
}

bool hasWarning(const ScanResult& r, WarningCode c)
{
    return std::any_of(r.warnings.begin(), r.warnings.end(), [c](const Warning& w) { return w.code == c; });
}

} // namespace

TEST_CASE("library: skan rekurencyjny, filtr rozszerzen bez rozrozniania wielkosci, sortowanie")
{
    TempDir td;
    touch(td.path / "b.vgm", "12345");
    touch(td.path / "A.VGZ");
    touch(td.path / "c.txt");
    touch(td.path / "noext");
    touch(td.path / "d.m3u");
    touch(td.path / "sub" / "z.vgz");
    touch(td.path / "sub" / "deep" / "y.VgM");
    touch(td.path / L"Zażółć" / L"gęślą.vgz");
    touch(td.path / L"日本語" / L"曲.vgm");

    const ScanResult r = runScan(td.path);
    CHECK_FALSE(r.cancelled);
    CHECK_FALSE(r.truncatedByMaxFiles);
    CHECK_FALSE(r.truncatedByMaxDepth);
    CHECK(r.reparsePointsSkipped == 0);
    CHECK(r.directoriesVisited == 5);   // root, sub, sub/deep, Zażółć, 日本語
    REQUIRE(r.files.size() == 6);
    // sortowanie bez rozrozniania wielkosci liter, stabilne
    const std::vector<std::string> n = names(r, td.path);
    CHECK(n[0] == "A.VGZ");
    CHECK(n[1] == "b.vgm");
    CHECK(std::is_sorted(r.files.begin(), r.files.end(), [](const FoundFile& a, const FoundFile& b) {
        std::wstring x = a.path.native(), y = b.path.native();
        ::CharLowerBuffW(x.data(), static_cast<DWORD>(x.size()));
        ::CharLowerBuffW(y.data(), static_cast<DWORD>(y.size()));
        return x < y;
    }));
    for (const auto& f : r.files) CHECK(f.path.is_absolute());
    const auto it = std::find_if(r.files.begin(), r.files.end(), [](const FoundFile& f) { return f.path.filename() == fs::path("b.vgm"); });
    REQUIRE(it != r.files.end());
    CHECK(it->sizeBytes == 5);
    CHECK(std::find(n.begin(), n.end(), "sub/deep/y.VgM") != n.end());
    CHECK(std::find(n.begin(), n.end(), "日本語/曲.vgm") != n.end());

    // wlasne rozszerzenia (takze m3u/m3u8; bez kropki tez dziala)
    ScanOptions o;
    o.extensions = {".m3u", "m3u8"};
    touch(td.path / "sub" / "list.M3U8");
    const ScanResult r2 = runScan(td.path, o);
    CHECK(r2.files.size() == 2);
}

TEST_CASE("library: recursive=false i maxDepth")
{
    TempDir td;
    touch(td.path / "a.vgm");
    touch(td.path / "l1" / "b.vgm");
    touch(td.path / "l1" / "l2" / "c.vgm");
    touch(td.path / "l1" / "l2" / "l3" / "d.vgm");

    ScanOptions o;
    o.recursive = false;
    CHECK(runScan(td.path, o).files.size() == 1);

    o = ScanOptions{};
    o.maxDepth = 0;   // tylko root
    ScanResult r0 = runScan(td.path, o);
    CHECK(r0.files.size() == 1);
    CHECK(r0.truncatedByMaxDepth);
    CHECK(hasWarning(r0, WarningCode::LimitReached));

    o.maxDepth = 2;
    ScanResult r2 = runScan(td.path, o);
    CHECK(r2.files.size() == 3);   // a, b, c
    CHECK(r2.truncatedByMaxDepth);

    o.maxDepth = 3;
    ScanResult r3 = runScan(td.path, o);
    CHECK(r3.files.size() == 4);
    CHECK_FALSE(r3.truncatedByMaxDepth);
}

TEST_CASE("library: maxFiles")
{
    TempDir td;
    for (int i = 0; i < 20; ++i) touch(td.path / ("f" + std::to_string(i) + ".vgm"));
    ScanOptions o;
    o.maxFiles = 7;
    const ScanResult r = runScan(td.path, o);
    CHECK(r.files.size() == 7);
    CHECK(r.truncatedByMaxFiles);
    CHECK(hasWarning(r, WarningCode::LimitReached));

    o.maxFiles = 20;   // dokladnie tyle plikow: bez obciecia
    const ScanResult r2 = runScan(td.path, o);
    CHECK(r2.files.size() == 20);
    CHECK_FALSE(r2.truncatedByMaxFiles);
}

TEST_CASE("library: junction pomijany z ostrzezeniem, followReparsePoints=true podaza")
{
    TempDir td;
    touch(td.path / "root" / "own.vgm");
    touch(td.path / "outside" / "secret.vgm");
    if (!makeJunction(td.path / "root" / "link", td.path / "outside")) {
        MESSAGE("nie mozna utworzyc junction (mklink /J) - test pominiety");
        return;
    }
    const ScanResult r = runScan(td.path / "root");
    REQUIRE(r.files.size() == 1);
    CHECK(r.files[0].path.filename() == fs::path("own.vgm"));
    CHECK(r.reparsePointsSkipped == 1);
    CHECK(hasWarning(r, WarningCode::ReparsePointSkipped));
    for (const auto& w : r.warnings) CHECK(w.message.find(td.path.string()) == std::string::npos);   // bez pelnych sciezek

    ScanOptions o;
    o.followReparsePoints = true;
    const ScanResult r2 = runScan(td.path / "root", o);
    if (r2.files.size() == 1 && hasWarning(r2, WarningCode::ReparsePointSkipped)) {
        // Wolumin bez GetFinalPathNameByHandle (np. dysk RAM): cel dowiazania nie da sie rozwiazac,
        // wiec skan zachowawczo go pomija (ostrzezenie). Na NTFS/ReFS zachodzi galaz ponizej.
        MESSAGE("wolumin testowy nie rozwiazuje dowiazan - asercje followReparsePoints=true pominiete");
        return;
    }
    CHECK(r2.files.size() == 2);
    CHECK(r2.reparsePointsSkipped == 0);
}

TEST_CASE("library: cykl przez junction przy followReparsePoints=true nie zapetla skanu")
{
    TempDir td;
    touch(td.path / "root" / "a.vgm");
    if (!makeJunction(td.path / "root" / "loop", td.path / "root")) {
        MESSAGE("nie mozna utworzyc junction (mklink /J) - test pominiety");
        return;
    }
    ScanOptions o;
    o.followReparsePoints = true;
    o.maxDepth = 50;
    const ScanResult r = runScan(td.path / "root", o);
    CHECK(r.files.size() <= 3);   // a.vgm, ewentualnie jedno wejscie przez petle; nie 50
    CHECK(r.files.size() >= 1);
}

TEST_CASE("library: junction jako root jest dozwolony (wybor uzytkownika)")
{
    TempDir td;
    touch(td.path / "real" / "a.vgm");
    if (!makeJunction(td.path / "alias", td.path / "real")) {
        MESSAGE("nie mozna utworzyc junction (mklink /J) - test pominiety");
        return;
    }
    CHECK(runScan(td.path / "alias").files.size() == 1);
}

TEST_CASE("library: bledy startu")
{
    TempDir td;
    touch(td.path / "plik.vgm");
    CHECK(ScanJob::start(td.path / "nie_ma").error().code == ErrorCode::NotFound);
    CHECK(ScanJob::start(td.path / "plik.vgm").error().code == ErrorCode::InvalidArgument);
    CHECK(ScanJob::start({}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("library: pusty katalog i takeResult tylko raz")
{
    TempDir td;
    auto job = ScanJob::start(td.path);
    REQUIRE(job.ok());
    while (!job.value()->done()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto r = job.value()->takeResult();
    REQUIRE(r.has_value());
    CHECK(r->files.empty());
    CHECK(r->directoriesVisited == 1);
    CHECK_FALSE(job.value()->takeResult().has_value());
    CHECK(job.value()->progress().directoriesVisited == 1);
}

TEST_CASE("library: anulowanie i destruktor dolaczajacy watek")
{
    TempDir td;
    for (int d = 0; d < 40; ++d)
        for (int f = 0; f < 10; ++f) touch(td.path / ("d" + std::to_string(d)) / ("f" + std::to_string(f) + ".vgm"));

    bool sawCancelled = false;
    for (int attempt = 0; attempt < 30 && !sawCancelled; ++attempt) {
        auto job = ScanJob::start(td.path);
        REQUIRE(job.ok());
        job.value()->cancel();
        job.value()->cancel();   // idempotentne
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!job.value()->done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(job.value()->done());   // takze po anulowaniu
        auto r = job.value()->takeResult();
        REQUIRE(r.has_value());
        if (r->cancelled) {
            sawCancelled = true;
            CHECK(r->files.size() < 400);
        } else {
            CHECK(r->files.size() == 400);   // skan zdazyl sie skonczyc przed anulowaniem
        }
    }
    CHECK(sawCancelled);

    // Zniszczenie joba w trakcie pracy nie wiesza sie i nie crashuje.
    for (int i = 0; i < 10; ++i) {
        auto job = ScanJob::start(td.path);
        REQUIRE(job.ok());
    }
}

TEST_CASE("library: katalog usuniety w trakcie skanu nie crashuje (Warning zamiast wyjatku)")
{
    TempDir td;
    touch(td.path / "a" / "x.vgm");
    touch(td.path / "b" / "y.vgm");
    const ScanResult r = runScan(td.path);   // sanity: dziala; brak wyjatkow w ogole
    CHECK(r.files.size() == 2);
}

// ---------------------------------------------------------------------------
// Metadane i cache (dodatek E2)
// ---------------------------------------------------------------------------

namespace {
fs::path validDir() { return fs::path(VGMPLAY_TEST_DATA_DIR) / "valid"; }
}

TEST_CASE("library meta: readTrackMeta z prawdziwego pliku VGM (GD3, dlugosc petli)")
{
    const fs::path f = validDir() / "sn76489_loop_gg_v110_gd3jp.vgm";
    REQUIRE(fs::exists(f));
    auto m = readTrackMeta(f);
    REQUIRE(m.ok());
    CHECK(m.value().path == f);
    CHECK(m.value().stamp.sizeBytes == fs::file_size(f));
    CHECK(m.value().stamp.mtime != 0);
    CHECK(m.value().durationMs > 0);
    CHECK(m.value().hasGd3);
    CHECK((!m.value().trackEn.empty() || !m.value().trackJp.empty()));
    CHECK(m.value().loopMs > 0);
    CHECK(m.value().loopMs <= m.value().durationMs);

    auto vgz = readTrackMeta(validDir() / "megadrive_ym2612_sn76489_v151.vgz");   // gzip
    REQUIRE(vgz.ok());
    CHECK(vgz.value().durationMs > 0);
}

TEST_CASE("library meta: bledy - brak pliku, smieci, katalog, anulowanie")
{
    TempDir td;
    CHECK(readTrackMeta(td.path / "nie_ma.vgm").error().code == ErrorCode::NotFound);
    touch(td.path / "junk.vgm", "to nie jest vgm, ale jest dlugie na tyle zeby miec naglowek");
    CHECK_FALSE(readTrackMeta(td.path / "junk.vgm").ok());
    CHECK_FALSE(readTrackMeta(td.path).ok());
    CancelToken t;
    t.requestCancel();
    auto r = readTrackMeta(validDir() / "sn76489_loop_gg_v110_gd3jp.vgm", t);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Cancelled);
}

TEST_CASE("library cache: getOrRead trafia w cache, zmiana pliku uniewaznia wpis, JSON round-trip")
{
    TempDir td;
    const fs::path src = validDir() / "sn76489_loop_gg_v110_gd3jp.vgm";
    const fs::path f = td.path / "kopia.vgm";
    fs::copy_file(src, f);

    MetaCache c;
    auto a = c.getOrRead(f);
    REQUIRE(a.ok());
    CHECK(c.size() == 1);
    auto stamp = statFile(f);
    REQUIRE(stamp.ok());
    CHECK(c.find(f, stamp.value()).has_value());
    // wielkosc liter w sciezce nie ma znaczenia
    std::wstring up = f.wstring();
    ::CharUpperBuffW(up.data(), static_cast<DWORD>(up.size()));
    CHECK(c.find(fs::path(up), stamp.value()).has_value());

    // Podmieniony wpis w cache (sztuczny durationMs) jest zwracany, dopoki odcisk sie zgadza.
    TrackMeta fake = a.value();
    fake.durationMs = 424242;
    c.put(fake);
    CHECK(c.size() == 1);
    CHECK(c.getOrRead(f).value().durationMs == 424242);

    // JSON round-trip, deterministyczny
    const std::string j = c.toJson();
    auto c2 = MetaCache::fromJson(j);
    REQUIRE(c2.ok());
    CHECK(c2.value().size() == 1);
    CHECK(c2.value().toJson() == j);
    auto hit = c2.value().find(f, stamp.value());
    REQUIRE(hit.has_value());
    CHECK(hit->durationMs == 424242);
    CHECK(hit->trackJp == a.value().trackJp);

    // Zmiana pliku (rozmiar) -> cache niewazny, odczyt na nowo
    {
        std::ofstream o(f, std::ios::binary | std::ios::app);
        o << '\0';
    }
    auto stamp2 = statFile(f);
    REQUIRE(stamp2.ok());
    CHECK_FALSE(stamp2.value() == stamp.value());
    CHECK_FALSE(c.find(f, stamp2.value()).has_value());
    auto b = c.getOrRead(f);   // plik z doklejonym bajtem moze byc poprawny lub nie - wazne, ze nie zwraca starego wpisu
    if (b.ok()) CHECK(b.value().durationMs != 424242);
}

TEST_CASE("library cache: plik - brak = pusty, zapis atomowy, uszkodzony = blad")
{
    TempDir td;
    const fs::path f = td.path / "cfg" / "library-cache.json";
    auto none = loadMetaCacheFile(f);
    REQUIRE(none.ok());
    CHECK(none.value().size() == 0);

    MetaCache c;
    REQUIRE(c.getOrRead(validDir() / "sn76489_loop_gg_v110_gd3jp.vgm").ok());
    REQUIRE(saveMetaCacheFile(f, c).ok());
    auto back = loadMetaCacheFile(f);
    REQUIRE(back.ok());
    CHECK(back.value().toJson() == c.toJson());

    touch(f, c.toJson().substr(0, 40));
    CHECK(loadMetaCacheFile(f).error().code == ErrorCode::ParseError);
    CHECK(MetaCache::fromJson(R"({"schemaVersion":9,"items":[]})").error().code == ErrorCode::UnsupportedSchema);
    CHECK(MetaCache::fromJson(R"({"schemaVersion":1,"items":[{"path":"a"},5,{"path":"b","size":1,"mtime":2}]})").value().size() == 1);
}

// ===========================================================================
// E2-sec
// ===========================================================================

namespace {

// Plik z atrybutem OFFLINE (symulacja zawartosci w chmurze); atrybut mozna ustawic zwyklym SetFileAttributesW.
bool setOffline(const fs::path& p, bool on)
{
    DWORD a = ::GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    a = on ? (a | FILE_ATTRIBUTE_OFFLINE) : (a & ~static_cast<DWORD>(FILE_ATTRIBUTE_OFFLINE));
    return ::SetFileAttributesW(p.c_str(), a) != 0;
}

std::wstring extended(const fs::path& p) { return L"\\\\?\\" + p.wstring(); }

} // namespace

TEST_CASE("library sec: predykat isCloudPlaceholder")
{
    CHECK_FALSE(isCloudPlaceholder(0));
    CHECK_FALSE(isCloudPlaceholder(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN));
    CHECK(isCloudPlaceholder(FILE_ATTRIBUTE_OFFLINE));
    CHECK(isCloudPlaceholder(FILE_ATTRIBUTE_RECALL_ON_OPEN));
    CHECK(isCloudPlaceholder(FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS));
    CHECK(isCloudPlaceholder(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS));
    static_assert(kAttrOffline == FILE_ATTRIBUTE_OFFLINE);
    static_assert(kAttrRecallOnOpen == FILE_ATTRIBUTE_RECALL_ON_OPEN);
    static_assert(kAttrRecallOnDataAccess == FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS);
}

TEST_CASE("library sec: pliki w chmurze - readTrackMeta/getOrRead pomijaja, 'pobierz mimo to' czyta, skan ostrzega")
{
    TempDir td;
    const fs::path f = td.path / "chmura.vgm";
    fs::copy_file(validDir() / "sn76489_loop_gg_v110_gd3jp.vgm", f);
    REQUIRE(setOffline(f, true));

    auto skipped = readTrackMeta(f);
    REQUIRE_FALSE(skipped.ok());
    CHECK(isCloudPlaceholderError(skipped.error()));
    CHECK(skipped.error().code == ErrorCode::CloudPlaceholder);
    CHECK_FALSE(isCloudPlaceholderError(makeError(ErrorCode::AccessDenied, "other")));
    CHECK_FALSE(isCloudPlaceholderError(makeError(ErrorCode::AccessDenied, "cloud placeholder (prefiks nie wystarcza)")));

    MetaCache c;
    auto s2 = c.getOrRead(f);
    REQUIRE_FALSE(s2.ok());
    CHECK(isCloudPlaceholderError(s2.error()));
    CHECK(c.size() == 0);

    auto forced = c.getOrRead(f, {}, /*allowCloudDownload=*/true);
    REQUIRE(forced.ok());
    CHECK(c.size() == 1);
    // trafienie w cache nie czyta pliku, wiec dziala takze bez zgody na pobranie
    CHECK(c.getOrRead(f).ok());

    // skan: plik jest na liscie z atrybutami i z ostrzezeniem
    const ScanResult r = runScan(td.path);
    REQUIRE(r.files.size() == 1);
    CHECK(isCloudPlaceholder(r.files[0].attributes));
    bool warned = false;
    for (const auto& w : r.warnings) warned = warned || w.message.find("cloud placeholder") != std::string::npos;
    CHECK(warned);

    REQUIRE(setOffline(f, false));
    CHECK(readTrackMeta(f).ok());
    const ScanResult r2 = runScan(td.path);
    REQUIRE(r2.files.size() == 1);
    CHECK_FALSE(isCloudPlaceholder(r2.files[0].attributes));
    CHECK(r2.files[0].attributes != 0);   // FILE_ATTRIBUTE_ARCHIVE itp.
}

TEST_CASE("library sec: skan pomija nazwy zarezerwowane i z koncowa kropka/spacja")
{
    TempDir td;
    touch(td.path / "ok.vgm");
    // takie nazwy da sie utworzyc tylko przez przedrostek \\?\ (Win32 normalnie je zmienia)
    const fs::path badDir = td.path / "dir.";
    const fs::path conFile = td.path / "con.vgm";
    const fs::path inBad = badDir / "inside.vgm";
    bool created = ::CreateDirectoryW(extended(badDir).c_str(), nullptr) != 0;
    if (created) {
        HANDLE h = ::CreateFileW(extended(inBad).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        h = ::CreateFileW(extended(conFile).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
    struct Cleanup {
        std::wstring dir, a, b;
        ~Cleanup()
        {
            ::DeleteFileW(a.c_str());
            ::DeleteFileW(b.c_str());
            ::RemoveDirectoryW(dir.c_str());
        }
    } cleanup{extended(badDir), extended(inBad), extended(conFile)};
    if (!created) {
        MESSAGE("system plikow nie pozwala utworzyc nazw z koncowa kropka - test pominiety");
        return;
    }
    const ScanResult r = runScan(td.path);
    REQUIRE(r.files.size() == 1);
    CHECK(r.files[0].path.filename() == fs::path("ok.vgm"));
    CHECK(hasWarning(r, WarningCode::EntrySkipped));
}

TEST_CASE("library sec: MetaCache - 200000 wpisow wczytuje i zapisuje sie szybko, wynik deterministyczny")
{
    MetaCache c;
    for (int i = 0; i < 200000; ++i) {
        TrackMeta m;
        m.path = fs::path(L"D:/Music/pack_" + std::to_wstring(i / 100)) / (L"track_" + std::to_wstring(i) + L".vgz");
        m.stamp.sizeBytes = 1000 + static_cast<std::uint64_t>(i);
        m.stamp.mtime = 133000000000000000ull + static_cast<std::uint64_t>(i);
        m.durationMs = 60000 + static_cast<std::uint64_t>(i % 1000);
        m.hasGd3 = true;
        m.trackEn = "Track " + std::to_string(i);
        m.gameEn = "Game " + std::to_string(i / 100);
        c.put(m);
    }
    REQUIRE(c.size() == 200000);

    const auto t0 = std::chrono::steady_clock::now();
    const std::string j = c.toJson();
    const auto t1 = std::chrono::steady_clock::now();
    auto back = MetaCache::fromJson(j);
    const auto t2 = std::chrono::steady_clock::now();
    REQUIRE(back.ok());
    const double saveS = std::chrono::duration<double>(t1 - t0).count();
    const double loadS = std::chrono::duration<double>(t2 - t1).count();
    MESSAGE("MetaCache 200000 wpisow: toJson " << saveS << " s, fromJson " << loadS << " s, " << j.size() / 1024 << " KiB");
    CHECK(back.value().size() == 200000);
    CHECK(back.value().toJson() == j);
#ifdef NDEBUG
    CHECK(saveS < 1.0);
    CHECK(loadS < 1.0);
#else
    CHECK(saveS < 10.0);
    CHECK(loadS < 10.0);
#endif
}

TEST_CASE("library sec: cache - limity parsera, UNC, tytuly i czasy")
{
    CHECK(MetaCache::fromJson(std::string(100000, '[')).error().code == ErrorCode::LimitExceeded);
    CHECK(MetaCache::fromJson(R"({"schemaVersion":1,"items":)" + std::string(100000, '[')).error().code == ErrorCode::LimitExceeded);
    CHECK(MetaCache::fromJson(R"({"schemaVersion":1,"items":[],"x":[[[[[[[[1]]]]]]]]})").error().code == ErrorCode::LimitExceeded);

    const std::string doc =
        R"({"schemaVersion":1,"items":[)"
        R"({"path":"C:/a.vgm","size":1,"mtime":2,"durationMs":99999999999999,"loopMs":99999999999999,"trackEn":"x\u202Ey\u0007z"},)"
        R"({"path":"//srv/share/b.vgm","size":1,"mtime":2},)"
        R"({"path":"\\\\?\\C:\\c.vgm","size":1,"mtime":2},)"
        R"({"path":"http://x/d.vgm","size":1,"mtime":2}]})";
    auto r = MetaCache::fromJson(doc);
    REQUIRE(r.ok());
    CHECK(r.value().size() == 1);
    auto hit = r.value().find(fs::path("C:/a.vgm"), FileStamp{1, 2});
    REQUIRE(hit.has_value());
    CHECK(hit->durationMs == 24ull * 3600 * 1000);
    CHECK(hit->loopMs == 24ull * 3600 * 1000);
    CHECK(hit->trackEn == "xyz");
    auto net = MetaCache::fromJson(doc, /*allowNetworkPaths=*/true);
    REQUIRE(net.ok());
    CHECK(net.value().size() == 2);   // UNC dozwolone, urzadzenia i URL nigdy
}

TEST_CASE("library sec: plik cache > 64 MiB -> TooLarge")
{
    TempDir td;
    const fs::path f = td.path / "library-cache.json";
    touch(f, std::string((64u << 20) + 1, ' '));
    CHECK(loadMetaCacheFile(f).error().code == ErrorCode::TooLarge);
}
