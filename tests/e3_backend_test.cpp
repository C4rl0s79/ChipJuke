// Testy dodatkow E3: ustawienia (powtarzanie, pasek, podpowiedzi, sciezki sieciowe, urzadzenie,
// wiazania) oraz PlaylistModel::updateEntryMeta.
#include <doctest/doctest.h>

#include <ostream>   // wymagane przez doctest dla stringifikacji std::string_view

#include <chrono>
#include <climits>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "vgmplay/input.hpp"
#include "vgmplay/playlist.hpp"
#include "vgmplay/settings.hpp"

namespace {

using namespace vgmplay;

bool hasWarn(const std::vector<Warning>& w, const std::string& needle)
{
    for (const auto& x : w)
        if (x.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_SUITE("e3 settings")
{
    TEST_CASE("round-trip nowych pol")
    {
        settings::Settings s;
        s.playback.repeat = settings::Repeat::All;
        s.ui.scopeBar = settings::ScopeBar::Hidden;
        s.ui.showButtonHints = false;
        s.library.allowNetworkPaths = true;
        s.audio.device = "{0.0.0.00000000}.{abc-123}";
        const std::string j = settings::toJson(s);
        auto r = settings::fromJson(j);
        REQUIRE(r.ok());
        CHECK(r.value().playback.repeat == settings::Repeat::All);
        CHECK(r.value().ui.scopeBar == settings::ScopeBar::Hidden);
        CHECK_FALSE(r.value().ui.showButtonHints);
        CHECK(r.value().library.allowNetworkPaths);
        CHECK(r.value().audio.device == s.audio.device);
        CHECK(r.value().schemaVersion == 1);
        CHECK(settings::toJson(r.value()) == j);
        for (auto rep : {settings::Repeat::Off, settings::Repeat::One, settings::Repeat::All})
            for (auto sb : {settings::ScopeBar::Small, settings::ScopeBar::Large, settings::ScopeBar::Hidden}) {
                settings::Settings t;
                t.playback.repeat = rep;
                t.ui.scopeBar = sb;
                auto rt = settings::fromJson(settings::toJson(t));
                REQUIRE(rt.ok());
                CHECK(rt.value().playback.repeat == rep);
                CHECK(rt.value().ui.scopeBar == sb);
            }
    }

    TEST_CASE("brak nowych pol = domyslne (bez ostrzezen); zle wartosci = domyslne + Warning")
    {
        std::vector<Warning> w;
        auto r = settings::fromJson(R"({"schemaVersion":1})", &w);
        REQUIRE(r.ok());
        CHECK(r.value().playback.repeat == settings::Repeat::Off);
        CHECK(r.value().ui.scopeBar == settings::ScopeBar::Small);
        CHECK(r.value().ui.showButtonHints);
        CHECK_FALSE(r.value().library.allowNetworkPaths);
        CHECK(w.empty());

        w.clear();
        auto b = settings::fromJson(
            R"({"playback":{"repeat":"forever"},"ui":{"scopeBar":3,"showButtonHints":"yes"},"library":{"allowNetworkPaths":1}})", &w);
        REQUIRE(b.ok());
        CHECK(b.value().playback.repeat == settings::Repeat::Off);
        CHECK(b.value().ui.scopeBar == settings::ScopeBar::Small);
        CHECK(b.value().ui.showButtonHints);
        CHECK_FALSE(b.value().library.allowNetworkPaths);
        CHECK(hasWarn(w, "repeat"));
        CHECK(hasWarn(w, "scopeBar"));
        CHECK(hasWarn(w, "showButtonHints"));
        CHECK(hasWarn(w, "allowNetworkPaths"));

        w.clear();
        auto notObj = settings::fromJson(R"({"library":5})", &w);
        REQUIRE(notObj.ok());
        CHECK(hasWarn(w, "library"));
    }

    TEST_CASE("library.allowNetworkPaths z tego samego pliku NIE odblokowuje UNC w session.trackPath [E3-sec]")
    {
        const std::string unc = R"(\\\\srv\\share\\a.vgm)";
        const std::string doc1 = R"({"session":{"trackPath":")" + unc + R"("},"library":{"allowNetworkPaths":true}})";
        const std::string doc2 = R"({"library":{"allowNetworkPaths":true},"session":{"trackPath":")" + unc + R"("}})";
        const std::string doc3 = R"({"session":{"trackPath":")" + unc + R"("},"library":{"allowNetworkPaths":false}})";
        std::vector<Warning> w;
        auto r1 = settings::fromJson(doc1, &w, false);
        REQUIRE(r1.ok());
        CHECK(r1.value().session.trackPath.empty());               // flaga z pliku nie wystarcza
        CHECK(r1.value().library.allowNetworkPaths);                // ale sama flaga jest wczytana
        CHECK(hasWarn(w, "session.trackPath rejected"));
        CHECK(settings::fromJson(doc2, nullptr, false).value().session.trackPath.empty());
        CHECK(settings::fromJson(doc3, nullptr, false).value().session.trackPath.empty());
        // Parametr (flaga --allow-network-paths lub wartosc efektywna z aplikacji) odblokowuje, niezaleznie od pliku.
        CHECK_FALSE(settings::fromJson(doc1, nullptr, true).value().session.trackPath.empty());
        CHECK_FALSE(settings::fromJson(doc3, nullptr, true).value().session.trackPath.empty());
        // URL odrzucany zawsze.
        const std::string url = R"({"session":{"trackPath":"http://x/a.vgm"},"library":{"allowNetworkPaths":true}})";
        CHECK(settings::fromJson(url, nullptr, true).value().session.trackPath.empty());
    }

    TEST_CASE("audio.device: oczyszczanie i limit 512 B")
    {
        std::vector<Warning> w;
        auto r = settings::fromJson("{\"audio\":{\"device\":\"ab\\u0001c\\u202ed\\n\"}}", &w);
        REQUIRE(r.ok());
        CHECK(r.value().audio.device == "abcd");
        CHECK(hasWarn(w, "audio.device"));

        const std::string longId(1000, 'x');
        auto l = settings::fromJson(R"({"audio":{"device":")" + longId + R"("}})");
        REQUIRE(l.ok());
        CHECK(l.value().audio.device.size() == 512);

        auto bad = settings::fromJson(R"({"audio":{"device":42}})");
        REQUIRE(bad.ok());
        CHECK(bad.value().audio.device.empty());
    }

    TEST_CASE("wiazania: round-trip dla wszystkich Action i SourceKind")
    {
        settings::Settings s;
        const auto n = static_cast<int>(input::Action::Count_);
        for (int a = 1; a < n; ++a)
            for (int k = 0; k < 4; ++k)
                if (input::actionRebindable(static_cast<input::Action>(a)))
                s.input.bindings.push_back(
                    input::Binding{static_cast<input::Action>(a), {static_cast<input::SourceKind>(k), a * 10 + k - 5}});
        auto r = settings::fromJson(settings::toJson(s));
        REQUIRE(r.ok());
        REQUIRE(r.value().input.bindings.size() == s.input.bindings.size());
        for (std::size_t i = 0; i < s.input.bindings.size(); ++i) CHECK(r.value().input.bindings[i] == s.input.bindings[i]);
    }

    TEST_CASE("wiazania: akcje nieprzypisywalne (Mute/SoloChannel) odrzucane z Warning [E3-sec]")
    {
        std::vector<Warning> w;
        auto r = settings::fromJson(
            R"({"input":{"bindings":[{"action":"muteChannel","source":"key","code":1},)"
            R"({"action":"soloChannel","source":"key","code":2},{"action":"up","source":"key","code":3}]}})", &w);
        REQUIRE(r.ok());
        REQUIRE(r.value().input.bindings.size() == 1);
        CHECK(r.value().input.bindings[0].action == input::Action::Up);
        CHECK(hasWarn(w, "invalid binding skipped"));
        CHECK_FALSE(input::actionRebindable(input::Action::MuteChannel));
    }

    TEST_CASE("wiazania: kod > INT32_MAX (takze uint64 max) odrzucany bez zawijania [E3-sec]")
    {
        std::vector<Warning> w;
        auto r = settings::fromJson(
            R"({"input":{"bindings":[{"action":"up","source":"key","code":18446744073709551615},)"
            R"({"action":"up","source":"key","code":2147483648},{"action":"up","source":"key","code":2147483647},)"
            R"({"action":"up","source":"key","code":-2147483648},{"action":"up","source":"key","code":-2147483649}]}})", &w);
        REQUIRE(r.ok());
        REQUIRE(r.value().input.bindings.size() == 2);
        CHECK(r.value().input.bindings[0].source.code == 2147483647);
        CHECK(r.value().input.bindings[1].source.code == INT32_MIN);
        CHECK(hasWarn(w, "invalid binding skipped"));
    }

    TEST_CASE("wiazania: nieznane akcje/zrodla pominiete z Warning, limit liczby")
    {
        std::vector<Warning> w;
        auto r = settings::fromJson(
            R"({"input":{"bindings":[{"action":"quit","source":"key","code":41},{"action":"fly","source":"key","code":1},)"
            R"({"action":"up","source":"mouse","code":1},{"action":"none","source":"key","code":2},{"action":"up","source":"key"},7]}})",
            &w);
        REQUIRE(r.ok());
        REQUIRE(r.value().input.bindings.size() == 1);
        CHECK(r.value().input.bindings[0].action == input::Action::Quit);
        CHECK(hasWarn(w, "invalid binding skipped"));

        std::string many = R"({"input":{"bindings":[)";
        for (int i = 0; i < 1500; ++i)
            many += std::string(i ? "," : "") + R"({"action":"up","source":"key","code":)" + std::to_string(i) + "}";
        many += "]}}";
        w.clear();
        auto m = settings::fromJson(many, &w);
        REQUIRE(m.ok());
        CHECK(m.value().input.bindings.size() == 1024);
        CHECK(hasWarn(w, "too many bindings"));
    }
}

TEST_SUITE("e3 playlist")
{
    using namespace vgmplay::playlist;

    TEST_CASE("updateEntryMeta: wszystkie playlisty, porownanie sciezek bez wielkosci liter i separatorow")
    {
        PlaylistModel m;
        REQUIRE(m.addPlaylist("A").ok());
        REQUIRE(m.addPlaylist("B").ok());
        const Entry e1{L"D:/Muzyka/Song.vgm", "", 0};
        const Entry e2{L"d:\\muzyka\\song.VGM", "", 0};
        const Entry other{L"D:/Muzyka/Inna.vgm", "stary", 5};
        for (std::size_t l = 0; l < m.playlistCount(); ++l) {
            const Entry es[] = {e1, other, e2};
            REQUIRE(m.insert(l, 0, es).ok());
        }
        REQUIRE(m.setCursor(0, 2).ok());

        CHECK(m.updateEntryMeta(L"D:\\MUZYKA\\song.vgm", "Tytul\x01 ok", 90000) == 4);
        for (std::size_t l = 0; l < m.playlistCount(); ++l) {
            const auto& es = m.playlist(l).entries;
            CHECK(es[0].title == "Tytul ok");
            CHECK(es[0].durationMs == 90000);
            CHECK(es[2].title == "Tytul ok");
            CHECK(es[1].title == "stary");        // inny wpis nietkniety
            CHECK(es[1].durationMs == 5);
        }
        CHECK(m.playlist(0).cursor == 2);          // kursor bez zmian
        CHECK(m.playlist(0).entries.size() == 3);

        // Bez zmian -> 0; brak wpisu -> 0; pusta sciezka -> 0.
        CHECK(m.updateEntryMeta(L"D:/Muzyka/Song.vgm", "Tytul ok", 90000) == 0);
        CHECK(m.updateEntryMeta(L"D:/Muzyka/Brak.vgm", "x", 1) == 0);
        CHECK(m.updateEntryMeta(std::filesystem::path(), "x", 1) == 0);
    }

    TEST_CASE("updateEntryMeta: czyszczenie tytulu i przyciecie czasu do 24 h")
    {
        PlaylistModel m;
        REQUIRE(m.addPlaylist("A").ok());
        const Entry es[] = {Entry{L"C:/a.vgm", "", 0}};
        REQUIRE(m.insert(0, 0, es).ok());
        const std::string longTitle(2000, 'z');
        CHECK(m.updateEntryMeta(L"C:/a.vgm", longTitle, ~0ull) == 1);
        CHECK(m.playlist(0).entries[0].title.size() == 512);
        CHECK(m.playlist(0).entries[0].durationMs == 24ull * 3600 * 1000);
        CHECK(m.updateEntryMeta(L"C:/a.vgm", "a\xE2\x80\xAE" "b\tc", 0) == 1);   // U+202E usuniete, TAB -> spacja
        CHECK(m.playlist(0).entries[0].title == "ab c");
        CHECK(m.playlist(0).entries[0].durationMs == 0);
    }

    TEST_CASE("updateEntryMeta: pusty tytul nie kasuje istniejacego, czas sie aktualizuje [E3-sec]")
    {
        PlaylistModel m;
        REQUIRE(m.addPlaylist("A").ok());
        const Entry es[] = {Entry{L"C:/a.vgm", "Stary", 10}};
        REQUIRE(m.insert(0, 0, es).ok());
        CHECK(m.updateEntryMeta(L"C:/a.vgm", "", 5000) == 1);
        CHECK(m.playlist(0).entries[0].title == "Stary");
        CHECK(m.playlist(0).entries[0].durationMs == 5000);
        CHECK(m.updateEntryMeta(L"C:/a.vgm", "\x01\x02", 5000) == 0);   // tytul po czyszczeniu pusty, czas bez zmian
        CHECK(m.playlist(0).entries[0].title == "Stary");
    }

    TEST_CASE("updateEntryMeta: wariant wsadowy - semantyka jak pojedyncza, kolejne aktualizacje po kolei [E3-sec]")
    {
        PlaylistModel m;
        REQUIRE(m.addPlaylist("A").ok());
        const Entry es[] = {Entry{L"C:/Muz/a.vgm", "", 0}, Entry{L"C:/Muz/b.vgm", "B", 1}, Entry{L"c:\\muz\\A.VGM", "", 0}};
        REQUIRE(m.insert(0, 0, es).ok());
        const EntryMetaUpdate ups[] = {
            {L"C:/MUZ/a.vgm", "Pierwszy", 100}, {L"C:/Muz/a.vgm", "", 200},   // drugi: pusty tytul, nowy czas
            {L"C:/Muz/nie-ma.vgm", "x", 1}, {std::filesystem::path(), "y", 2}};
        CHECK(m.updateEntryMeta(ups) == 2);
        CHECK(m.playlist(0).entries[0].title == "Pierwszy");
        CHECK(m.playlist(0).entries[0].durationMs == 200);
        CHECK(m.playlist(0).entries[2].title == "Pierwszy");
        CHECK(m.playlist(0).entries[1].title == "B");
        CHECK(m.updateEntryMeta(std::span<const EntryMetaUpdate>{}) == 0);
    }

    TEST_CASE("updateEntryMeta: 100k wpisow x 1000 aktualizacji w jednym przejsciu < 1 s [E3-sec]")
    {
        PlaylistModel m;
        REQUIRE(m.addPlaylist("A").ok());
        std::vector<Entry> es;
        es.reserve(100000);
        for (int i = 0; i < 100000; ++i)
            es.push_back(Entry{std::filesystem::path(L"D:/Muzyka/Album/utwor_" + std::to_wstring(i) + L".vgm"), "", 0});
        REQUIRE(m.insert(0, 0, es).ok());
        std::vector<EntryMetaUpdate> ups;
        for (int i = 0; i < 1000; ++i)
            ups.push_back({std::filesystem::path(L"d:\\muzyka\\album\\UTWOR_" + std::to_wstring(i * 97) + L".vgm"),
                           "T" + std::to_string(i), 1000u + static_cast<unsigned>(i)});
        const auto t0 = std::chrono::steady_clock::now();
        const std::size_t changed = m.updateEntryMeta(ups);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(changed == 1000);
        CHECK(ms < 1000);
        MESSAGE("batch 100k x 1000: " << ms << " ms");
        CHECK(m.playlist(0).entries[97].title == "T1");
    }
}
