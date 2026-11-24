// Testy E4: logika wbudowanej przegladarki plikow (bez ImGui) na katalogach tymczasowych - filtrowanie
// rozszerzen, sortowanie (katalogi pierwsze, naturalne), "..", limit wpisow i czasu, UNC bez zgody,
// ostatnie foldery, nazwa eksportu M3U oraz maszyna stanow (nawigacja, zaznaczanie, wyniki).
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "vgmplay_ui/file_browser.h"

using namespace vgmplay::ui;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        static std::atomic<int> n{0};
        path = fs::temp_directory_path() /
               (L"vgmplay_e4_brw_" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"_" +
                std::to_wstring(n++));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void file(const std::wstring& rel, const std::string& content = "x") const
    {
        std::ofstream o(path / rel, std::ios::binary);
        o << content;
    }
    void dir(const std::wstring& rel) const { fs::create_directories(path / rel); }
};

std::string u8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::vector<std::string> names(const Listing& l)
{
    std::vector<std::string> v;
    for (const BrowserEntry& e : l.entries) v.push_back(e.name);
    return v;
}

// Czeka na zakonczenie listowania w FileBrowser (worker + tick); false = przekroczono czas.
bool settle(FileBrowser& b, int timeoutMs = 5000)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(timeoutMs)) {
        b.tick();
        if (!b.loading()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

int indexOf(const FileBrowser& b, const std::string& name)
{
    for (std::size_t i = 0; i < b.entries().size(); ++i)
        if (b.entries()[i].name == name) return static_cast<int>(i);
    return -1;
}

void fillSample(const TempDir& t)
{
    t.dir(L"b_dir");
    t.dir(L"A_dir");
    t.file(L"x10.vgm");
    t.file(L"x2.vgm");
    t.file(L"y.VGZ");
    t.file(L"z.vgm.gz");
    t.file(L"list.m3u");
    t.file(L"list2.M3U8");
    t.file(L"note.txt");
    t.file(L"song.vgm.bak");
}

} // namespace

TEST_SUITE("ui/file_browser")
{
    TEST_CASE("sciezki: lokalne, sieciowe (UNC) i odrzucane; UNC tylko za zgoda")
    {
        CHECK(classifyPath(L"C:\\muzyka") == PathClass::Local);
        CHECK(classifyPath(L"") == PathClass::Local);
        CHECK(classifyPath(L"\\\\serwer\\udzial\\a") == PathClass::Network);
        CHECK(classifyPath(L"//serwer/udzial") == PathClass::Network);
        CHECK(classifyPath(L"\\\\?\\C:\\x") == PathClass::Rejected);
        CHECK(classifyPath(L"\\\\.\\PhysicalDrive0") == PathClass::Rejected);
        CHECK(classifyPath(L"\\\\serwer\\pipe\\x") == PathClass::Rejected);
        CHECK(classifyPath(L"http://example.com/a") == PathClass::Rejected);
        CHECK_FALSE(isPathAllowed(L"\\\\serwer\\udzial", false));
        CHECK(isPathAllowed(L"\\\\serwer\\udzial", true));
        CHECK_FALSE(isPathAllowed(L"\\\\?\\C:\\x", true));   // przestrzen urzadzen nigdy
        CHECK(isPathAllowed(L"C:\\x", false));
    }

    TEST_CASE("filtr rozszerzen w trybach")
    {
        CHECK(matchesMode(L"a.vgm", BrowserMode::AddFiles));
        CHECK(matchesMode(L"a.VGZ", BrowserMode::AddFiles));
        CHECK(matchesMode(L"a.vgm.gz", BrowserMode::AddFiles));
        CHECK(matchesMode(L"A.VGM.GZ", BrowserMode::AddFiles));
        CHECK_FALSE(matchesMode(L"a.gz", BrowserMode::AddFiles));
        CHECK_FALSE(matchesMode(L"a.vgm.bak", BrowserMode::AddFiles));
        CHECK_FALSE(matchesMode(L"a.m3u", BrowserMode::AddFiles));
        CHECK(matchesMode(L"a.m3u", BrowserMode::ImportM3u));
        CHECK(matchesMode(L"a.m3u8", BrowserMode::ImportM3u));
        CHECK_FALSE(matchesMode(L"a.vgm", BrowserMode::ImportM3u));
        CHECK_FALSE(matchesMode(L"a.m3u", BrowserMode::AddFolder));
        CHECK_FALSE(matchesMode(L"a.m3u", BrowserMode::ExportM3u));
        CHECK(listsFiles(BrowserMode::AddFiles));
        CHECK_FALSE(listsFiles(BrowserMode::AddFolder));
        CHECK_FALSE(listsFiles(BrowserMode::ExportM3u));
    }

    TEST_CASE("sortowanie naturalne bez wielkosci liter")
    {
        CHECK(naturalCompare("track2", "track10") < 0);
        CHECK(naturalCompare("track10", "track2") > 0);
        CHECK(naturalCompare("a", "B") < 0);
        CHECK(naturalCompare("B", "a") > 0);
        CHECK(naturalCompare("x01", "x1") != 0);   // rozne nazwy nie sa rowne
        CHECK(naturalCompare("same", "same") == 0);
        CHECK(naturalCompare("a", "ab") < 0);
    }

    TEST_CASE("listowanie: Parent pierwszy, katalogi przed plikami, tylko pasujace rozszerzenia")
    {
        TempDir t;
        fillSample(t);
        ListOptions o;
        o.mode = BrowserMode::AddFiles;
        const Listing l = listDirectory(t.path, o);
        REQUIRE(l.error.empty());
        CHECK_FALSE(l.truncated);
        CHECK(names(l) == std::vector<std::string>{"..", "A_dir", "b_dir", "x2.vgm", "x10.vgm", "y.VGZ", "z.vgm.gz"});
        CHECK(l.entries[0].kind == EntryKind::Parent);
        CHECK(l.entries[0].path == parentOf(t.path));
        CHECK(l.entries[1].kind == EntryKind::Dir);
        CHECK(l.entries[3].kind == EntryKind::File);
        CHECK(l.entries[3].size == 1);

        o.mode = BrowserMode::ImportM3u;
        CHECK(names(listDirectory(t.path, o)) == std::vector<std::string>{"..", "A_dir", "b_dir", "list.m3u", "list2.M3U8"});
        o.mode = BrowserMode::AddFolder;
        CHECK(names(listDirectory(t.path, o)) == std::vector<std::string>{"..", "A_dir", "b_dir"});
        o.mode = BrowserMode::ExportM3u;
        CHECK(names(listDirectory(t.path, o)) == std::vector<std::string>{"..", "A_dir", "b_dir"});
    }

    TEST_CASE("listowanie: nazwy z polskimi i japonskimi znakami (UTF-8)")
    {
        TempDir t;
        t.file(L"za\u017c\u00f3\u0142\u0107.vgm");
        t.dir(L"\u30b2\u30fc\u30e0");
        const Listing l = listDirectory(t.path, ListOptions{});
        REQUIRE(l.entries.size() == 3);
        CHECK(l.entries[1].name == "\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0");
        CHECK(l.entries[2].name == "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87.vgm");
    }

    TEST_CASE("limit wpisow: obciecie z flaga, bledny katalog -> blad bez wyjatku")
    {
        TempDir t;
        for (int i = 0; i < 30; ++i) t.file(L"f" + std::to_wstring(i) + L".vgm");
        ListOptions o;
        o.maxEntries = 10;
        const Listing l = listDirectory(t.path, o);
        CHECK(l.truncated);
        CHECK(l.entries.size() == 11);   // ".." + 10
        CHECK(l.error.empty());

        const Listing bad = listDirectory(t.path / L"nie_ma_takiego", ListOptions{});
        CHECK_FALSE(bad.error.empty());
        REQUIRE(bad.entries.size() == 1);   // zostaje ".." - mozna wyjsc
        CHECK(bad.entries[0].kind == EntryKind::Parent);

        const Listing file = listDirectory(t.path / L"f1.vgm", ListOptions{});   // plik zamiast katalogu
        CHECK_FALSE(file.error.empty());
    }

    TEST_CASE("limit czasu i anulowanie daja wynik czesciowy z flaga")
    {
        TempDir t;
        for (int i = 0; i < 200; ++i) t.file(L"g" + std::to_wstring(i) + L".vgm");
        ListOptions o;
        o.timeLimit = std::chrono::milliseconds(-1);   // zawsze przekroczony
        const Listing l = listDirectory(t.path, o);
        CHECK(l.timedOut);
        CHECK(l.entries.size() < 201);

        std::atomic<bool> cancel{true};
        const Listing c = listDirectory(t.path, ListOptions{}, &cancel);
        CHECK(c.cancelled);

        ListOptions v;
        v.maxVisited = 5;
        CHECK(listDirectory(t.path, v).truncated);
    }

    TEST_CASE("UNC bez zgody: listowanie zablokowane (bez dotykania sieci)")
    {
        ListOptions o;
        o.allowNetwork = false;
        const Listing l = listDirectory(L"\\\\serwer.invalid\\udzial", o);
        CHECK(l.blocked);
        CHECK_FALSE(l.error.empty());
        CHECK(l.entries.empty());
        ListOptions dev;
        dev.allowNetwork = true;
        CHECK(listDirectory(L"\\\\?\\C:\\", dev).blocked);   // przestrzen urzadzen blokowana zawsze
    }

    TEST_CASE("DirectoryLister: ostatnie zadanie wygrywa, wynik odbierany raz")
    {
        TempDir a, b;
        a.file(L"a.vgm");
        b.file(L"b.vgm");
        DirectoryLister lister;
        lister.request(a.path, ListOptions{});
        lister.request(b.path, ListOptions{});
        std::optional<Listing> r;
        for (int i = 0; i < 2500 && !r; ++i) {
            r = lister.poll();
            if (!r) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(r);
        CHECK(r->dir == b.path);
        CHECK(names(*r) == std::vector<std::string>{"..", "b.vgm"});
        CHECK_FALSE(lister.poll());
        CHECK_FALSE(lister.busy());
    }

    TEST_CASE("katalog nadrzedny i poziom korzenia")
    {
        CHECK(isRootLevel(L"C:\\"));
        CHECK(isRootLevel(L"C:"));
        CHECK_FALSE(isRootLevel(L"C:\\muzyka"));
        CHECK(isRootLevel(L"\\\\serwer\\udzial"));
        CHECK(isRootLevel(L"\\\\serwer\\udzial\\"));
        CHECK_FALSE(isRootLevel(L"\\\\serwer\\udzial\\x"));
        CHECK(parentOf(L"C:\\muzyka") == fs::path(L"C:\\"));
        CHECK(parentOf(L"C:\\muzyka\\gry\\") == fs::path(L"C:\\muzyka"));
        CHECK(parentOf(L"C:\\").empty());               // korzen dysku -> lista dyskow
        CHECK(parentOf(L"\\\\serwer\\udzial").empty());
        CHECK(parentOf(L"\\\\serwer\\udzial\\x") == fs::path(L"\\\\serwer\\udzial"));
        CHECK(parentOf(L"").empty());
    }

    TEST_CASE("ostatnie foldery: najnowszy pierwszy, bez duplikatow (bez wielkosci liter), limit")
    {
        std::vector<std::string> r;
        r = pushRecentFolder(r, "C:\\a");
        r = pushRecentFolder(r, "C:\\b");
        r = pushRecentFolder(r, "c:\\A\\");   // ten sam folder inaczej zapisany
        CHECK(r == std::vector<std::string>{"c:\\A\\", "C:\\b"});
        for (int i = 0; i < 20; ++i) r = pushRecentFolder(r, "D:\\f" + std::to_string(i));
        CHECK(r.size() == kMaxRecentFolders);
        CHECK(r.front() == "D:\\f19");
        CHECK(pushRecentFolder(r, "").size() == r.size());
    }

    TEST_CASE("widok 'Ten komputer': ostatnie (bez UNC bez zgody) i dyski")
    {
        const std::vector<std::string> recent = {"C:\\m", "\\\\srv\\share\\x", "http://x/y", "D:\\n"};
        const auto off = rootEntries(recent, false, 0b101);   // A:, C:
        REQUIRE(off.size() == 4);
        CHECK(off[0].kind == EntryKind::Recent);
        CHECK(off[0].name == "C:\\m");
        CHECK(off[1].name == "D:\\n");
        CHECK(off[2].kind == EntryKind::Drive);
        CHECK(off[2].name == "A:\\");
        CHECK(off[3].name == "C:\\");
        const auto on = rootEntries(recent, true, 0);
        REQUIRE(on.size() == 3);   // UNC dozwolone, URL nigdy
        CHECK(on[1].name == "\\\\srv\\share\\x");
    }

    TEST_CASE("nazwa pliku z nazwy playlisty: znaki niedozwolone, zarezerwowane, dlugosc, unikalnosc")
    {
        CHECK(sanitizeFileName("Moja playlista") == "Moja playlista");
        CHECK(sanitizeFileName("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j");
        CHECK(sanitizeFileName("  ..nazwa.. ") == "..nazwa");   // koncowe kropki i spacje znikaja
        CHECK(sanitizeFileName("") == "playlist");
        CHECK(sanitizeFileName("...") == "playlist");
        CHECK(sanitizeFileName("CON") == "_CON");
        CHECK(sanitizeFileName("lpt1.txt") == "_lpt1.txt");
        CHECK(sanitizeFileName("com10") == "com10");
        CHECK(sanitizeFileName(std::string("a\x01" "b")) == "a_b");
        CHECK(sanitizeFileName("x\xE2\x80\xAE" "y") == "x_y");   // U+202E (odwracanie kierunku)
        const std::string longName(300, 'x');
        CHECK(sanitizeFileName(longName).size() == 100);
        std::string jp;
        for (int i = 0; i < 60; ++i) jp += "\xE3\x82\xB2";   // 3 bajty na znak
        const std::string cut = sanitizeFileName(jp);
        CHECK(cut.size() <= 100);
        CHECK(cut.size() % 3 == 0);   // nie ucina w srodku znaku

        TempDir t;
        const fs::path p1 = uniqueFilePath(t.path, "Lista", ".m3u8");
        CHECK(p1 == t.path / L"Lista.m3u8");
        t.file(L"Lista.m3u8");
        const fs::path p2 = uniqueFilePath(t.path, "Lista", ".m3u8");
        CHECK(p2 == t.path / L"Lista (2).m3u8");
        t.file(L"Lista (2).m3u8");
        CHECK(uniqueFilePath(t.path, "Lista", ".m3u8") == t.path / L"Lista (3).m3u8");
    }

    TEST_CASE("FileBrowser: start w ostatnim folderze, nawigacja, '..' i powrot na zaznaczony katalog")
    {
        TempDir t;
        fillSample(t);
        t.file(L"A_dir\\inside.vgm");
        FileBrowser b;
        b.setDriveMaskForTest(0b100);   // C:
        b.open(BrowserMode::AddFiles, {u8(t.path)}, false);
        CHECK(b.isOpen());
        CHECK(b.loading());
        REQUIRE(settle(b));
        CHECK(b.dir() == t.path);
        CHECK(b.selection() == 1);   // pierwszy po ".."
        CHECK(b.entries()[0].kind == EntryKind::Parent);

        b.select(indexOf(b, "A_dir"));
        CHECK(b.activate().kind == BrowserOutcome::Kind::None);
        REQUIRE(settle(b));
        CHECK(b.dir() == t.path / L"A_dir");
        CHECK(names(b.status()).empty());   // status nie trzyma wpisow
        REQUIRE(b.entries().size() == 2);
        CHECK(b.entries()[1].name == "inside.vgm");

        CHECK(b.back().kind == BrowserOutcome::Kind::None);   // w gore
        REQUIRE(settle(b));
        CHECK(b.dir() == t.path);
        CHECK(b.entries()[static_cast<std::size_t>(b.selection())].name == "A_dir");   // wrocilismy na ten sam katalog

        b.select(0);   // ".."
        b.activate();
        REQUIRE(settle(b));
        CHECK(b.dir() == parentOf(t.path));
    }

    TEST_CASE("FileBrowser: pad/klawiatura - B na liscie dyskow zamyka; brak ostatnich -> lista dyskow")
    {
        FileBrowser b;
        b.setDriveMaskForTest(0b1100);
        b.open(BrowserMode::AddFolder, {}, false);
        CHECK(b.atRoot());
        CHECK_FALSE(b.loading());
        REQUIRE(b.entries().size() == 2);
        CHECK(b.entries()[0].name == "C:\\");
        CHECK(b.back().kind == BrowserOutcome::Kind::Close);
        b.close();
        CHECK_FALSE(b.isOpen());

        b.open(BrowserMode::AddFolder, {"Z:\\tego\\nie\\ma\\na\\pewno"}, false);   // znikniety folder
        REQUIRE(settle(b));
        CHECK(b.atRoot());                                        // wracamy do listy dyskow
        REQUIRE(b.entries().size() == 3);                         // zapamietany (znikniety) folder + 2 dyski
        CHECK(b.entries()[0].kind == EntryKind::Recent);
    }

    TEST_CASE("FileBrowser: wybor pliku, zaznaczanie wielu (Y), dodanie biezacego folderu (X)")
    {
        TempDir t;
        t.file(L"a.vgm");
        t.file(L"b.vgm");
        t.file(L"c.vgm");
        t.dir(L"sub");
        FileBrowser b;
        b.open(BrowserMode::AddFiles, {u8(t.path)}, false);
        REQUIRE(settle(b));
        // wpisy: "..", sub, a, b, c
        const int ia = indexOf(b, "a.vgm"), ib = indexOf(b, "b.vgm"), ic = indexOf(b, "c.vgm");
        REQUIRE((ia > 0 && ib > 0 && ic > 0));

        b.select(ib);   // jeden plik bez zaznaczen -> tylko on
        BrowserOutcome one = b.activate();
        REQUIRE(one.kind == BrowserOutcome::Kind::AddPaths);
        REQUIRE(one.paths.size() == 1);
        CHECK(one.paths[0] == t.path / L"b.vgm");
        CHECK(one.folder == t.path);

        b.select(ia);
        b.toggleMark();   // zaznacza a i przechodzi do nastepnego
        CHECK(b.isMarked(ia));
        CHECK(b.selection() == ib);
        b.select(ic);
        BrowserOutcome many = b.activate();   // zaznaczone + biezacy
        REQUIRE(many.kind == BrowserOutcome::Kind::AddPaths);
        REQUIRE(many.paths.size() == 2);
        CHECK(many.paths[0] == t.path / L"a.vgm");
        CHECK(many.paths[1] == t.path / L"c.vgm");
        CHECK(b.markedCount() == 1);

        b.select(indexOf(b, "sub"));   // katalog nie jest zaznaczalny w trybie plikow
        b.toggleMark();
        CHECK(b.markedCount() == 1);

        BrowserOutcome folder = b.useHere();   // X: caly biezacy folder
        REQUIRE(folder.kind == BrowserOutcome::Kind::AddPaths);
        REQUIRE(folder.paths.size() == 1);
        CHECK(folder.paths[0] == t.path);
    }

    TEST_CASE("FileBrowser: tryb folderow - zaznaczone foldery albo biezacy; import M3U; Parent nie jest zaznaczalny")
    {
        TempDir t;
        t.dir(L"g1");
        t.dir(L"g2");
        t.file(L"l.m3u");
        t.file(L"a.vgm");
        FileBrowser f;
        f.open(BrowserMode::AddFolder, {u8(t.path)}, false);
        REQUIRE(settle(f));
        CHECK(indexOf(f, "a.vgm") < 0);   // pliki ukryte
        f.select(0);
        f.toggleMark();
        CHECK(f.markedCount() == 0);       // ".." nie
        f.select(indexOf(f, "g1"));
        f.toggleMark();
        f.select(indexOf(f, "g2"));
        f.toggleMark();
        CHECK(f.markedCount() == 2);
        BrowserOutcome marked = f.useHere();
        REQUIRE(marked.kind == BrowserOutcome::Kind::AddPaths);
        CHECK(marked.paths.size() == 2);
        f.select(indexOf(f, "g1"));
        f.toggleMark();   // odznacz
        f.select(indexOf(f, "g2"));
        f.toggleMark();
        BrowserOutcome here = f.useHere();
        REQUIRE(here.paths.size() == 1);
        CHECK(here.paths[0] == t.path);

        FileBrowser imp;
        imp.open(BrowserMode::ImportM3u, {u8(t.path)}, false);
        REQUIRE(settle(imp));
        imp.select(indexOf(imp, "l.m3u"));
        const BrowserOutcome io = imp.activate();
        REQUIRE(io.kind == BrowserOutcome::Kind::Import);
        CHECK(io.paths.front() == t.path / L"l.m3u");
        CHECK(imp.useHere().kind == BrowserOutcome::Kind::None);   // import nie ma "tutaj"
        imp.toggleMark();
        CHECK(imp.markedCount() == 0);
    }

    TEST_CASE("FileBrowser: eksport M3U zapisuje do biezacego folderu pod nazwa playlisty, bez nadpisywania")
    {
        TempDir t;
        t.dir(L"out");
        FileBrowser b;
        b.open(BrowserMode::ExportM3u, {u8(t.path)}, false, "Moja: lista/1");
        REQUIRE(settle(b));
        CHECK(b.exportTarget() == t.path / L"Moja_ lista_1.m3u8");
        BrowserOutcome o = b.useHere();
        REQUIRE(o.kind == BrowserOutcome::Kind::Export);
        CHECK(o.paths.front() == t.path / L"Moja_ lista_1.m3u8");
        CHECK(o.folder == t.path);

        t.file(L"Moja_ lista_1.m3u8");   // juz istnieje -> kolejna nazwa
        b.reload();
        REQUIRE(settle(b));
        CHECK(b.exportTarget() == t.path / L"Moja_ lista_1 (2).m3u8");

        b.select(indexOf(b, "out"));      // zejscie do podfolderu zmienia cel
        b.activate();
        REQUIRE(settle(b));
        CHECK(b.exportTarget() == t.path / L"out" / L"Moja_ lista_1.m3u8");

        FileBrowser root;
        root.setDriveMaskForTest(0b100);
        root.open(BrowserMode::ExportM3u, {}, false, "x");
        CHECK(root.useHere().kind == BrowserOutcome::Kind::None);   // na liscie dyskow nie ma gdzie zapisac
    }

    TEST_CASE("FileBrowser: UNC bez zgody - nawigacja odrzucona, ostatnie z UNC ukryte; strony i przewijanie")
    {
        FileBrowser b;
        b.setDriveMaskForTest(0b100);
        b.open(BrowserMode::AddFiles, {"\\\\srv\\share\\x"}, false);
        CHECK(b.atRoot());                                  // UNC bez zgody nie jest startem
        for (const BrowserEntry& e : b.entries()) CHECK(e.kind != EntryKind::Recent);
        b.navigate(L"\\\\srv\\share");
        CHECK(b.networkBlocked());
        CHECK(b.atRoot());                                  // zostajemy tam, gdzie bylismy
        b.navigate(L"\\\\?\\C:\\");
        CHECK(b.networkBlocked());

        TempDir t;
        for (int i = 0; i < 25; ++i) t.file(L"s" + std::to_wstring(100 + i) + L".vgm");
        b.open(BrowserMode::AddFiles, {u8(t.path)}, false);
        REQUIRE(settle(b));
        CHECK_FALSE(b.networkBlocked());
        b.setVisibleRows(10);
        CHECK(b.selection() == 1);
        b.page(1);
        CHECK(b.selection() == 10);
        CHECK(b.scrollTop() == 1);
        b.page(1);
        b.page(1);
        b.page(1);
        CHECK(b.selection() == 25);   // ostatni, bez zawijania
        b.page(-1);
        CHECK(b.selection() == 16);
        b.move(-100);
        CHECK(b.selection() == 0);
        CHECK(b.scrollTop() == 0);
        b.scrollBy(100);
        CHECK(b.scrollTop() == 16);   // 26 wpisow - 10 widocznych
    }

    TEST_CASE("FileBrowser: limit wpisow ustawiony z zewnatrz daje komunikat o obcieciu")
    {
        TempDir t;
        for (int i = 0; i < 15; ++i) t.file(L"h" + std::to_wstring(i) + L".vgm");
        FileBrowser b;
        b.setLimitsForTest(5, std::chrono::milliseconds(4000));
        b.open(BrowserMode::AddFiles, {u8(t.path)}, false);
        REQUIRE(settle(b));
        CHECK(b.status().truncated);
        CHECK(b.entries().size() == 6);
    }

    TEST_CASE("sec: sanitizeFileName korzysta z fsutil - CONIN$, CONOUT$, COM0/LPT0, indeksy gorne, rozszerzenia i spacje")
    {
        CHECK(sanitizeFileName("CONIN$") == "_CONIN$");
        CHECK(sanitizeFileName("conout$.txt") == "_conout$.txt");
        CHECK(sanitizeFileName("COM0") == "_COM0");
        CHECK(sanitizeFileName("lpt0") == "_lpt0");
        CHECK(sanitizeFileName("COM9") == "_COM9");
        CHECK(sanitizeFileName("COM\xC2\xB9") == "_COM\xC2\xB9");      // COM + U+00B9
        CHECK(sanitizeFileName("LPT\xC2\xB2.x") == "_LPT\xC2\xB2.x");  // LPT + U+00B2
        CHECK(sanitizeFileName("com\xC2\xB3") == "_com\xC2\xB3");      // com + U+00B3
        CHECK(sanitizeFileName("CON .txt") == "_CON .txt");            // spacja przed kropka nie chroni
        CHECK(sanitizeFileName("nul.tar.gz") == "_nul.tar.gz");
        CHECK(sanitizeFileName("nul. . ") == "_nul");                  // koncowe kropki i spacje znikaja, potem nazwa urzadzenia
        CHECK(sanitizeFileName("AUX  ") == "_AUX");
        CHECK(sanitizeFileName("com10") == "com10");                   // nie nazwa urzadzenia
        CHECK(sanitizeFileName("COM") == "COM");
        CHECK(sanitizeFileName("console") == "console");
        CHECK(sanitizeFileName("CON\xC2\xB9x") == "CON\xC2\xB9x");
    }

    TEST_CASE("sec: cele dowiazan (reparse) - UNC, przestrzen urzadzen, rozszerzone sciezki lokalne")
    {
        CHECK(isNetworkLinkTarget(L"\\??\\UNC\\srv\\share\\x"));
        CHECK(isNetworkLinkTarget(L"\\\\?\\UNC\\srv\\share"));
        CHECK(isNetworkLinkTarget(L"\\\\srv\\share\\x"));
        CHECK(isNetworkLinkTarget(L"//srv/share/x"));
        CHECK(isNetworkLinkTarget(L"\\\\.\\pipe\\x"));
        CHECK_FALSE(isNetworkLinkTarget(L""));
        CHECK_FALSE(isNetworkLinkTarget(L"..\\x"));
        CHECK_FALSE(isNetworkLinkTarget(L"sub\\dir"));
        CHECK_FALSE(isNetworkLinkTarget(L"\\??\\C:\\x"));
        CHECK_FALSE(isNetworkLinkTarget(L"\\\\?\\C:\\x"));
        CHECK_FALSE(isNetworkLinkTarget(L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\x"));
        TempDir t;
        t.dir(L"d");
        t.file(L"f.vgm");
        CHECK_FALSE(linkPointsToNetwork(t.path / L"d"));       // zwykly katalog
        CHECK_FALSE(linkPointsToNetwork(t.path / L"f.vgm"));   // zwykly plik
        CHECK_FALSE(linkPointsToNetwork(t.path / L"nie_ma"));
    }

    TEST_CASE("sec: dyski sieciowe bez zgody - zablokowane (wyszarzone), z zgoda dostepne; reszta bez zmian")
    {
        const auto kind = +[](wchar_t c) { return c == L'Z' ? DriveKind::Network : (c == L'A' ? DriveKind::Removable : DriveKind::Fixed); };
        const std::uint32_t mask = (1u << 0) | (1u << 2) | (1u << 25);   // A: C: Z:
        const auto off = driveEntries(mask, false, kind);
        REQUIRE(off.size() == 3);
        CHECK_FALSE(off[0].blocked);
        CHECK_FALSE(off[1].blocked);
        CHECK(off[2].name == "Z:\\");
        CHECK(off[2].drive == DriveKind::Network);
        CHECK(off[2].blocked);
        const auto on = driveEntries(mask, true, kind);
        REQUIRE(on.size() == 3);
        for (const BrowserEntry& e : on) CHECK_FALSE(e.blocked);
        CHECK(on[2].drive == DriveKind::Network);
    }

    TEST_CASE("sec: zapowiedz nazwy eksportu liczy watek roboczy (Listing::exportTarget), bez I/O w UI")
    {
        TempDir t;
        ListOptions o;
        o.mode = BrowserMode::ExportM3u;
        o.exportStem = "Lista";
        CHECK(listDirectory(t.path, o).exportTarget == t.path / L"Lista.m3u8");
        t.file(L"Lista.m3u8");
        t.file(L"Lista (2).m3u8");
        CHECK(listDirectory(t.path, o).exportTarget == t.path / L"Lista (3).m3u8");
        o.exportStem.clear();   // inne tryby: brak zapowiedzi
        CHECK(listDirectory(t.path, o).exportTarget.empty());
    }

    TEST_CASE("sec: dowiazanie do UNC bez zgody - wpis zablokowany, wejscie odrzucone (bez kontaktu z siecia)")
    {
        TempDir t;
        t.dir(L"local");
        std::error_code ec;
        fs::create_directory_symlink(L"\\\\srv.invalid\\share", t.path / L"netlink", ec);
        if (ec) {
            MESSAGE("brak uprawnien do dowiazan symbolicznych (" << ec.message() << ") - test pominiety");
            return;
        }
        fs::create_directory_symlink(t.path / L"local", t.path / L"locallink", ec);
        ListOptions o;
        o.allowNetwork = false;
        const Listing l = listDirectory(t.path, o);
        REQUIRE(l.error.empty());
        const BrowserEntry* net = nullptr;
        const BrowserEntry* loc = nullptr;
        for (const BrowserEntry& e : l.entries) {
            if (e.name == "netlink") net = &e;
            if (e.name == "locallink") loc = &e;
        }
        REQUIRE(net);
        CHECK(net->kind == EntryKind::Dir);
        CHECK(net->link);
        CHECK(net->blocked);
        if (!ec) {   // dowiazanie lokalne: widoczne, oznaczone jako link, niezablokowane
            REQUIRE(loc);
            CHECK(loc->link);
            CHECK_FALSE(loc->blocked);
        }
        CHECK(linkPointsToNetwork(t.path / L"netlink"));
        const Listing inside = listDirectory(t.path / L"netlink", o);   // wejscie bezposrednie tez zablokowane
        CHECK(inside.blocked);

        FileBrowser b;
        b.setDriveMaskForTest(0b100);
        b.open(BrowserMode::AddFolder, {u8(t.path)}, false);
        REQUIRE(settle(b));
        b.select(indexOf(b, "netlink"));
        (void)b.activate();
        CHECK(b.networkBlocked());
        CHECK(b.dir() == t.path);   // zostajemy w biezacym katalogu

        // Zaznaczanie wpisu zablokowanego jest odrzucane (X nie doda go do playlisty).
        b.select(indexOf(b, "netlink"));
        b.toggleMark();
        CHECK(b.markedCount() == 0);

        // Folder, ktorego listowanie wrocilo `blocked` (wejscie np. z listy ostatnich): ani dodania, ani zapisu.
        for (BrowserMode m : {BrowserMode::AddFiles, BrowserMode::AddFolder, BrowserMode::ExportM3u}) {
            FileBrowser c;
            c.setDriveMaskForTest(0b100);
            c.open(m, {}, false, "Lista");
            c.navigate(t.path / L"netlink");
            REQUIRE(settle(c));
            CHECK(c.status().blocked);
            CHECK(c.useHere().kind == BrowserOutcome::Kind::None);
            CHECK(c.activate().kind == BrowserOutcome::Kind::None);
        }
    }

    TEST_CASE("sec: sprawdzenie sieci idzie skladnik po skladniku - junction lokalny->lokalny dozwolony, dowiazanie do UNC w srodku sciezki zablokowane")
    {
        TempDir t;
        t.dir(L"real/sub");
        t.file(L"real/sub/a.vgm");
        const std::string cmd = "mklink /J \"" + (t.path / L"jn").string() + "\" \"" + (t.path / L"real").string() + "\" >nul 2>nul";
        if (std::system(cmd.c_str()) == 0 && fs::exists(t.path / L"jn")) {
            CHECK_FALSE(isNetworkRedirected(t.path / L"jn"));
            CHECK_FALSE(isNetworkRedirected(t.path / L"jn" / L"sub"));
            CHECK_FALSE(isNetworkRedirected(t.path / L"jn" / L"sub" / L"nie_ma"));
            ListOptions o;
            o.allowNetwork = false;
            const Listing l = listDirectory(t.path / L"jn" / L"sub", o);
            CHECK_FALSE(l.blocked);
            CHECK(l.error.empty());
        } else {
            MESSAGE("nie mozna utworzyc junction (mklink /J) - czesc testu pominieta");
        }
        CHECK_FALSE(isNetworkRedirected(t.path / L"real" / L"sub"));
        CHECK_FALSE(isNetworkRedirected(t.path));

        std::error_code ec;
        fs::create_directory_symlink(L"\\\\srv.invalid\\share", t.path / L"netlink", ec);
        if (ec) {
            MESSAGE("brak uprawnien do dowiazan symbolicznych (" << ec.message() << ") - czesc testu pominieta");
            return;
        }
        // Dowiazanie do UNC jako NIE ostatni skladnik: wykrywane na samym skladniku, bez otwierania pelnej sciezki.
        CHECK(isNetworkRedirected(t.path / L"netlink"));
        CHECK(isNetworkRedirected(t.path / L"netlink" / L"sub" / L"deeper"));
        // Lokalne dowiazanie, ktorego cel prowadzi przez dowiazanie do UNC.
        fs::create_directory_symlink(t.path / L"netlink" / L"x", t.path / L"viaNet", ec);
        if (!ec) CHECK(isNetworkRedirected(t.path / L"viaNet"));
        // Wzgledny cel lokalny.
        fs::create_directory_symlink(L"real", t.path / L"rel", ec);
        if (!ec) CHECK_FALSE(isNetworkRedirected(t.path / L"rel" / L"sub"));
    }

    TEST_CASE("sec: cel dowiazania jako tekst - UNC, mapowany dysk, wolumin lokalny")
    {
        CHECK(isNetworkLinkTarget(L"\\??\\UNC\\srv\\share"));
        CHECK_FALSE(isNetworkLinkTarget(L"\\??\\C:\\Users"));
        CHECK_FALSE(isNetworkLinkTarget(L"\\??\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\"));
        const auto kind = +[](wchar_t c) { return c == L'Z' ? DriveKind::Network : DriveKind::Fixed; };
        const auto on = driveEntries(1u << 25, false, kind);
        REQUIRE(on.size() == 1);
        CHECK(on[0].blocked);
    }

    TEST_CASE("sec: DirectoryLister - limit watkow listujacych: nadmiar czeka (queued) i rusza po zwolnieniu miejsca")
    {
        struct Restore { ~Restore() { DirectoryLister::setMaxThreadsForTest(-1); } } restore;
        TempDir t;
        t.file(L"a.vgm");
        DirectoryLister::setMaxThreadsForTest(0);   // zadne miejsce
        DirectoryLister lister;
        lister.request(t.path, ListOptions{});
        CHECK(lister.queued());
        CHECK(lister.busy());
        CHECK_FALSE(lister.poll());
        CHECK(lister.queued());
        lister.cancel();   // anulowanie czekajacego zadania
        CHECK_FALSE(lister.queued());
        CHECK_FALSE(lister.busy());

        lister.request(t.path, ListOptions{});
        DirectoryLister::setMaxThreadsForTest(DirectoryLister::kMaxListerThreads);   // miejsce sie zwolnilo
        std::optional<Listing> r;
        for (int i = 0; i < 5000 && !r; ++i) {
            r = lister.poll();
            if (!r) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(r);
        CHECK_FALSE(lister.queued());
        CHECK(names(*r) == std::vector<std::string>{"..", "a.vgm"});
        for (int i = 0; i < 2500 && DirectoryLister::activeThreads() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(DirectoryLister::activeThreads() == 0);   // miejsce zwolnione po zakonczeniu
    }

    TEST_CASE("sec: FileBrowser - zapowiedz eksportu z listowania w tle; nic nie jest liczone synchronicznie w tick()")
    {
        TempDir t;
        t.file(L"Plan.m3u8");
        FileBrowser b;
        b.open(BrowserMode::ExportM3u, {u8(t.path)}, false, "Plan");
        CHECK(b.exportTarget().empty());   // przed odebraniem wyniku listowania brak zapowiedzi
        REQUIRE(settle(b));
        CHECK(b.exportTarget() == t.path / L"Plan (2).m3u8");
    }
}
