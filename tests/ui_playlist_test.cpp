// Testy panelu PLAYLISTA i okien modalnych (ImGui bez backendu): klawiatura/pad przez NavInput::events,
// zamierzenia trafiaja do PlayerActions. Logika biznesowa jest w AppController (ui_session_test.cpp).
#include <doctest/doctest.h>

#include <imgui.h>

#include <string>
#include <vector>

#include "vgmplay_ui/nav_input.h"
#include "vgmplay_ui/player_screen.h"
#include "vgmplay_ui/player_view.h"
#include "vgmplay_ui/track_meta.h"

using namespace vgmplay;
using namespace vgmplay::ui;

namespace {

struct Rec final : PlayerActions {
    std::vector<std::string> log;
    settings::Settings lastSettings;
    void requestOpenFile() override { log.push_back("open"); }
    void togglePlay() override {}
    void play() override {}
    void pause() override {}
    void stop() override {}
    void seekToFrame(std::uint64_t) override {}
    void seekBySeconds(double) override {}
    void setVolume(float) override {}
    void dismissError() override {}
    void toggleChannelMute(int) override {}
    void toggleChannelSolo(int) override {}
    void muteAllChannels() override {}
    void unmuteAllChannels() override {}
    void clearSolo() override {}
    void setChannelScopesVisible(bool) override {}
    void setPatternRowTicks(std::uint32_t) override {}
    void playEntry(std::size_t i) override { log.push_back("play" + std::to_string(i)); }
    void playNext() override { log.push_back("next"); }
    void playPrevious() override { log.push_back("prev"); }
    void removeEntry(std::size_t i) override { log.push_back("remove" + std::to_string(i)); }
    void moveEntry(std::size_t a, std::size_t b) override { log.push_back("move" + std::to_string(a) + ">" + std::to_string(b)); }
    void shufflePlaylist() override { log.push_back("shuffle"); }
    void setRepeat(playlist::RepeatMode m) override { log.push_back("repeat" + std::to_string(static_cast<int>(m))); }
    void selectPlaylist(std::size_t i) override { log.push_back("select" + std::to_string(i)); }
    void createPlaylist(const std::string& n) override { log.push_back("create:" + n); }
    void renamePlaylist(std::size_t i, const std::string& n) override { log.push_back("rename" + std::to_string(i) + ":" + n); }
    void deletePlaylist(std::size_t i) override { log.push_back("delete" + std::to_string(i)); }
    void applySettings(const settings::Settings& s) override { lastSettings = s; log.push_back("settings"); }
    void dismissNotice() override { log.push_back("dismiss"); }
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

struct Fixture {
    Headless g;
    playlist::PlaylistModel model;
    TrackMetaStore meta;
    settings::Settings settings;
    PlayerView view;
    Rec act;
    PlayerScreen screen;
    NavInput nav;

    Fixture()
    {
        REQUIRE(model.addPlaylist("Jeden").ok());
        REQUIRE(model.addPlaylist("Dwa").ok());
        (void)model.setActive(0);
        std::vector<playlist::Entry> e(3);
        e[0].path = "C:/m/a.vgm"; e[1].path = "C:/m/b.vgm"; e[2].path = "C:/m/c.vgm";
        e[1].title = "Drugi \xE2\x80\xAE" "evil";   // znak kierunkowy U+202E musi zostac usuniety przy wyswietlaniu
        REQUIRE(model.insert(0, 0, e).ok());
        view.playlists = &model;
        view.meta = &meta;
        view.settings = &settings;
        nav.setFocus(Panel::Playlist);
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
};

} // namespace

TEST_SUITE("ui/playlist_panel")
{
    TEST_CASE("tytul wpisu: GD3 wg jezyka, potem tytul z pliku, potem nazwa; sanityzacja znakow kierunkowych")
    {
        TrackMetaStore store;
        playlist::Entry e;
        e.path = "C:/m/plik.vgm";
        CHECK(entryTitle(e, &store, format::Gd3Language::En) == "plik.vgm");
        e.title = "Z M3U \xE2\x80\xAE" "x";
        CHECK(entryTitle(e, &store, format::Gd3Language::En) == "Z M3U x");
        TrackDisplay d;
        d.trackEn = "English";
        d.trackJp = "\xE6\x97\xA5\xE6\x9C\xAC";
        d.durationMs = 61000;
        store.put("c:/M/PLIK.vgm", d);   // klucz bez rozrozniania wielkosci liter
        CHECK(entryTitle(e, &store, format::Gd3Language::En) == "English");
        CHECK(entryTitle(e, &store, format::Gd3Language::Jp) == "\xE6\x97\xA5\xE6\x9C\xAC");
        CHECK(entryDurationMs(e, &store) == 61000);
    }

    TEST_CASE("klawiatura w panelu: wybor, Enter gra, Delete usuwa, Ctrl+Gora/Dol przenosi")
    {
        Fixture f;
        f.key(Action::Down);
        CHECK(f.screen.playlistSelection() == 1);
        f.key(Action::Accept);
        CHECK(f.act.log.back() == "play1");
        f.key(Action::Delete);
        CHECK(f.act.log.back() == "remove1");
        f.key(Action::MoveDown);
        CHECK(f.act.log.back() == "move1>2");
        CHECK(f.screen.playlistSelection() == 2);
        f.key(Action::MoveUp);
        CHECK(f.act.log.back() == "move2>1");
        f.key(Action::Up);
        f.key(Action::Up);
        f.key(Action::Up);
        CHECK(f.screen.playlistSelection() == 0);
    }

    TEST_CASE("pad: X pyta o potwierdzenie (A = tak, B = nie), Y otwiera menu wpisu")
    {
        Fixture f;
        f.act.log.clear();
        f.key(Action::ToggleSolo);   // X
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::DeleteEntry);
        CHECK(f.act.log.empty());
        f.key(Action::Back);         // B
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
        CHECK(f.act.log.empty());

        f.key(Action::ToggleSolo);
        f.key(Action::Accept);       // A
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
        REQUIRE(f.act.log.size() == 1);
        CHECK(f.act.log[0] == "remove0");

        f.act.log.clear();
        f.key(Action::ResetMix);     // Y
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::EntryMenu);
        f.key(Action::Down);         // "Usun"
        f.key(Action::Accept);
        CHECK(f.act.log.back() == "remove0");
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
    }

    TEST_CASE("okno modalne przejmuje nawigacje: panel pod spodem nie dostaje zdarzen")
    {
        Fixture f;
        f.key(Action::ResetMix);   // menu wpisu
        f.act.log.clear();
        f.key(Action::Delete);     // w menu nic nie robi
        f.key(Action::Down);
        CHECK(f.act.log.empty());
        CHECK(f.screen.playlistSelection() == 0);
        f.key(Action::Back);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::None);
    }

    TEST_CASE("Lewo/Prawo przelacza playliste, F6/F7/P/N sa globalne")
    {
        Fixture f;
        f.key(Action::Right);
        CHECK(f.act.log.back() == "select1");
        f.key(Action::Left);
        CHECK(f.act.log.back() == "select1");   // (0 + 2 - 1) % 2: zawija
        f.nav.setFocus(Panel::Transport);
        f.key(Action::ToggleRepeat);
        CHECK(f.act.log.back() == "repeat2");   // Off -> All
        f.view.repeat = playlist::RepeatMode::All;
        f.key(Action::ToggleRepeat);
        CHECK(f.act.log.back() == "repeat1");   // All -> One
        f.key(Action::Shuffle);
        CHECK(f.act.log.back() == "shuffle");
        f.key(Action::Next);
        CHECK(f.act.log.back() == "next");
        f.key(Action::Prev);
        CHECK(f.act.log.back() == "prev");
    }

    TEST_CASE("playlisty: F8 nowa (pad A potwierdza domyslna nazwe), F9 nazwa, Shift+Del usuwa po potwierdzeniu")
    {
        Fixture f;
        f.act.log.clear();
        f.key(Action::NewPlaylist);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::NewList);
        f.key(Action::Accept);
        REQUIRE(f.act.log.size() == 1);
        CHECK(f.act.log[0].rfind("create:", 0) == 0);
        CHECK(f.act.log[0].size() > 7);
        f.key(Action::RenamePlaylist);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::RenameList);
        f.key(Action::Accept);
        CHECK(f.act.log.back() == "rename0:Jeden");
        f.key(Action::DeletePlaylist);
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::DeleteList);
        f.key(Action::Back);
        CHECK(f.act.log.back() == "rename0:Jeden");   // anulowane: bez usuniecia
        f.key(Action::DeletePlaylist);
        f.key(Action::Accept);
        CHECK(f.act.log.back() == "delete0");
    }

    TEST_CASE("ustawienia (F10): nawigacja Gora/Dol i zmiana wartosci Lewo/Prawo przez applySettings")
    {
        Fixture f;
        f.nav.setFocus(Panel::Transport);
        f.key(Action::ToggleSettings);
        CHECK(f.screen.settingsOpen());
        f.act.log.clear();
        f.key(Action::Down);    // wiersz 1: liczba petli
        f.key(Action::Right);
        REQUIRE(f.act.log.size() == 1);
        CHECK(f.act.lastSettings.playback.loopCount == f.settings.playback.loopCount + 1);
        f.key(Action::Down);    // fade
        f.key(Action::Right);
        CHECK(f.act.lastSettings.playback.fadeMs == f.settings.playback.fadeMs + 250);
        f.key(Action::Down);
        f.key(Action::Down);
        f.key(Action::Down);    // wiersz 5: jezyk
        f.key(Action::Accept);
        CHECK(f.act.lastSettings.ui.language != f.settings.ui.language);
        f.key(Action::Down);    // GD3
        f.key(Action::Right);
        CHECK(f.act.lastSettings.ui.gd3Language == format::Gd3Language::Jp);
        f.key(Action::Down);    // rowTicks
        f.key(Action::Right);
        CHECK(f.act.lastSettings.ui.rowTicks > f.settings.ui.rowTicks);   // Prawo = dluzszy wiersz
        f.key(Action::Back);
        CHECK_FALSE(f.screen.settingsOpen());
    }

    TEST_CASE("pad: Y w panelu Transport otwiera ustawienia")
    {
        Fixture f;
        f.nav.setFocus(Panel::Transport);
        f.key(Action::ResetMix);
        CHECK(f.screen.settingsOpen());
    }

    TEST_CASE("komunikat z potwierdzeniem otwiera okno raportu; Enter/A potwierdza")
    {
        Fixture f;
        Notice n;
        n.text = "uszkodzony";
        n.details = {"playlists.json: ParseError"};
        n.needsAck = true;
        n.serial = 1;
        f.view.notice = n;
        f.run();
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::Report);
        f.act.log.clear();
        f.key(Action::Back);   // Esc zamyka okno, ale NIE potwierdza zapisu
        CHECK(f.act.log.empty());
        f.view.notice->serial = 2;
        f.run();
        CHECK(f.screen.dialog() == PlayerScreen::DlgKind::Report);
        f.key(Action::Accept);
        CHECK(f.act.log.back() == "dismiss");
    }

    TEST_CASE("bez wskaznika ustawien dzialaja stare przelaczniki lokalne (zgodnosc)")
    {
        Fixture f;
        f.view.settings = nullptr;
        f.nav.setFocus(Panel::Transport);
        f.key(Action::ToggleGd3Lang);
        CHECK(f.act.log.empty());   // zmiana lokalna, bez applySettings
    }
}
