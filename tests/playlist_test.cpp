// Testy modulu playlist: model, JSON, pliki (zapis atomowy, uszkodzenia), M3U/M3U8.
#include <doctest/doctest.h>

#include <ostream>   // wymagane przez doctest dla stringifikacji std::string_view

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vgmplay/playlist.hpp"

namespace {

using namespace vgmplay;
using namespace vgmplay::playlist;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir()
    {
        static int n = 0;
        path = fs::temp_directory_path() / (L"vgmplay_pl_test_" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(n++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << s;
}

std::vector<Entry> makeEntries(int n, const char* prefix = "t")
{
    std::vector<Entry> v;
    for (int i = 0; i < n; ++i) {
        Entry e;
        e.path = fs::path(L"C:/music") / (std::string(prefix) + std::to_string(i) + ".vgz");
        e.title = std::string(prefix) + std::to_string(i);
        e.durationMs = 1000u * static_cast<unsigned>(i + 1);
        v.push_back(std::move(e));
    }
    return v;
}

std::string title(const PlaylistModel& m, std::size_t list, std::size_t i) { return m.playlist(list).entries[i].title; }

// Tworzy junction (bez uprawnien administratora). false = nie udalo sie.
bool makeJunction(const fs::path& link, const fs::path& target)
{
    const std::string cmd = "mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>nul";
    return std::system(cmd.c_str()) == 0 && fs::exists(link);
}

M3uImportOptions opts(const fs::path& base)
{
    M3uImportOptions o;
    o.baseDir = base;
    return o;
}

} // namespace

// ---------------------------------------------------------------------------
// Model: zbior playlist
// ---------------------------------------------------------------------------

TEST_CASE("playlist: dodawanie, nazwy unikalne bez rozrozniania wielkosci liter")
{
    PlaylistModel m;
    CHECK(m.playlistCount() == 0);
    CHECK(m.addPlaylist("Mega Drive").ok());
    CHECK(m.addPlaylist("mega drive").error().code == ErrorCode::InvalidArgument);
    CHECK(m.addPlaylist("MEGA DRIVE").error().code == ErrorCode::InvalidArgument);
    CHECK(m.addPlaylist("").error().code == ErrorCode::InvalidArgument);
    CHECK(m.addPlaylist("   ").error().code == ErrorCode::InvalidArgument);
    CHECK(m.addPlaylist(std::string(300, 'x')).error().code == ErrorCode::InvalidArgument);
    // Unicode: Ł/ł to ta sama nazwa
    CHECK(m.addPlaylist("Łódź").ok());
    CHECK(m.addPlaylist("łódź").error().code == ErrorCode::InvalidArgument);
    CHECK(m.playlistCount() == 2);
}

TEST_CASE("playlist: zmiana nazwy, przenoszenie i usuwanie playlist; aktywna podaza")
{
    PlaylistModel m;
    for (const char* n : {"A", "B", "C", "D"}) REQUIRE(m.addPlaylist(n).ok());
    CHECK(m.renamePlaylist(1, "b").ok());                      // ta sama nazwa inna wielkoscia: OK
    CHECK(m.renamePlaylist(1, "C").error().code == ErrorCode::InvalidArgument);
    CHECK(m.renamePlaylist(9, "Z").error().code == ErrorCode::InvalidArgument);

    REQUIRE(m.setActive(2).ok());                              // C
    CHECK(m.setActive(4).error().code == ErrorCode::InvalidArgument);
    CHECK(m.movePlaylist(2, 0).ok());                          // C A b D
    CHECK(m.playlist(0).name == "C");
    CHECK(m.activeIndex() == 0);
    CHECK(m.movePlaylist(1, 3).ok());                          // C b D A
    CHECK(m.playlist(3).name == "A");
    CHECK(m.activeIndex() == 0);
    CHECK(m.movePlaylist(0, 4).error().code == ErrorCode::InvalidArgument);

    REQUIRE(m.setActive(3).ok());                              // A
    CHECK(m.removePlaylist(0).ok());                           // b D A
    CHECK(m.activeIndex() == 2);
    CHECK(m.playlist(m.activeIndex()).name == "A");
    CHECK(m.removePlaylist(2).ok());                           // usunieto aktywna: indeks nadal < count
    CHECK(m.activeIndex() < m.playlistCount());
    CHECK(m.removePlaylist(0).ok());
    CHECK(m.removePlaylist(0).ok());
    CHECK(m.playlistCount() == 0);
    CHECK(m.activeIndex() == 0);
    CHECK(m.removePlaylist(0).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("playlist: limit 1000 playlist")
{
    PlaylistModel m;
    for (int i = 0; i < 1000; ++i) REQUIRE(m.addPlaylist("p" + std::to_string(i)).ok());
    CHECK(m.addPlaylist("overflow").error().code == ErrorCode::LimitExceeded);
}

// ---------------------------------------------------------------------------
// Model: wpisy i kursor
// ---------------------------------------------------------------------------

TEST_CASE("playlist: insert/remove - kursor sledzi wpis")
{
    PlaylistModel m;
    REQUIRE(m.addPlaylist("L").ok());
    const auto e = makeEntries(5);
    REQUIRE(m.insert(0, 0, e).ok());
    REQUIRE(m.setCursor(0, 2).ok());                           // t2

    const auto extra = makeEntries(2, "x");
    CHECK(m.insert(0, 0, extra).ok());                         // przed kursorem
    CHECK(m.playlist(0).cursor == 4);
    CHECK(title(m, 0, 4) == "t2");
    CHECK(m.insert(0, 7, extra).ok());                         // za kursorem (na koncu)
    CHECK(m.playlist(0).cursor == 4);
    CHECK(m.insert(0, 4, extra).ok());                         // dokladnie na kursorze: wpis przesuwa sie w prawo
    CHECK(m.playlist(0).cursor == 6);
    CHECK(title(m, 0, 6) == "t2");

    CHECK(m.remove(0, 0, 2).ok());                             // usuwa przed kursorem
    CHECK(m.playlist(0).cursor == 4);
    CHECK(title(m, 0, 4) == "t2");
    CHECK(m.remove(0, 4, 1).ok());                             // usuwa biezacy wpis
    CHECK(m.playlist(0).cursor == -1);

    // zle indeksy
    CHECK(m.insert(0, 100, extra).error().code == ErrorCode::InvalidArgument);
    CHECK(m.insert(3, 0, extra).error().code == ErrorCode::InvalidArgument);
    CHECK(m.remove(0, 0, 1000).error().code == ErrorCode::InvalidArgument);
    CHECK(m.remove(0, 1000, 0).error().code == ErrorCode::InvalidArgument);
    CHECK(m.remove(0, static_cast<std::size_t>(-1), 2).error().code == ErrorCode::InvalidArgument);   // przepelnienie
    CHECK(m.setCursor(0, 1000).error().code == ErrorCode::InvalidArgument);
    CHECK(m.setCursor(0, -2).error().code == ErrorCode::InvalidArgument);
    CHECK(m.setCursor(0, -1).ok());
}

TEST_CASE("playlist: limit 100000 wpisow na liste")
{
    PlaylistModel m;
    REQUIRE(m.addPlaylist("L").ok());
    std::vector<Entry> big(100000);
    for (auto& x : big) x.path = L"a.vgm";
    CHECK(m.insert(0, 0, big).ok());
    Entry one;
    one.path = L"b.vgm";
    CHECK(m.insert(0, 0, std::span<const Entry>(&one, 1)).error().code == ErrorCode::LimitExceeded);
}

TEST_CASE("playlist: move - zakres, kursor w zakresie i poza nim")
{
    PlaylistModel m;
    REQUIRE(m.addPlaylist("L").ok());
    const auto e = makeEntries(6);   // t0..t5
    REQUIRE(m.insert(0, 0, e).ok());

    auto order = [&] {
        std::string s;
        for (const auto& x : m.playlist(0).entries) s += x.title.back();
        return s;
    };
    REQUIRE(m.setCursor(0, 1).ok());                           // t1 w przesuwanym zakresie [1,3)
    CHECK(m.move(0, 1, 2, 3).ok());                            // t0 t3 t4 t1 t2 t5
    CHECK(order() == "034125");
    CHECK(m.playlist(0).cursor == 3);
    CHECK(title(m, 0, 3) == "t1");

    REQUIRE(m.setCursor(0, 5).ok());                           // t5 poza zakresem
    CHECK(m.move(0, 3, 2, 0).ok());                            // t1 t2 t0 t3 t4 t5
    CHECK(order() == "120345");
    CHECK(m.playlist(0).cursor == 5);

    REQUIRE(m.setCursor(0, 0).ok());                           // t1
    CHECK(m.move(0, 4, 2, 0).ok());                            // t4 t5 t1 t2 t0 t3
    CHECK(order() == "451203");
    CHECK(m.playlist(0).cursor == 2);
    CHECK(title(m, 0, 2) == "t1");

    CHECK(m.move(0, 0, 2, 5).error().code == ErrorCode::InvalidArgument);   // to > n-count
    CHECK(m.move(0, 5, 2, 0).error().code == ErrorCode::InvalidArgument);   // zakres poza lista
    CHECK(m.move(0, 0, 0, 3).ok());                                          // pusty zakres: bez zmian
    CHECK(order() == "451203");
}

TEST_CASE("playlist: shuffle deterministyczny, zachowuje zawartosc i kursor")
{
    auto build = [] {
        PlaylistModel m;
        (void)m.addPlaylist("L");
        const auto e = makeEntries(50);
        (void)m.insert(0, 0, e);
        (void)m.setCursor(0, 17);
        return m;
    };
    PlaylistModel a = build(), b = build(), c = build();
    REQUIRE(a.shuffle(0, 12345).ok());
    REQUIRE(b.shuffle(0, 12345).ok());
    REQUIRE(c.shuffle(0, 999).ok());
    bool same = true, diff = false, moved = false;
    for (std::size_t i = 0; i < 50; ++i) {
        same = same && title(a, 0, i) == title(b, 0, i);
        diff = diff || title(a, 0, i) != title(c, 0, i);
        moved = moved || title(a, 0, i) != "t" + std::to_string(i);
    }
    CHECK(same);
    CHECK(diff);
    CHECK(moved);
    std::vector<std::string> titles;
    for (const auto& x : a.playlist(0).entries) titles.push_back(x.title);
    std::sort(titles.begin(), titles.end());
    CHECK(std::adjacent_find(titles.begin(), titles.end()) == titles.end());   // brak duplikatow/strat
    CHECK(title(a, 0, static_cast<std::size_t>(a.playlist(0).cursor)) == "t17");
    CHECK(a.shuffle(5, 1).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("playlist: next/previous z RepeatMode")
{
    PlaylistModel m;
    CHECK(m.next(RepeatMode::All).error().code == ErrorCode::NotFound);   // brak playlist
    REQUIRE(m.addPlaylist("L").ok());
    CHECK(m.next(RepeatMode::All).error().code == ErrorCode::NotFound);   // pusta lista
    const auto e = makeEntries(3);
    REQUIRE(m.insert(0, 0, e).ok());

    SUBCASE("Off")
    {
        CHECK(m.previous(RepeatMode::Off).error().code == ErrorCode::NotFound);   // brak kursora
        CHECK(m.next(RepeatMode::Off).value() == 0);
        CHECK(m.next(RepeatMode::Off).value() == 1);
        CHECK(m.next(RepeatMode::Off).value() == 2);
        CHECK(m.next(RepeatMode::Off).error().code == ErrorCode::NotFound);
        CHECK(m.playlist(0).cursor == 2);                                          // bez zmian po NotFound
        CHECK(m.previous(RepeatMode::Off).value() == 1);
        CHECK(m.previous(RepeatMode::Off).value() == 0);
        CHECK(m.previous(RepeatMode::Off).error().code == ErrorCode::NotFound);
        CHECK(m.playlist(0).cursor == 0);
    }
    SUBCASE("One")
    {
        CHECK(m.next(RepeatMode::One).value() == 0);   // brak kursora -> pierwszy
        CHECK(m.next(RepeatMode::One).value() == 0);   // dalej ten sam
        CHECK(m.previous(RepeatMode::One).value() == 0);
        REQUIRE(m.setCursor(0, 2).ok());
        CHECK(m.next(RepeatMode::One).value() == 2);
    }
    SUBCASE("All")
    {
        CHECK(m.previous(RepeatMode::All).value() == 2);   // brak kursora -> ostatni
        CHECK(m.next(RepeatMode::All).value() == 0);       // zawija
        CHECK(m.previous(RepeatMode::All).value() == 2);   // zawija wstecz
        CHECK(m.next(RepeatMode::All).value() == 0);
        CHECK(m.next(RepeatMode::All).value() == 1);
    }
    SUBCASE("dziala na aktywnej playliscie")
    {
        REQUIRE(m.addPlaylist("L2").ok());
        const auto e2 = makeEntries(2, "q");
        REQUIRE(m.insert(1, 0, e2).ok());
        REQUIRE(m.setActive(1).ok());
        CHECK(m.next(RepeatMode::Off).value() == 0);
        CHECK(m.playlist(0).cursor == -1);
        CHECK(m.playlist(1).cursor == 0);
    }
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

namespace {
PlaylistModel sampleModel()
{
    PlaylistModel m;
    (void)m.addPlaylist("Domyślna");
    (void)m.addPlaylist("日本語のリスト");
    Entry a;
    a.path = fs::path(L"D:/Muzyka/Zażółć gęślą jaźń/01 Wstęp.vgz");
    a.title = "Wstęp – ąęśżźćń";
    a.durationMs = 93123;
    Entry b;
    b.path = fs::path(L"D:/音楽/ゲーム/曲 01.vgm");
    b.title = "テーマ曲";
    Entry c;
    c.path = fs::path(L"relative/dir/plain.vgm");
    const Entry list1[] = {a, b, c};
    (void)m.insert(0, 0, list1);
    const Entry list2[] = {b};
    (void)m.insert(1, 0, list2);
    (void)m.setCursor(0, 1);
    (void)m.setCursor(1, 0);
    (void)m.setActive(1);
    return m;
}
} // namespace

TEST_CASE("playlist JSON: round-trip (polskie i japonskie znaki), deterministyczny")
{
    const PlaylistModel m = sampleModel();
    const std::string j1 = saveJson(m);
    CHECK(j1.find("\"schemaVersion\": 1") != std::string::npos);
    CHECK(j1.find("D:/Muzyka/") != std::string::npos);   // '/' jako separator
    CHECK(j1.find('\\') == std::string::npos);

    auto r = loadJson(j1);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    const PlaylistModel& n = r.value();
    REQUIRE(n.playlistCount() == 2);
    CHECK(n.playlist(0).name == "Domyślna");
    CHECK(n.playlist(1).name == "日本語のリスト");
    REQUIRE(n.playlist(0).entries.size() == 3);
    CHECK(n.playlist(0).entries[0].title == "Wstęp – ąęśżźćń");
    CHECK(n.playlist(0).entries[0].durationMs == 93123);
    CHECK(n.playlist(0).entries[0].path.generic_wstring() == L"D:/Muzyka/Zażółć gęślą jaźń/01 Wstęp.vgz");
    CHECK(n.playlist(0).entries[1].path.generic_wstring() == L"D:/音楽/ゲーム/曲 01.vgm");
    CHECK(n.playlist(0).cursor == 1);
    CHECK(n.playlist(1).cursor == 0);
    CHECK(n.activeIndex() == 1);
    CHECK(saveJson(n) == j1);   // bajt w bajt
}

TEST_CASE("playlist JSON: uszkodzone, obciete i nowsze wersje")
{
    const std::string good = saveJson(sampleModel());
    CHECK(loadJson("").error().code == ErrorCode::ParseError);
    CHECK(loadJson("not json").error().code == ErrorCode::ParseError);
    CHECK(loadJson("[1,2]").error().code == ErrorCode::ParseError);
    CHECK(loadJson("{}").error().code == ErrorCode::ParseError);   // brak schemaVersion
    for (std::size_t cut : {good.size() / 2, good.size() - 3, std::size_t{10}})
        CHECK(loadJson(good.substr(0, cut)).error().code == ErrorCode::ParseError);

    CHECK(loadJson(R"({"schemaVersion":2,"playlists":[]})").error().code == ErrorCode::UnsupportedSchema);
    CHECK(loadJson(R"({"schemaVersion":0,"playlists":[]})").error().code == ErrorCode::ParseError);
    CHECK(loadJson(R"({"schemaVersion":1})").error().code == ErrorCode::ParseError);
    CHECK(loadJson(R"({"schemaVersion":1,"playlists":[{"name":"A"}]})").error().code == ErrorCode::ParseError);
    CHECK(loadJson(R"({"schemaVersion":1,"playlists":[{"name":"A","entries":[]},{"name":"a","entries":[]}]})").error().code ==
          ErrorCode::ParseError);   // duplikat nazwy
    CHECK(loadJson(R"({"schemaVersion":1,"playlists":[{"name":"A","entries":[{"title":"x"}]}]})").error().code == ErrorCode::ParseError);
    CHECK(loadJson(R"({"schemaVersion":1,"playlists":[]})").ok());
    // nieznane pola sa ignorowane
    CHECK(loadJson(R"({"schemaVersion":1,"future":{"x":1},"playlists":[{"name":"A","extra":1,"entries":[]}]})").ok());
    // niepoprawny kursor/aktywna -> wartosci bezpieczne
    auto r = loadJson(R"({"schemaVersion":1,"active":7,"playlists":[{"name":"A","cursor":9,"entries":[{"path":"a.vgm"}]}]})");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r.value().activeIndex() == 0);
    CHECK(r.value().playlist(0).cursor == -1);
}

TEST_CASE("playlist JSON: limity 1000 playlist i 100000 wpisow")
{
    std::string many = R"({"schemaVersion":1,"playlists":[)";
    for (int i = 0; i < 1001; ++i) many += (i ? "," : "") + std::string(R"({"name":"p)") + std::to_string(i) + R"(","entries":[]})";
    many += "]}";
    CHECK(loadJson(many).error().code == ErrorCode::LimitExceeded);

    std::string big = R"({"schemaVersion":1,"playlists":[{"name":"A","entries":[)";
    for (int i = 0; i < 100001; ++i) big += (i ? "," : "") + std::string(R"({"path":"a.vgm"})");
    big += "]}]}";
    CHECK(loadJson(big).error().code == ErrorCode::LimitExceeded);
}

TEST_CASE("playlist JSON: wpisy-URL i sciezki urzadzen sa pomijane")
{
    auto r = loadJson(R"({"schemaVersion":1,"playlists":[{"name":"A","cursor":2,"entries":[
        {"path":"http://evil.example/a.vgm"},{"path":"file:///C:/a.vgm"},{"path":"\\\\?\\C:\\a.vgm"},
        {"path":"C:/ok.vgm"},{"path":"//./PhysicalDrive0"},{"path":""}]}]})");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    REQUIRE(r.value().playlist(0).entries.size() == 1);
    CHECK(r.value().playlist(0).entries[0].path.generic_wstring() == L"C:/ok.vgm");
    CHECK(r.value().playlist(0).cursor == -1);   // kursor wskazywal na pominiety wpis
}

TEST_CASE("playlist JSON: kursor podaza za wpisem po pominieciu wczesniejszych")
{
    auto r = loadJson(R"({"schemaVersion":1,"playlists":[{"name":"A","cursor":2,"entries":[
        {"path":"http://x/a"},{"path":"b.vgm"},{"path":"c.vgm"}]}]})");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r.value().playlist(0).cursor == 1);
    CHECK(r.value().playlist(0).entries[1].path == fs::path("c.vgm"));
}

// ---------------------------------------------------------------------------
// Pliki
// ---------------------------------------------------------------------------

TEST_CASE("playlist plik: zapis atomowy i odczyt, brak plikow tymczasowych")
{
    TempDir td;
    const fs::path f = td.path / "sub" / "playlists.json";   // katalog zostanie utworzony
    const PlaylistModel m = sampleModel();
    REQUIRE(savePlaylistsFile(f, m).ok());
    REQUIRE(savePlaylistsFile(f, m).ok());   // nadpisanie istniejacego
    auto r = loadPlaylistsFile(f);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(saveJson(r.value()) == saveJson(m));
    int files = 0;
    for (const auto& e : fs::directory_iterator(f.parent_path())) {
        (void)e;
        ++files;
    }
    CHECK(files == 1);   // tylko playlists.json
}

TEST_CASE("playlist plik: brak pliku = pusty model")
{
    TempDir td;
    auto r = loadPlaylistsFile(td.path / "nope.json");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r.value().playlistCount() == 0);
}

TEST_CASE("playlist plik: uszkodzony plik -> blad, oryginal nietkniety, kopia .bak")
{
    TempDir td;
    const fs::path f = td.path / "playlists.json";
    const std::string good = saveJson(sampleModel());
    const std::string cut = good.substr(0, good.size() / 2);
    writeFile(f, cut);
    auto r = loadPlaylistsFile(f);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::ParseError);
    CHECK(readFile(f) == cut);
    fs::path bak = f;
    bak += ".bak";
    REQUIRE(fs::exists(bak));
    CHECK(readFile(bak) == cut);

    writeFile(f, std::string("\xFF\xFE\x00\x01 smieci", 12));
    CHECK_FALSE(loadPlaylistsFile(f).ok());
    writeFile(f, R"({"schemaVersion":99,"playlists":[]})");
    CHECK(loadPlaylistsFile(f).error().code == ErrorCode::UnsupportedSchema);
    CHECK(readFile(bak).find("schemaVersion\":99") != std::string::npos);   // nowszy plik tez ma kopie
}

TEST_CASE("playlist plik: zapis do katalogu, ktory jest plikiem -> blad, bez wyjatku")
{
    TempDir td;
    writeFile(td.path / "blocker", "x");
    auto r = savePlaylistsFile(td.path / "blocker" / "p.json", PlaylistModel{});
    CHECK_FALSE(r.ok());
}

// ---------------------------------------------------------------------------
// M3U: import
// ---------------------------------------------------------------------------

TEST_CASE("M3U import: paczka w stylu vgmrips (CRLF, bez BOM, bez EXTM3U, nazwy ze spacjami)")
{
    TempDir td;
    const fs::path pack = td.path / "Castlevania_(IBM_PC_XT_AT)";
    for (const char* n : {"01 Prologue.vgz", "02 Vampire Killer.vgz"}) writeFile(pack / n, "x");
    const std::string m3u = "01 Prologue.vgz\r\n02 Vampire Killer.vgz\r\n";
    writeFile(pack / "Castlevania.m3u", m3u);

    auto r = importM3u(readFile(pack / "Castlevania.m3u"), opts(pack));
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r.value().rejected == 0);
    REQUIRE(r.value().entries.size() == 2);
    CHECK(r.value().entries[0].path == (pack / "01 Prologue.vgz").lexically_normal());
    CHECK(r.value().entries[1].path.filename() == fs::path("02 Vampire Killer.vgz"));
    CHECK(r.value().entries[0].title.empty());
}

TEST_CASE("M3U import: BOM UTF-8, EXTM3U/EXTINF, LF/CR, puste linie, komentarze")
{
    TempDir td;
    const std::string text =
        "\xEF\xBB\xBF#EXTM3U\n"
        "# zwykly komentarz\n"
        "#EXTINF:93,Zażółć gęślą jaźń\n"
        "pl/Zażółć.vgz\r\n"
        "\r\n"
        "#EXTINF:-1,日本語のタイトル\r"
        "jp/曲.vgm\r"
        "#EXTINF:12.5,Ułamek\n"
        "  spaced.vgz  \n"
        "bez_info.vgz\n";
    auto r = importM3u(text, opts(td.path));
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    REQUIRE(r.value().entries.size() == 4);
    CHECK(r.value().entries[0].title == "Zażółć gęślą jaźń");
    CHECK(r.value().entries[0].durationMs == 93000);
    CHECK(r.value().entries[0].path.filename().wstring() == L"Zażółć.vgz");
    CHECK(r.value().entries[1].title == "日本語のタイトル");
    CHECK(r.value().entries[1].durationMs == 0);   // -1 = nieznany
    CHECK(r.value().entries[1].path.filename().wstring() == L"曲.vgm");
    CHECK(r.value().entries[2].durationMs == 12500);
    CHECK(r.value().entries[2].path.filename() == fs::path("spaced.vgz"));
    CHECK(r.value().entries[3].title.empty());     // EXTINF dotyczy tylko nastepnej pozycji
    CHECK(r.value().entries[3].durationMs == 0);
}

TEST_CASE("M3U import: brak BOM - UTF-8 (assumeUtf8) kontra strona kodowa systemu")
{
    TempDir td;
    const std::string utf8 = "Zażółć.vgz\n";
    {
        auto r = importM3u(utf8, opts(td.path));
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
        REQUIRE(r.value().entries.size() == 1);
        CHECK(r.value().entries[0].path.filename().wstring() == L"Zażółć.vgz");
    }
    {
        // Niepoprawny UTF-8 przy assumeUtf8=true: pozycja odrzucona, reszta wczytana.
        auto r = importM3u(std::string("bad\xFF\xFE.vgz\nok.vgz\n"), opts(td.path));
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
        CHECK(r.value().rejected == 1);
        CHECK(r.value().entries.size() == 1);
    }
    {
        M3uImportOptions o = opts(td.path);
        o.assumeUtf8 = false;
        auto r = importM3u(std::string("plain.vgz\n"), o);   // czysty ASCII wszedzie to samo
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
        CHECK(r.value().entries.size() == 1);
        if (::GetACP() == 1250) {
            auto r2 = importM3u(std::string("\x9C" "wiat.vgz\n"), o);   // 0x9C = 's z kreska w cp1250
            REQUIRE_MESSAGE(r2.ok(), (r2.ok() ? std::string() : r2.error().message));
            REQUIRE(r2.value().entries.size() == 1);
            CHECK(r2.value().entries[0].path.filename().wstring() == L"\u015Bwiat.vgz");
        } else {
            MESSAGE("ACP != 1250 (" << ::GetACP() << "): pominieto asercje z polskimi znakami w stronie kodowej");
        }
        // BOM wygrywa z assumeUtf8=false
        auto r3 = importM3u(std::string("\xEF\xBB\xBF") + "Zażółć.vgz\n", o);
        REQUIRE(r3.ok());
        REQUIRE(r3.value().entries.size() == 1);
        CHECK(r3.value().entries[0].path.filename().wstring() == L"Zażółć.vgz");
    }
    // UTF-16 nieobslugiwany
    CHECK(importM3u(std::string("\xFF\xFE" "a\0", 4), opts(td.path)).error().code == ErrorCode::ParseError);
}

TEST_CASE("M3U import: URL, UNC, sciezki urzadzen, strumienie, nazwy zarezerwowane sa odrzucane")
{
    TempDir td;
    M3uImportOptions o = opts(td.path);
    o.allowAbsolutePaths = true;   // nawet gdy absolutne dozwolone
    const std::string text =
        "http://example.com/a.vgm\n"
        "https://example.com/a.vgm\n"
        "file:///C:/Windows/a.vgm\n"
        "ftp://x/a.vgm\n"
        "C://dziwny.vgm\n"
        "\\\\server\\share\\a.vgm\n"
        "//server/share/a.vgm\n"
        "\\\\?\\C:\\a.vgm\n"
        "\\\\.\\PhysicalDrive0\n"
        "//?/C:/a.vgm\n"
        "a.vgm:stream\n"
        "mailto:x@y\n"
        "NUL.vgm\n"
        "sub/COM1\n"
        "dir./a.vgm\n"
        "trailing /a.vgm\n"
        "wild*.vgm\n"
        "C:drive_relative.vgm\n"
        "ok.vgm\n";
    auto r = importM3u(text, o);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r.value().entries.size() == 1);
    CHECK(r.value().rejected == 18);
    CHECK(r.value().warnings.size() >= 18);
    CHECK(r.value().entries[0].path.filename() == fs::path("ok.vgm"));
}

TEST_CASE("M3U import: sciezki absolutne tylko za zgoda i tylko w allowedRoot")
{
    TempDir td;
    const fs::path inside = td.path / "lib" / "a.vgz";
    const fs::path outside = td.path / "other" / "b.vgz";
    const std::string text = inside.string() + "\n" + outside.string() + "\n";

    M3uImportOptions o = opts(td.path / "lib");
    {
        auto r = importM3u(text, o);   // allowAbsolutePaths=false
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
        CHECK(r.value().entries.empty());
        CHECK(r.value().rejected == 2);
    }
    o.allowAbsolutePaths = true;
    {
        auto r = importM3u(text, o);   // root = baseDir
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
        REQUIRE(r.value().entries.size() == 1);
        CHECK(r.value().entries[0].path == inside);
        CHECK(r.value().rejected == 1);
    }
    o.allowedRoot = td.path;
    {
        auto r = importM3u(text, o);   // szerszy root
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
        CHECK(r.value().entries.size() == 2);
    }
    // wielkosc liter w sciezce nie ma znaczenia dla kontroli rootu (Windows)
    {
        std::string up = inside.string();
        std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        M3uImportOptions o2 = opts(td.path / "lib");
        o2.allowAbsolutePaths = true;
        auto r = importM3u(up + "\n", o2);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
        CHECK(r.value().entries.size() == 1);
    }
    // sciezka zaczynajaca sie od '\' (bez dysku) - tez absolutna
    {
        const std::string rooted = inside.string().substr(2) + "\n";   // bez "X:"
        CHECK(importM3u(rooted, opts(td.path / "lib")).value().entries.empty());
        M3uImportOptions o3 = opts(td.path / "lib");
        o3.allowAbsolutePaths = true;
        CHECK(importM3u(rooted, o3).value().entries.size() == 1);
    }
}

TEST_CASE("M3U import: '..' wychodzace poza allowedRoot")
{
    TempDir td;
    fs::create_directories(td.path / "pack" / "disc1");
    const std::string text =
        "../outside.vgz\n"
        "disc1/../../outside2.vgz\n"
        "..\\..\\..\\Windows\\a.vgz\n"
        "disc1/../ok1.vgz\n"
        "./ok2.vgz\n"
        "disc1//ok3.vgz\n";
    auto r = importM3u(text, opts(td.path / "pack"));
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r.value().rejected == 3);
    REQUIRE(r.value().entries.size() == 3);
    CHECK(r.value().entries[0].path == td.path / "pack" / "ok1.vgz");   // znormalizowana, bez '..'
    for (const auto& e : r.value().entries)
        for (const auto& c : e.path) CHECK(c != fs::path(".."));

    // szerszy allowedRoot dopuszcza '..' wewnatrz niego
    M3uImportOptions o = opts(td.path / "pack");
    o.allowedRoot = td.path;
    auto r2 = importM3u("../sibling/a.vgz\n../../x.vgz\n", o);
    REQUIRE_MESSAGE(r2.ok(), (r2.ok() ? std::string() : r2.error().message));
    CHECK(r2.value().entries.size() == 1);
    CHECK(r2.value().rejected == 1);
}

TEST_CASE("M3U import: junction wychodzacy poza root jest odrzucony")
{
    TempDir td;
    fs::create_directories(td.path / "pack");
    fs::create_directories(td.path / "secret");
    writeFile(td.path / "secret" / "s.vgz", "x");
    if (!makeJunction(td.path / "pack" / "link", td.path / "secret")) {
        MESSAGE("nie mozna utworzyc junction (mklink /J) - test pominiety");
        return;
    }
    auto r = importM3u("link/s.vgz\nreal.vgz\n", opts(td.path / "pack"));
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r.value().rejected == 1);
    REQUIRE(r.value().entries.size() == 1);
    CHECK(r.value().entries[0].path.filename() == fs::path("real.vgz"));
}

TEST_CASE("M3U import: limity i argumenty")
{
    TempDir td;
    M3uImportOptions o = opts(td.path);
    o.maxEntries = 3;
    std::string text;
    for (int i = 0; i < 10; ++i) text += "f" + std::to_string(i) + ".vgz\n";
    auto r = importM3u(text, o);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r.value().entries.size() == 3);
    CHECK_FALSE(r.value().warnings.empty());

    CHECK(importM3u("a.vgz\n", M3uImportOptions{}).error().code == ErrorCode::InvalidArgument);   // brak baseDir
    CHECK(importM3u("", opts(td.path)).value().entries.empty());
    CHECK(importM3u("\n\n#EXTM3U\n", opts(td.path)).value().entries.empty());
    // bardzo dluga linia
    auto r2 = importM3u(std::string(200000, 'a') + "\nok.vgz\n", opts(td.path));
    REQUIRE_MESSAGE(r2.ok(), (r2.ok() ? std::string() : r2.error().message));
    CHECK(r2.value().entries.size() == 1);
    CHECK(r2.value().rejected == 1);
}

// ---------------------------------------------------------------------------
// M3U: eksport i round-trip
// ---------------------------------------------------------------------------

TEST_CASE("M3U eksport: M3U8 bez BOM, sciezki wzgledne, EXTINF; round-trip przez import")
{
    TempDir td;
    Playlist pl;
    pl.name = "x";
    Entry a;
    a.path = td.path / L"Gra" / L"01 Wstęp.vgz";
    a.title = "Wstęp";
    a.durationMs = 61500;
    Entry b;
    b.path = td.path / L"Gra" / L"ディスク1" / L"曲.vgm";
    b.title = "曲\r\nz nowa linia";
    Entry c;
    c.path = fs::path(L"Z:/gdzies/indziej/c.vgz");   // poza baseDir: zostaje absolutna
    pl.entries = {a, b, c};

    M3uExportOptions eo;
    eo.baseDir = td.path / "Gra";
    const std::string out = exportM3u(pl, eo);
    CHECK(out.rfind("#EXTM3U\r\n", 0) == 0);           // bez BOM
    CHECK(static_cast<unsigned char>(out[0]) != 0xEF);
    CHECK(out.find("#EXTINF:62,Wstęp\r\n01 Wstęp.vgz\r\n") != std::string::npos);
    CHECK(out.find("ディスク1/曲.vgm\r\n") != std::string::npos);
    CHECK(out.find("#EXTINF:-1,曲  z nowa linia\r\n") != std::string::npos);   // CR/LF w tytule usuniete
    CHECK(out.find("Z:/gdzies/indziej/c.vgz\r\n") != std::string::npos);

    M3uImportOptions io = opts(td.path / "Gra");
    io.allowAbsolutePaths = true;
    io.allowedRoot = td.path;
    auto r2 = importM3u(out, io);
    REQUIRE_MESSAGE(r2.ok(), (r2.ok() ? std::string() : r2.error().message));
    REQUIRE(r2.value().entries.size() == 2);   // c poza rootem
    CHECK(r2.value().entries[0].path == a.path.lexically_normal());
    CHECK(r2.value().entries[0].title == "Wstęp");
    CHECK(r2.value().entries[0].durationMs == 62000);   // zaokraglone do sekund (EXTINF)
    CHECK(r2.value().entries[1].path == b.path.lexically_normal());

    // bez EXTINF
    eo.extInf = false;
    const std::string plain = exportM3u(pl, eo);
    CHECK(plain.find("#EXT") == std::string::npos);
}

TEST_CASE("M3U eksport: strona kodowa systemu (utf8=false) dla ASCII")
{
    Playlist pl;
    Entry a;
    a.path = fs::path(L"C:/m/a.vgz");
    pl.entries = {a};
    M3uExportOptions eo;
    eo.utf8 = false;
    eo.extInf = false;
    CHECK(exportM3u(pl, eo) == "C:/m/a.vgz\r\n");
}

TEST_CASE("M3U: struktura paczki vgmrips w kopii (plik .m3u obok .vgz)")
{
    // Odtwarza uklad realnej paczki (CRLF, spacje w nazwach, nawiasy w nazwie katalogu).
    TempDir td;
    const fs::path pack = td.path / "Akumajo_Dracula_(Sharp_X68000)";
    std::string m3u;
    for (int i = 1; i <= 12; ++i) {
        const std::string n = (i < 10 ? "0" : "") + std::to_string(i) + " Utwór " + std::to_string(i) + ".vgz";
        m3u += n + "\r\n";
    }
    auto r = importM3u(m3u, opts(pack));
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r.value().entries.size() == 12);
    CHECK(r.value().rejected == 0);
    CHECK(r.value().entries[11].path.parent_path() == pack.lexically_normal());
}

// ===========================================================================
// E2-sec: limity parsera, UNC, tytuly, pliki tymczasowe i .bak
// ===========================================================================

namespace {

// Ustawia czas ostatniego zapisu pliku na `secondsAgo` s temu.
bool setAge(const fs::path& p, std::uint64_t secondsAgo)
{
    HANDLE h = ::CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER u{};
    u.LowPart = now.dwLowDateTime;
    u.HighPart = now.dwHighDateTime;
    u.QuadPart -= secondsAgo * 10000000ull;
    FILETIME ft{u.LowPart, static_cast<DWORD>(u.HighPart)};
    const BOOL ok = ::SetFileTime(h, nullptr, nullptr, &ft);
    ::CloseHandle(h);
    return ok != FALSE;
}

// Symlink do pliku bez praw administratora (tryb dewelopera); false = brak uprawnien.
bool makeFileSymlink(const fs::path& link, const fs::path& target)
{
    return ::CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
}

std::string oneEntryJson(const std::string& pathJson, const std::string& extra = "")
{
    return R"({"schemaVersion":1,"playlists":[{"name":"A","entries":[{"path":")" + pathJson + "\"" + extra + "}]}]}";
}

} // namespace

TEST_CASE("playlist JSON sec: 100000 zagniezdzonych '[' konczy sie bledem, bez przepelnienia stosu")
{
    CHECK(loadJson(std::string(100000, '[')).error().code == ErrorCode::LimitExceeded);
    CHECK(loadJson(R"({"schemaVersion":1,"playlists":)" + std::string(100000, '[')).error().code == ErrorCode::LimitExceeded);
    std::string deepObj;
    for (int i = 0; i < 50000; ++i) deepObj += R"({"a":)";
    CHECK(loadJson(deepObj).error().code == ErrorCode::LimitExceeded);
    // glebokosc 9 (root=0) odrzucona, normalna struktura (glebokosc 5) przechodzi
    CHECK(loadJson(R"({"schemaVersion":1,"playlists":[],"x":[[[[[[[[1]]]]]]]]})").error().code == ErrorCode::LimitExceeded);
    CHECK(loadJson(saveJson(sampleModel())).ok());
}

TEST_CASE("playlist JSON sec: limit liczby wartosci przed zbudowaniem calego DOM")
{
    std::string big = R"({"schemaVersion":1,"playlists":[],"x":[)";
    for (int i = 0; i < 4000100; ++i) big += i ? ",0" : "0";
    big += "]}";
    CHECK(loadJson(big).error().code == ErrorCode::LimitExceeded);
}

TEST_CASE("playlist plik sec: plik > 64 MiB -> TooLarge, bez kopii .bak")
{
    TempDir td;
    const fs::path f = td.path / "playlists.json";
    writeFile(f, std::string((64u << 20) + 1, ' '));
    auto r = loadPlaylistsFile(f);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::TooLarge);
    CHECK_FALSE(fs::exists(td.path / "playlists.json.bak"));
}

TEST_CASE("playlist JSON sec: UNC domyslnie odrzucane, opcja allowNetworkPaths; URL i urzadzenia zawsze")
{
    const std::string unc = R"(\\\\serwer\\udzial\\a.vgm)";    // JSON: \\serwer\udzial\a.vgm
    const std::string unc2 = "//serwer/udzial/a.vgm";
    const std::string dev = R"(\\\\?\\C:\\a.vgm)";
    const std::string pipe = R"(\\\\host\\pipe\\x)";
    const std::string uncExt = R"(\\\\?\\UNC\\host\\share\\a.vgm)";
    const std::string url = "http://example.com/a.vgm";

    PlaylistLoadOptions allow;
    allow.allowNetworkPaths = true;
    for (const std::string& p : {unc, unc2}) {
        auto def = loadJson(oneEntryJson(p));
        REQUIRE(def.ok());
        CHECK(def.value().playlist(0).entries.empty());
        auto net = loadJson(oneEntryJson(p), allow);
        REQUIRE(net.ok());
        CHECK(net.value().playlist(0).entries.size() == 1);
    }
    for (const std::string& p : {dev, pipe, uncExt, url}) {
        CHECK(loadJson(oneEntryJson(p)).value().playlist(0).entries.empty());
        CHECK(loadJson(oneEntryJson(p), allow).value().playlist(0).entries.empty());
    }
    // zwykla sciezka nadal przechodzi
    CHECK(loadJson(oneEntryJson("C:/ok.vgm")).value().playlist(0).entries.size() == 1);
}

TEST_CASE("playlist JSON sec: tytuly czyszczone, durationMs przycinane do 24 h")
{
    // U+202E (RLO), U+200B, BEL, TAB, LF w tytule
    const std::string title = R"(,"title":"a\u202Eb\u200Bc\u0007d\te\nf\u2066g")";
    auto r = loadJson(oneEntryJson("a.vgm", title + R"(,"durationMs":99999999999999)"));
    REQUIRE(r.ok());
    const Entry& e = r.value().playlist(0).entries[0];
    CHECK(e.title == "abcd efg");
    CHECK(e.durationMs == 24ull * 3600 * 1000);

    // limit 512 B na granicy znaku (znak 3-bajtowy nie jest ciety)
    std::string longTitle;
    for (int i = 0; i < 400; ++i) longTitle += "\xE6\x9B\xB2";   // U+66F2, 3 B
    auto r2 = loadJson(oneEntryJson("a.vgm", ",\"title\":\"" + longTitle + "\""));
    REQUIRE(r2.ok());
    const std::string& t2 = r2.value().playlist(0).entries[0].title;
    CHECK(t2.size() == 510);   // 170 znakow * 3 B <= 512
    CHECK(t2.size() % 3 == 0);
}

TEST_CASE("M3U sec: wejscie > 16 MiB -> TooLarge; limit linii; tytul EXTINF czyszczony")
{
    TempDir td;
    CHECK(importM3u(std::string((16u << 20) + 1, '\n'), opts(td.path)).error().code == ErrorCode::TooLarge);
    CHECK(importM3u(std::string(16u << 20, '\n'), opts(td.path)).ok());   // dokladnie na limicie

    auto lines = importM3u(std::string(1100000, '\n') + "late.vgz\n", opts(td.path));
    REQUIRE(lines.ok());
    CHECK(lines.value().entries.empty());   // za limitem linii
    bool limitWarn = false;
    for (const auto& w : lines.value().warnings) limitWarn = limitWarn || w.code == WarningCode::LimitReached;
    CHECK(limitWarn);

    // RLO (E2 80 AE), ZWSP (E2 80 8B), BEL i TAB w tytule
    auto r = importM3u(std::string("#EXTINF:10,a\xE2\x80\xAE" "b\xE2\x80\x8B" "c\x07" "d\te\nf.vgz\n"), opts(td.path));
    REQUIRE(r.ok());
    REQUIRE(r.value().entries.size() == 1);
    CHECK(r.value().entries[0].title == "abcd e");
}

TEST_CASE("playlist plik sec: nazwy zarezerwowane i koncowa kropka w M3U odrzucane wspolnym predykatem")
{
    TempDir td;
    auto r = importM3u(std::string("aux.vgz\ndir ./a.vgz\nCOM3.txt\nok.vgz\nLPT0.vgz\n"), opts(td.path));
    REQUIRE(r.ok());
    CHECK(r.value().rejected == 4);   // aux, "dir ." , COM3, LPT0 (COM0/LPT0 tez sa nazwami urzadzen)
    CHECK(r.value().entries.size() == 1);
}

TEST_CASE("playlist plik sec: symlink w miejscu .bak nie przekierowuje kopii")
{
    TempDir td;
    const fs::path f = td.path / "playlists.json";
    const fs::path victim = td.path / "victim.txt";
    writeFile(victim, "WAZNE");
    fs::path bak = f;
    bak += ".bak";
    if (!makeFileSymlink(bak, victim)) {
        MESSAGE("brak uprawnien do tworzenia dowiazan symbolicznych - test pominiety");
        return;
    }
    writeFile(f, "{ to nie jest json");
    auto r = loadPlaylistsFile(f);
    REQUIRE_FALSE(r.ok());
    CHECK(readFile(victim) == "WAZNE");                      // cel dowiazania nietkniety
    CHECK(readFile(bak) == "{ to nie jest json");            // .bak to teraz zwykly plik z kopia
    CHECK((::GetFileAttributesW(bak.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
}

TEST_CASE("playlist plik sec: osierocone pliki tymczasowe sprzatane tylko wg wlasnego wzorca i wieku")
{
    TempDir td;
    const fs::path f = td.path / "playlists.json";
    const std::string hex = "0123456789abcdef0123456789abcdef";
    const fs::path oldHex = td.path / ("playlists.json.tmp" + hex);
    const fs::path oldLegacy = td.path / "playlists.json.tmp123_4";
    const fs::path newHex = td.path / "playlists.json.tmpfedcba9876543210fedcba9876543210";
    const fs::path foreign = td.path / "playlists.json.tmpNOTOURS";
    const fs::path otherFile = td.path / ("other.json.tmp" + hex);
    const fs::path shortHex = td.path / "playlists.json.tmpabc";
    for (const auto& p : {oldHex, oldLegacy, newHex, foreign, otherFile, shortHex}) writeFile(p, "x");
    for (const auto& p : {oldHex, oldLegacy, foreign, otherFile, shortHex}) REQUIRE(setAge(p, 2 * 24 * 3600));

    REQUIRE(savePlaylistsFile(f, sampleModel()).ok());
    CHECK_FALSE(fs::exists(oldHex));
    CHECK_FALSE(fs::exists(oldLegacy));
    CHECK(fs::exists(newHex));      // swiezy - moze nalezec do trwajacego zapisu
    CHECK(fs::exists(foreign));     // obcy wzorzec
    CHECK(fs::exists(otherFile));   // inny plik
    CHECK(fs::exists(shortHex));
    // zapis nie zostawia wlasnych plikow tymczasowych
    int tmps = 0;
    for (const auto& e : fs::directory_iterator(td.path))
        if (e.path().filename().string().rfind("playlists.json.tmp", 0) == 0) ++tmps;
    CHECK(tmps == 3);   // newHex, foreign, shortHex
}
