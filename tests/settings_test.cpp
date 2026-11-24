// Testy modulu settings: domyslne wartosci, JSON, migracje, lokalizacja pliku, zapis atomowy.
#include <doctest/doctest.h>

#include <ostream>   // wymagane przez doctest dla stringifikacji std::string_view

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vgmplay/settings.hpp"

namespace {

using namespace vgmplay;
using namespace vgmplay::settings;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir()
    {
        static int n = 0;
        path = fs::temp_directory_path() / (L"vgmplay_st_test_" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(n++));
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

Settings customSettings()
{
    Settings s;
    s.audio.device = "{0.0.0.00000000}.{abc-ąę}";
    s.audio.bufferMs = 80;
    s.audio.sampleRate = 44100;
    s.playback.loopCount = 5;
    s.playback.fadeMs = 2500;
    s.playback.volume = 0.35f;
    s.playback.nonLoopEnd = NonLoopEnd::FadeOut;
    s.playback.cores = {CoreChoice{ChipType::YM2612, 0, 0x4E554B44}, CoreChoice{ChipType::SN76496, 1, 7}};
    s.ui.language = i18n::Language::En;
    s.ui.gd3Language = format::Gd3Language::Jp;
    s.ui.theme = Theme::Light;
    s.ui.fontScale = 1.75f;
    s.ui.scopeMode = ScopeMode::PerDevice;
    s.ui.trackerView = TrackerView::Scopes;
    s.ui.rowTicks = 735;
    s.input.bindings = {input::Binding{input::Action::PlayPause, {input::SourceKind::Keyboard, 44}},
                        input::Binding{input::Action::Confirm, {input::SourceKind::GamepadButton, 0}},
                        input::Binding{input::Action::VolumeUp, {input::SourceKind::GamepadAxisPositive, 3}},
                        input::Binding{input::Action::VolumeDown, {input::SourceKind::GamepadAxisNegative, 3}}};
    s.input.repeatDelayMs = 300;
    s.input.repeatRateMs = 40;
    s.session.trackPath = "D:/Muzyka/日本/曲 01.vgz";
    s.session.positionMs = 123456;
    return s;
}

bool hasWarning(const std::vector<Warning>& w, const std::string& needle)
{
    for (const auto& x : w)
        if (x.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("settings: wartosci domyslne zgodne z decyzjami uzytkownika")
{
    const Settings s;
    CHECK(s.schemaVersion == kSettingsSchemaVersion);
    CHECK(s.ui.language == i18n::Language::Pl);
    CHECK(s.ui.gd3Language == format::Gd3Language::En);
    CHECK(s.ui.rowTicks == 2205);
    CHECK(s.playback.loopCount == 2);
    CHECK(s.playback.fadeMs == 1000);
    CHECK(s.playback.nonLoopEnd == NonLoopEnd::Stop);
    CHECK(s.ui.scopeMode == ScopeMode::Master);
    CHECK(s.session.trackPath.empty());
    CHECK(s.session.positionMs == 0);
    // Pusty obiekt JSON daje te same wartosci domyslne.
    auto r = fromJson("{}");
    REQUIRE(r.ok());
    CHECK(toJson(r.value()) == toJson(s));
}

TEST_CASE("settings JSON: round-trip wszystkich pol, deterministyczny")
{
    const Settings s = customSettings();
    const std::string j1 = toJson(s);
    std::vector<Warning> w;
    auto r = fromJson(j1, &w);
    REQUIRE(r.ok());
    CHECK(w.empty());
    const Settings& t = r.value();
    CHECK(t.audio.device == s.audio.device);
    CHECK(t.audio.bufferMs == 80);
    CHECK(t.audio.sampleRate == 44100);
    CHECK(t.playback.loopCount == 5);
    CHECK(t.playback.fadeMs == 2500);
    CHECK(t.playback.volume == doctest::Approx(0.35f));
    CHECK(t.playback.nonLoopEnd == NonLoopEnd::FadeOut);
    REQUIRE(t.playback.cores.size() == 2);
    CHECK(t.playback.cores[0] == s.playback.cores[0]);
    CHECK(t.playback.cores[1] == s.playback.cores[1]);
    CHECK(t.ui.language == i18n::Language::En);
    CHECK(t.ui.gd3Language == format::Gd3Language::Jp);
    CHECK(t.ui.theme == Theme::Light);
    CHECK(t.ui.fontScale == doctest::Approx(1.75f));
    CHECK(t.ui.scopeMode == ScopeMode::PerDevice);
    CHECK(t.ui.trackerView == TrackerView::Scopes);
    CHECK(t.ui.rowTicks == 735);
    CHECK(t.input.bindings == s.input.bindings);
    CHECK(t.input.repeatDelayMs == 300);
    CHECK(t.input.repeatRateMs == 40);
    CHECK(t.session.trackPath == s.session.trackPath);
    CHECK(t.session.positionMs == 123456);
    CHECK(toJson(t) == j1);   // bajt w bajt
}

TEST_CASE("settings JSON: wartosci domyslne wolne od bledow zaokraglen (volume 0.8)")
{
    const std::string j = toJson(Settings{});
    CHECK(j.find("\"volume\": 0.8\n") != std::string::npos);
    CHECK(fromJson(j).value().playback.volume == 0.8f);
}

TEST_CASE("settings JSON: nieznane pola ignorowane, brakujace domyslne")
{
    std::vector<Warning> w;
    auto r = fromJson(R"({"schemaVersion":1,"futureThing":{"a":[1,2,3]},"audio":{"bufferMs":120,"newField":true},"ui":{"theme":"light"}})", &w);
    REQUIRE(r.ok());
    CHECK(r.value().audio.bufferMs == 120);
    CHECK(r.value().ui.theme == Theme::Light);
    CHECK(r.value().audio.sampleRate == 48000);
    CHECK(r.value().playback.loopCount == 2);
    CHECK(w.empty());
}

TEST_CASE("settings JSON: zakresy przycinane z ostrzezeniem")
{
    std::vector<Warning> w;
    auto r = fromJson(R"({"schemaVersion":1,
        "audio":{"bufferMs":1,"sampleRate":100},
        "playback":{"volume":7.5,"loopCount":5000,"fadeMs":999999},
        "ui":{"fontScale":0.01,"rowTicks":5},
        "input":{"repeatDelayMs":1,"repeatRateMs":99999}})", &w);
    REQUIRE(r.ok());
    const Settings& s = r.value();
    CHECK(s.audio.bufferMs == 10);
    CHECK(s.audio.sampleRate == 8000);
    CHECK(s.playback.volume == 1.0f);
    CHECK(s.playback.loopCount == 99);
    CHECK(s.playback.fadeMs == 30000);
    CHECK(s.ui.fontScale == 0.5f);
    CHECK(s.ui.rowTicks == 64);
    CHECK(s.input.repeatDelayMs == 100);
    CHECK(s.input.repeatRateMs == 1000);
    CHECK(hasWarning(w, "audio.bufferMs"));
    CHECK(hasWarning(w, "playback.volume"));
    CHECK(hasWarning(w, "ui.fontScale"));
    CHECK(hasWarning(w, "input.repeatRateMs"));
    // sampleRate 0 = czestotliwosc urzadzenia, wartosc dozwolona
    CHECK(fromJson(R"({"audio":{"sampleRate":0}})").value().audio.sampleRate == 0);
    // przyciete: wartosc nad limit uint32 nie zawija sie
    CHECK(fromJson(R"({"audio":{"bufferMs":4294967297}})").value().audio.bufferMs == 500);
}

TEST_CASE("settings JSON: zle typy i nieznane wartosci -> domyslne + ostrzezenie, bez bledu")
{
    std::vector<Warning> w;
    auto r = fromJson(R"({"schemaVersion":1,
        "audio":{"bufferMs":"szybko","device":5},
        "playback":{"volume":"glosno","nonLoopEnd":"explode","cores":{"x":1}},
        "ui":{"language":"de","theme":3,"scopeMode":"nope"},
        "input":{"bindings":[{"action":"nonsense","source":"key","code":1},{"action":"confirm","source":"key"},
                             {"action":"confirm","source":"key","code":2},5,null],"repeatDelayMs":-5},
        "session":"oops"})", &w);
    REQUIRE(r.ok());
    const Settings d;
    CHECK(r.value().audio.bufferMs == d.audio.bufferMs);
    CHECK(r.value().playback.nonLoopEnd == NonLoopEnd::Stop);
    CHECK(r.value().ui.language == i18n::Language::Pl);
    CHECK(r.value().input.repeatDelayMs == d.input.repeatDelayMs);
    REQUIRE(r.value().input.bindings.size() == 1);
    CHECK(r.value().input.bindings[0] == input::Binding{input::Action::Confirm, {input::SourceKind::Keyboard, 2}});
    CHECK(w.size() >= 8);
    // sekcja o zlym typie
    CHECK(fromJson(R"({"audio":[1,2]})").ok());
}

TEST_CASE("settings JSON: uszkodzone i obciete wejscie -> ParseError")
{
    const std::string good = toJson(customSettings());
    CHECK(fromJson("").error().code == ErrorCode::ParseError);
    CHECK(fromJson("nie json").error().code == ErrorCode::ParseError);
    CHECK(fromJson("[]").error().code == ErrorCode::ParseError);
    CHECK(fromJson("\"x\"").error().code == ErrorCode::ParseError);
    CHECK(fromJson(good.substr(0, good.size() / 2)).error().code == ErrorCode::ParseError);
    CHECK(fromJson(good.substr(0, good.size() - 2)).error().code == ErrorCode::ParseError);
    CHECK(fromJson(R"({"schemaVersion":"1"})").error().code == ErrorCode::ParseError);
    CHECK(fromJson(R"({"schemaVersion":-1})").error().code == ErrorCode::ParseError);
    CHECK(fromJson(R"({"schemaVersion":1.5})").error().code == ErrorCode::ParseError);
}

TEST_CASE("settings JSON: migracja schematu")
{
    // wersja 0 (brak pola schemaVersion lub 0) -> migracja do 1 z ostrzezeniem
    for (const char* doc : {R"({"ui":{"theme":"light"}})", R"({"schemaVersion":0,"ui":{"theme":"light"}})"}) {
        std::vector<Warning> w;
        auto r = fromJson(doc, &w);
        REQUIRE(r.ok());
        CHECK(r.value().ui.theme == Theme::Light);
        CHECK(r.value().schemaVersion == kSettingsSchemaVersion);
        CHECK(hasWarning(w, "migrated"));
        CHECK(toJson(r.value()).find("\"schemaVersion\": 1") != std::string::npos);
    }
    // nowszy schemat -> UnsupportedSchema
    CHECK(fromJson(R"({"schemaVersion":2})").error().code == ErrorCode::UnsupportedSchema);
    CHECK(fromJson(R"({"schemaVersion":4294967296})").error().code == ErrorCode::UnsupportedSchema);
    // aktualny: bez ostrzezenia o migracji
    std::vector<Warning> w;
    REQUIRE(fromJson(R"({"schemaVersion":1})", &w).ok());
    CHECK(w.empty());
}

TEST_CASE("settings JSON: BOM UTF-8 na poczatku jest tolerowany")
{
    CHECK(fromJson("\xEF\xBB\xBF{\"ui\":{\"theme\":\"light\"}}").value().ui.theme == Theme::Light);
}

// ---------------------------------------------------------------------------
// Lokalizacja
// ---------------------------------------------------------------------------

TEST_CASE("settings lokalizacja: folder zapisywalny -> tryb przenosny obok exe")
{
    TempDir td;
    const fs::path exe = td.path / "app";
    fs::create_directories(exe);
    const fs::path appdata = td.path / "AppData";
    const auto loc = resolveSettingsLocation(exe, appdata);
    CHECK(loc.portable);
    CHECK(loc.file == exe / "settings.json");
    CHECK(isDirectoryWritable(exe));
    // proba zapisu nie zostawia sladow
    CHECK(fs::is_empty(exe));
    CHECK_FALSE(fs::exists(appdata));
}

TEST_CASE("settings lokalizacja: folder niedostepny do zapisu -> %APPDATA%\\vgmplay (sciezki wstrzykniete)")
{
    TempDir td;
    const fs::path appdata = td.path / "AppData";
    SUBCASE("katalog nie istnieje")
    {
        const auto loc = resolveSettingsLocation(td.path / "brak", appdata);
        CHECK_FALSE(loc.portable);
        CHECK(loc.file == appdata / "ChipJuke" / "settings.json");
        CHECK_FALSE(isDirectoryWritable(td.path / "brak"));
        CHECK_FALSE(fs::exists(td.path / "brak"));   // nie tworzy katalogu
    }
    SUBCASE("sciezka jest plikiem")
    {
        writeFile(td.path / "plik", "x");
        const auto loc = resolveSettingsLocation(td.path / "plik", appdata);
        CHECK_FALSE(loc.portable);
        CHECK(loc.file == appdata / "ChipJuke" / "settings.json");
    }
    SUBCASE("pusta sciezka")
    {
        CHECK_FALSE(isDirectoryWritable({}));
        CHECK_FALSE(resolveSettingsLocation({}, appdata).portable);
    }
    SUBCASE("katalog z odmowa zapisu (ACL)")
    {
        const fs::path ro = td.path / "ro";
        fs::create_directories(ro);
        wchar_t sys[MAX_PATH] = {};
        ::GetSystemDirectoryW(sys, MAX_PATH);
        const std::string icacls = fs::path(sys).string() + "/icacls.exe";
        // Zewnetrzne cudzyslowy: cmd /c zdejmuje pierwszy i ostatni, zostawiajac cytowane argumenty.
        const std::string deny = "\"\"" + icacls + "\" \"" + ro.string() + "\" /deny *S-1-1-0:(OI)(CI)(WD,AD) >nul 2>nul\"";
        const std::string undo = "\"\"" + icacls + "\" \"" + ro.string() + "\" /remove:d *S-1-1-0 >nul 2>nul\"";
        if (std::system(deny.c_str()) != 0 || isDirectoryWritable(ro)) {
            (void)std::system(undo.c_str());
            MESSAGE("nie udalo sie odebrac praw zapisu (icacls) - podtest pominiety");
        } else {
            const auto loc = resolveSettingsLocation(ro, appdata);
            CHECK_FALSE(loc.portable);
            CHECK(loc.file == appdata / "ChipJuke" / "settings.json");
            CHECK(std::system(undo.c_str()) == 0);   // zeby TempDir mogl posprzatac
            CHECK(isDirectoryWritable(ro));
        }
    }
    SUBCASE("brak appData i niezapisywalny exe: nadal zwraca sciezke, nie przenosna")
    {
        const auto loc = resolveSettingsLocation(td.path / "brak", {});
        CHECK_FALSE(loc.portable);
        CHECK(loc.file.filename() == fs::path("settings.json"));
    }
}

// ---------------------------------------------------------------------------
// Pliki
// ---------------------------------------------------------------------------

TEST_CASE("settings plik: brak pliku -> domyslne ustawienia (nie blad)")
{
    TempDir td;
    auto r = loadSettings(td.path / "nie" / "ma.json");
    REQUIRE(r.ok());
    CHECK(toJson(r.value()) == toJson(Settings{}));
}

TEST_CASE("settings plik: zapis tworzy katalogi, jest atomowy i odczytywalny")
{
    TempDir td;
    const fs::path f = td.path / "AppData" / "vgmplay" / "settings.json";
    const Settings s = customSettings();
    REQUIRE(saveSettings(f, s).ok());
    REQUIRE(saveSettings(f, s).ok());   // nadpisanie
    auto r = loadSettings(f);
    REQUIRE(r.ok());
    CHECK(toJson(r.value()) == toJson(s));
    int n = 0;
    for (const auto& e : fs::directory_iterator(f.parent_path())) {
        (void)e;
        ++n;
    }
    CHECK(n == 1);   // brak plikow *.tmp
}

TEST_CASE("settings plik: uszkodzony plik -> blad, oryginal zostaje, kopia .bad")
{
    TempDir td;
    const fs::path f = td.path / "settings.json";
    const std::string good = toJson(customSettings());
    const std::string cut = good.substr(0, good.size() / 3);
    writeFile(f, cut);
    auto r = loadSettings(f);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::ParseError);
    CHECK(readFile(f) == cut);
    fs::path bad = f;
    bad += ".bad";
    REQUIRE(fs::exists(bad));
    CHECK(readFile(bad) == cut);
    // po bledzie wywolujacy moze zapisac domyslne - kopia .bad pozostaje
    REQUIRE(saveSettings(f, Settings{}).ok());
    CHECK(readFile(bad) == cut);
    CHECK(loadSettings(f).ok());
}

TEST_CASE("settings plik: katalog zamiast pliku, zbyt duzy plik, zapis w niedostepne miejsce")
{
    TempDir td;
    fs::create_directories(td.path / "dir.json");
    CHECK_FALSE(loadSettings(td.path / "dir.json").ok());

    writeFile(td.path / "big.json", std::string(5u << 20, ' '));
    auto r = loadSettings(td.path / "big.json");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::TooLarge);

    writeFile(td.path / "blocker", "x");
    CHECK_FALSE(saveSettings(td.path / "blocker" / "settings.json", Settings{}).ok());
}

// ===========================================================================
// E2-sec
// ===========================================================================

TEST_CASE("settings sec: 100000 zagniezdzonych '[' i glebokosc > 8 -> LimitExceeded")
{
    CHECK(fromJson(std::string(100000, '[')).error().code == ErrorCode::LimitExceeded);
    CHECK(fromJson(R"({"audio":)" + std::string(100000, '[')).error().code == ErrorCode::LimitExceeded);
    CHECK(fromJson(R"({"a":[[[[[[[[1]]]]]]]]})").error().code == ErrorCode::LimitExceeded);   // glebokosc 9
    CHECK(fromJson(toJson(customSettings())).ok());
}

TEST_CASE("settings sec: limit liczby wartosci")
{
    std::string big = R"({"x":[)";
    for (int i = 0; i < 60000; ++i) big += i ? ",0" : "0";
    big += "]}";
    CHECK(fromJson(big).error().code == ErrorCode::LimitExceeded);
}

TEST_CASE("settings sec: session.trackPath walidowany jak wpisy playlist")
{
    auto load = [](const std::string& path, bool allowNet, std::vector<Warning>* w) {
        return fromJson(R"({"session":{"trackPath":")" + path + R"(","positionMs":5}})", w, allowNet);
    };
    {
        std::vector<Warning> w;
        auto r = load(R"(\\\\srv\\share\\a.vgm)", false, &w);   // UNC (JSON: dwa backslashe na separator)
        REQUIRE(r.ok());
        CHECK(r.value().session.trackPath.empty());
        CHECK(r.value().session.positionMs == 5);
        CHECK(hasWarning(w, "session.trackPath"));
    }
    {
        auto r = load(R"(\\\\srv\\share\\a.vgm)", true, nullptr);   // allowNetworkPaths
        REQUIRE(r.ok());
        CHECK_FALSE(r.value().session.trackPath.empty());
    }
    for (const char* bad : {"http://x/a.vgm", R"(\\\\?\\C:\\a.vgm)", R"(\\\\host\\pipe\\p)"}) {
        auto r = load(bad, true, nullptr);
        REQUIRE(r.ok());
        CHECK(r.value().session.trackPath.empty());
    }
    CHECK(load("D:/Muzyka/a.vgz", false, nullptr).value().session.trackPath == "D:/Muzyka/a.vgz");
    CHECK(fromJson(R"({"session":{"trackPath":""}})").value().session.trackPath.empty());
}

TEST_CASE("settings sec: recentFolders i session.trackPath wymagaja sciezki bezwzglednej")
{
    // Wzgledne, zalezne od biezacego katalogu i "C:foo" (katalog biezacy dysku) sa odrzucane z ostrzezeniem.
    std::vector<Warning> w;
    auto r = fromJson(
        R"({"library":{"recentFolders":["muzyka\\gry","..\\x","\\x","C:foo","C:\\ok","D:/ok2","\\\\srv\\share\\x"]},)"
        R"("session":{"trackPath":"rel\\a.vgm","positionMs":7}})",
        &w, true);
    REQUIRE(r.ok());
    CHECK(r.value().library.recentFolders == std::vector<std::string>{"C:\\ok", "D:/ok2", "\\\\srv\\share\\x"});
    CHECK(hasWarning(w, "recentFolders"));
    CHECK(r.value().session.trackPath.empty());
    CHECK(r.value().session.positionMs == 7);
    CHECK(hasWarning(w, "session.trackPath"));
    for (const char* bad : {"C:a.vgm", R"(\\a.vgm)", "a.vgm", "./a.vgm"}) {
        auto b = fromJson(std::string(R"({"session":{"trackPath":")") + bad + R"("}})", nullptr, true);
        REQUIRE(b.ok());
        CHECK(b.value().session.trackPath.empty());
    }
    auto ok = fromJson(R"({"session":{"trackPath":"C:\\Muzyka\\a.vgz"}})", nullptr, false);
    REQUIRE(ok.ok());
    CHECK(ok.value().session.trackPath == "C:\\Muzyka\\a.vgz");
}

TEST_CASE("settings sec: appDataDir UNC lub rozszerzony jest odrzucany")
{
    TempDir td;
    const fs::path bad = td.path / "brak";   // exeDir niezapisywalny (nie istnieje)
    for (const wchar_t* ad : {L"\\\\serwer\\udzial\\AppData", L"//serwer/udzial/AppData", L"\\\\?\\C:\\Users\\x\\AppData"}) {
        const auto loc = resolveSettingsLocation(bad, fs::path(ad));
        CHECK_FALSE(loc.portable);
        CHECK(loc.file == bad / "settings.json");
    }
    // zwykly appData nadal dziala
    CHECK(resolveSettingsLocation(bad, td.path / "AppData").file == td.path / "AppData" / "ChipJuke" / "settings.json");
}

TEST_CASE("settings sec: zapisywalny folder NTFS nie jest bledne uznawany za niezapisywalny (proba z DELETE)")
{
    TempDir td;
    for (int i = 0; i < 20; ++i) REQUIRE(isDirectoryWritable(td.path));
    CHECK(fs::is_empty(td.path));
}

TEST_CASE("settings sec: symlink w miejscu .bad nie przekierowuje kopii")
{
    TempDir td;
    const fs::path f = td.path / "settings.json";
    const fs::path victim = td.path / "victim.txt";
    {
        std::ofstream(victim, std::ios::binary) << "WAZNE";
    }
    fs::path bad = f;
    bad += ".bad";
    if (!::CreateSymbolicLinkW(bad.c_str(), victim.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        MESSAGE("brak uprawnien do tworzenia dowiazan symbolicznych - test pominiety");
        return;
    }
    {
        std::ofstream(f, std::ios::binary) << "{ zepsute";
    }
    REQUIRE_FALSE(loadSettings(f).ok());
    CHECK(readFile(victim) == "WAZNE");
    CHECK(readFile(bad) == "{ zepsute");
    CHECK((::GetFileAttributesW(bad.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
}

TEST_CASE("settings sec: zapis nie zostawia plikow, nazwa tymczasowa jest losowa")
{
    TempDir td;
    const fs::path f = td.path / "settings.json";
    // martwy plik tymczasowy o starym wzorcu (pid_licznik), wiek 2 dni -> sprzatniety
    const fs::path legacy = td.path / "settings.json.tmp77_1";
    {
        std::ofstream(legacy, std::ios::binary) << "x";
    }
    {
        HANDLE h = ::CreateFileW(legacy.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        FILETIME now{};
        ::GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER u{};
        u.LowPart = now.dwLowDateTime;
        u.HighPart = now.dwHighDateTime;
        u.QuadPart -= 2ull * 24 * 3600 * 10000000ull;
        FILETIME ft{u.LowPart, static_cast<DWORD>(u.HighPart)};
        REQUIRE(::SetFileTime(h, nullptr, nullptr, &ft));
        ::CloseHandle(h);
    }
    REQUIRE(saveSettings(f, Settings{}).ok());
    CHECK_FALSE(fs::exists(legacy));
    int n = 0;
    for (const auto& e : fs::directory_iterator(td.path)) {
        (void)e;
        ++n;
    }
    CHECK(n == 1);
}

TEST_CASE("settings lokalizacja: migracja %APPDATA%\\vgmplay -> %APPDATA%\\ChipJuke [E5]")
{
    TempDir td;
    const fs::path appdata = td.path / "AppData";
    SUBCASE("stary katalog istnieje, nowego brak -> przeniesiony z zawartoscia")
    {
        writeFile(appdata / "vgmplay" / "settings.json", "{}");
        CHECK(migrateLegacyAppDataDir(appdata));
        CHECK(fs::exists(appdata / "ChipJuke" / "settings.json"));
        CHECK_FALSE(fs::exists(appdata / "vgmplay"));
        CHECK_FALSE(migrateLegacyAppDataDir(appdata));   // drugi raz nic nie robi
    }
    SUBCASE("oba istnieja -> nic nie ruszane")
    {
        writeFile(appdata / "vgmplay" / "settings.json", "{\"old\":1}");
        writeFile(appdata / "ChipJuke" / "settings.json", "{}");
        CHECK_FALSE(migrateLegacyAppDataDir(appdata));
        CHECK(fs::exists(appdata / "vgmplay" / "settings.json"));
    }
    SUBCASE("brak starego, pusta sciezka, UNC -> false")
    {
        CHECK_FALSE(migrateLegacyAppDataDir(appdata));
        CHECK_FALSE(migrateLegacyAppDataDir(fs::path()));
        CHECK_FALSE(migrateLegacyAppDataDir(fs::path(L"\\\\serwer\\udzial")));
    }
}
