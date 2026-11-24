// Testy E4: czyste obliczenia szerokosci/przewijania kolumn kanalow i mapa fokusu (bez ImGui) oraz ekran
// glowny headless - poziome przewijanie wzorca z fokusem zawsze na widocznej kolumnie, zawijanie w Info,
// pasek podpowiedzi domyslnie wylaczony, wbudowana przegladarka plikow otwierana padem.
#include <doctest/doctest.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "vgmplay_ui/channel_mix.h"
#include "vgmplay_ui/channel_scopes.h"
#include "vgmplay_ui/focus_grid.h"
#include "vgmplay_ui/nav_input.h"
#include "vgmplay_ui/pattern_view.h"
#include "vgmplay_ui/player_screen.h"
#include "vgmplay_ui/scroll_layout.h"

using namespace vgmplay;
using namespace vgmplay::ui;
namespace fs = std::filesystem;

// ------------------------------------------------------------------ czyste funkcje

TEST_SUITE("ui/scroll_layout")
{
    TEST_CASE("szerokosc kolumny kanalu = 2x dawna (E3)")
    {
        const float oldW = std::floor(8.0f * (13.0f + 1.5f));   // wzor z E3 dla 13 znakow komorki
        CHECK(channelColumnWidth(8.0f, 13) == doctest::Approx(oldW * 2.0f));
        CHECK(kChannelColumnScale == 2.0f);
        CHECK(channelColumnWidth(7.0f, 9) == doctest::Approx(std::floor(7.0f * 10.5f) * 2.0f));
    }

    TEST_CASE("clampScroll i szerokosc zawartosci")
    {
        CHECK(contentWidth(10, 100.0f) == 1000.0f);
        CHECK(contentWidth(-3, 100.0f) == 0.0f);
        CHECK(clampScroll(-5.0f, 1000.0f, 300.0f) == 0.0f);
        CHECK(clampScroll(900.0f, 1000.0f, 300.0f) == 700.0f);
        CHECK(clampScroll(50.0f, 200.0f, 300.0f) == 0.0f);   // wszystko sie miesci: brak przesuniecia
    }

    TEST_CASE("kolumny widoczne czesciowo i w calosci")
    {
        // 10 kolumn po 100 px, okno 350 px
        ColumnRange r = columnsInView(0.0f, 10, 100.0f, 350.0f);
        CHECK(r.first == 0);
        CHECK(r.last == 3);   // 4. kolumna widoczna w polowie
        CHECK(r.count() == 4);
        ColumnRange f = fullyVisibleColumns(0.0f, 10, 100.0f, 350.0f);
        CHECK(f.first == 0);
        CHECK(f.last == 2);
        r = columnsInView(120.0f, 10, 100.0f, 350.0f);
        CHECK(r.first == 1);
        CHECK(r.last == 4);
        f = fullyVisibleColumns(120.0f, 10, 100.0f, 350.0f);
        CHECK(f.first == 2);
        CHECK(f.last == 3);
        r = columnsInView(650.0f, 10, 100.0f, 350.0f);
        CHECK(r.last == 9);
        CHECK(columnsInView(0.0f, 0, 100.0f, 350.0f).count() == 0);
        CHECK(fullyVisibleColumns(0.0f, 10, 400.0f, 350.0f).count() == 0);   // kolumna szersza niz okno
        CHECK(columnFullyVisible(2, 0.0f, 100.0f, 350.0f));
        CHECK_FALSE(columnFullyVisible(3, 0.0f, 100.0f, 350.0f));
    }

    TEST_CASE("revealColumn: minimalne przesuniecie, obciete do zakresu")
    {
        CHECK(revealColumn(0.0f, 2, 10, 100.0f, 350.0f) == 0.0f);          // juz widoczna
        CHECK(revealColumn(0.0f, 3, 10, 100.0f, 350.0f) == 50.0f);         // prawa krawedz kolumny = prawa krawedz okna
        CHECK(revealColumn(500.0f, 1, 10, 100.0f, 350.0f) == 100.0f);      // z prawej -> do lewej krawedzi
        CHECK(revealColumn(0.0f, 9, 10, 100.0f, 350.0f) == 650.0f);        // ostatnia: maksimum przewiniecia
        CHECK(revealColumn(0.0f, 99, 10, 100.0f, 350.0f) == 650.0f);       // poza zakresem: obciete
        for (int c = 0; c < 10; ++c) {                                      // po revealColumn kolumna jest w calosci widoczna
            const float o = revealColumn(333.0f, c, 10, 100.0f, 350.0f);
            CHECK(columnFullyVisible(c, o, 100.0f, 350.0f));
        }
    }

    TEST_CASE("pasek przewijania: uchwyt i przeciaganie sa odwrotne")
    {
        const Thumb t = scrollThumb(0.0f, 1000.0f, 250.0f, 250.0f, 10.0f);
        CHECK(t.pos == 0.0f);
        CHECK(t.len == doctest::Approx(62.5f));
        const Thumb e = scrollThumb(750.0f, 1000.0f, 250.0f, 250.0f, 10.0f);
        CHECK(e.pos + e.len == doctest::Approx(250.0f));
        const Thumb all = scrollThumb(0.0f, 100.0f, 250.0f, 250.0f, 10.0f);   // wszystko widoczne
        CHECK(all.len == 250.0f);
        CHECK(scrollThumb(0.0f, 100000.0f, 250.0f, 250.0f, 16.0f).len == 16.0f);   // minimalna dlugosc uchwytu
        for (float off : {0.0f, 100.0f, 333.0f, 750.0f}) {
            const Thumb th = scrollThumb(off, 1000.0f, 250.0f, 250.0f, 10.0f);
            CHECK(offsetFromThumb(th.pos, 1000.0f, 250.0f, 250.0f, th.len) == doctest::Approx(off).epsilon(0.01));
        }
        CHECK(offsetFromThumb(-50.0f, 1000.0f, 250.0f, 250.0f, 62.5f) == 0.0f);
        CHECK(offsetFromThumb(9999.0f, 1000.0f, 250.0f, 250.0f, 62.5f) == 750.0f);
    }

    TEST_CASE("kolko myszy: kierunek i obciecie")
    {
        CHECK(wheelScroll(0.0f, -1.0f, 50.0f, 1000.0f, 300.0f) == 50.0f);     // w dol/prawo
        CHECK(wheelScroll(0.0f, 1.0f, 50.0f, 1000.0f, 300.0f) == 0.0f);       // nie ponizej zera
        CHECK(wheelScroll(690.0f, -1.0f, 50.0f, 1000.0f, 300.0f) == 700.0f);  // nie ponad maksimum
    }
}

TEST_SUITE("ui/focus_grid")
{
    TEST_CASE("kursor staje tylko na zarejestrowanych elementach (dziury w numeracji sa pomijane)")
    {
        FocusGrid g(0, 0);
        g.begin();
        g.reg(0, 0);
        g.reg(0, 4);   // kolumny 1..3 sie nie zmiescily
        g.reg(0, 5);
        g.reg(1, 0);
        g.end();
        CHECK(g.has(0, 4));
        CHECK_FALSE(g.has(0, 2));
        CHECK(g.rowSize(0) == 3);
        g.moveCol(1);
        CHECK(g.col == 4);
        g.moveCol(1);
        CHECK(g.col == 5);
        g.moveCol(1);
        CHECK(g.col == 5);   // koniec: bez zawijania
        g.moveCol(-1);
        g.moveCol(-1);
        CHECK(g.col == 0);
        g.moveCol(-1);
        CHECK(g.col == 0);
    }

    TEST_CASE("po zmianie ukladu kursor wraca na najblizszy istniejacy element")
    {
        FocusGrid g(0, 3);
        g.begin();
        g.reg(0, 0);
        g.reg(0, 1);
        g.reg(0, 2);
        g.reg(0, 3);
        g.end();
        CHECK(g.at(0, 3));
        g.begin();   // okno sie zwezilo: zostaly tylko 0 i 1
        g.reg(0, 0);
        g.reg(0, 1);
        g.end();
        CHECK(g.at(0, 1));
        g.begin();   // zostal tylko element 0 i 5
        g.reg(0, 0);
        g.reg(0, 5);
        g.end();
        CHECK((g.col == 0 || g.col == 5));
        CHECK(g.has(g.row, g.col));
    }

    TEST_CASE("puste wiersze sa pomijane przy ruchu Gora/Dol i przy zwezeniu okna")
    {
        FocusGrid g(0, 0);
        g.begin();
        g.reg(2, 0);   // wiersze 0 i 1 puste (np. pasek narzedzi bez zmieszczonych przyciskow)
        g.reg(3, 0);
        g.end();
        CHECK(g.row == 2);
        g.moveRow(-1);
        CHECK(g.row == 2);
        g.moveRow(1);
        CHECK(g.row == 3);
        g.moveRow(1);
        CHECK(g.row == 3);
        g.moveRow(-5);
        CHECK(g.row == 2);

        FocusGrid h(1, 0);
        h.begin();
        h.reg(0, 0);
        h.reg(0, 1);
        h.end();   // wiersz 1 nie istnieje: kursor w wierszu 0
        CHECK(h.has(h.row, h.col));
    }

    TEST_CASE("kolumna zachowywana przy przejsciu miedzy wierszami (najblizsza)")
    {
        FocusGrid g(0, 4);
        g.begin();
        for (int c = 0; c < 6; ++c) g.reg(0, c);
        g.reg(1, 0);
        g.reg(1, 1);
        g.end();
        g.moveRow(1);
        CHECK(g.row == 1);
        CHECK(g.col == 1);
    }
}

// ------------------------------------------------------------------ ekran glowny (headless)

namespace {

struct Rec final : PlayerActions {
    std::vector<std::string> log;
    std::vector<std::filesystem::path> added, imported, exported;
    settings::Settings* target = nullptr;
    void requestOpenFile() override { log.push_back("open"); }
    void togglePlay() override {}
    void play() override {}
    void pause() override {}
    void stop() override {}
    void seekToFrame(std::uint64_t) override {}
    void seekBySeconds(double) override {}
    void setVolume(float) override {}
    void dismissError() override {}
    void toggleChannelMute(int ch) override { log.push_back("mute" + std::to_string(ch)); }
    void toggleChannelSolo(int) override {}
    void muteAllChannels() override {}
    void unmuteAllChannels() override {}
    void clearSolo() override {}
    void setChannelScopesVisible(bool) override {}
    void setPatternRowTicks(std::uint32_t) override {}
    void requestAddFolder() override { log.push_back("folder"); }
    void requestImportM3u() override { log.push_back("import"); }
    void requestExportM3u() override { log.push_back("export"); }
    void applySettings(const settings::Settings& s) override
    {
        if (target) *target = s;
        log.push_back("settings");
    }
    void addPaths(std::vector<std::filesystem::path> p) override { added = std::move(p); log.push_back("addPaths"); }
    void importM3uFile(const std::filesystem::path& p) override { imported.push_back(p); }
    void exportM3uFile(const std::filesystem::path& p) override { exported.push_back(p); }
    [[nodiscard]] bool has(const std::string& s) const { return std::find(log.begin(), log.end(), s) != log.end(); }
};

struct Headless {
    explicit Headless(float w = 1280.0f, float h = 720.0f)
    {
        ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(w, h);
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        unsigned char* px = nullptr;
        int fw = 0, fh = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    }
    ~Headless() { ImGui::DestroyContext(ctx); }
    ImGuiContext* ctx;
};

SDL_Event padEv(Uint8 button, bool down = true)
{
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = 3;
    e.gbutton.button = button;
    return e;
}

SDL_Event keyEv(SDL_Scancode sc, bool down = true)
{
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    return e;
}

// Uklad 3 ukladow x 8 kanalow = 24 kanaly (jak YM2151 + SegaPCM + ...), z wzorcem.
ChannelLayout bigLayout(int devices = 3, int per = 8)
{
    ChannelLayout l;
    for (int d = 0; d < devices; ++d) {
        DeviceDesc a;
        a.id = static_cast<std::uint16_t>(d);
        a.type = d == 0 ? ChipType::YM2151 : ChipType::SegaPcm;
        a.channelCount = static_cast<std::uint16_t>(per);
        a.mainChannelCount = static_cast<std::uint16_t>(per);
        l.devices.push_back(a);
        for (int c = 0; c < per; ++c) l.channels.push_back({static_cast<std::uint16_t>(d), static_cast<std::uint16_t>(c)});
    }
    return l;
}

struct Screen {
    Headless g;
    Rec act;
    PlayerScreen screen;
    NavInput nav;
    PlayerView view;
    settings::Settings settings;
    playlist::PlaylistModel model;
    TrackMetaStore meta;
    std::shared_ptr<TrackInfo> info;
    std::unique_ptr<ChannelMix> mix = std::make_unique<ChannelMix>();
    ChannelScopeBuffer cs;
    std::uint64_t now = 1000;

    explicit Screen(float w = 1280.0f, float h = 720.0f, int devices = 3, int per = 8) : g(w, h)
    {
        REQUIRE(model.addPlaylist("Lista").ok());
        (void)model.setActive(0);
        std::vector<playlist::Entry> e(2);
        e[0].path = "C:/m/a.vgm";
        e[1].path = "C:/m/b.vgm";
        REQUIRE(model.insert(0, 0, e).ok());
        info = std::make_shared<TrackInfo>();
        info->layout = bigLayout(devices, per);
        info->introFrames = 48000;
        info->loopFrames = 48000;
        mix->setLayout(info->layout);
        view.info = info;
        view.track = std::make_shared<loader::LoadedTrack>();
        view.mix = mix.get();
        view.channelScopes = &cs;
        view.playlists = &model;
        view.meta = &meta;
        view.settings = &settings;
        view.sampleRate = 48000;
        view.transport.state = engine::TransportState::Paused;
        view.transport.epoch = 1;
        view.pattern = makeFakePattern(info->layout, 44100ull * 2, 44100, true, 735);
        view.patternHasEffects = true;
        act.target = &settings;
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
    void tapPad(Uint8 b)
    {
        nav.handleEvent(padEv(b, true), now);
        run();
        nav.handleEvent(padEv(b, false), now + 10);
        run();
        now += 100;
    }
    void tapKey(SDL_Scancode sc)
    {
        nav.handleEvent(keyEv(sc, true), now);
        run();
        nav.handleEvent(keyEv(sc, false), now + 10);
        run();
        now += 100;
    }
    bool settleBrowser()
    {
        for (int i = 0; i < 2500; ++i) {
            run();
            if (screen.browserOpen() && !screen.browser().loading()) return true;
            if (!screen.browserOpen()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
};

struct TempDir {
    fs::path path;
    TempDir()
    {
        static std::atomic<int> n{0};
        path = fs::temp_directory_path() /
               (L"vgmplay_e4_scr_" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"_" +
                std::to_wstring(n++));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string u8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

TEST_SUITE("ui/e4_screen")
{
    TEST_CASE("wzorzec: 24 kanaly nie mieszcza sie - kolumny 2x szersze, przewijanie, fokus zawsze widoczny")
    {
        Screen f;
        f.nav.setFocus(Panel::Tracker);
        f.run();
        CHECK(f.screen.columnWidthPx() > 0.0f);
        CHECK(f.screen.visibleColumns() < 24);
        CHECK(f.screen.visibleColumns() >= 2);
        CHECK(f.screen.columnScroll() == 0.0f);
        CHECK(f.screen.channelFullyVisible());
        const float colW = f.screen.columnWidthPx();
        const float charW = ImGui::CalcTextSize("0").x;
        CHECK(colW == doctest::Approx(channelColumnWidth(charW, kCellCharsFx)));   // fake pattern ma efekty

        float prevScroll = 0.0f;
        int moved = 0;
        for (int i = 0; i < 30; ++i) {
            f.key(Action::Right);
            CHECK(f.screen.channelFullyVisible());                       // fokus nigdy poza widokiem
            CHECK(f.screen.channelCursor() >= f.screen.firstColumn());
            CHECK(f.screen.channelCursor() < f.screen.firstColumn() + f.screen.visibleColumns());
            CHECK(f.screen.columnScroll() >= prevScroll);
            prevScroll = f.screen.columnScroll();
            moved = f.screen.channelCursor();
        }
        CHECK(moved == 23);
        CHECK(f.screen.firstColumn() + f.screen.visibleColumns() == 24);   // widac ostatni kanal
        for (int i = 0; i < 30; ++i) {
            f.key(Action::Left);
            CHECK(f.screen.channelFullyVisible());
        }
        CHECK(f.screen.channelCursor() == 0);
        CHECK(f.screen.columnScroll() == 0.0f);
    }

    TEST_CASE("wzorzec: Shift+kolko przewija widok, a kursor kanalu zostaje na widocznej kolumnie")
    {
        Screen f;
        f.nav.setFocus(Panel::Tracker);
        f.run();
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(900.0f, 300.0f);   // nad panelem wzorca
        f.run();
        f.run();
        io.AddKeyEvent(ImGuiMod_Shift, true);
        for (int i = 0; i < 6; ++i) {
            io.AddMouseWheelEvent(0.0f, -1.0f);
            f.run();
        }
        CHECK(f.screen.columnScroll() > 0.0f);
        CHECK(f.screen.channelFullyVisible());
        CHECK(f.screen.channelCursor() >= 1);   // kursor poszedl za widokiem
        const float scrolled = f.screen.columnScroll();
        for (int i = 0; i < 40; ++i) {
            io.AddMouseWheelEvent(0.0f, -1.0f);
            f.run();
        }
        CHECK(f.screen.columnScroll() >= scrolled);
        CHECK(f.screen.firstColumn() + f.screen.visibleColumns() == 24);
        CHECK(f.screen.channelFullyVisible());
        io.AddKeyEvent(ImGuiMod_Shift, false);
        f.run();
    }

    TEST_CASE("wzorzec: zwezenie okna przewija widok do kanalu z fokusem")
    {
        Screen f(1600.0f, 720.0f, 1, 4);   // 4 kanaly miesci sie szeroko
        f.nav.setFocus(Panel::Tracker);
        f.run();
        for (int i = 0; i < 3; ++i) f.key(Action::Right);
        CHECK(f.screen.channelCursor() == 3);
        CHECK(f.screen.columnScroll() == 0.0f);
        ImGui::GetIO().DisplaySize = ImVec2(800.0f, 480.0f);
        f.run();
        f.run();
        CHECK(f.screen.channelFullyVisible());
        CHECK(f.screen.channelCursor() >= f.screen.firstColumn());
        CHECK(f.screen.channelCursor() < f.screen.firstColumn() + f.screen.visibleColumns());
    }

    TEST_CASE("pasek narzedzi wzorca: w waskim oknie fokus nie staje na ukrytym przycisku")
    {
        for (float w : {800.0f, 900.0f, 1000.0f, 1280.0f}) {
            Screen f(w, 480.0f, 1, 4);
            f.nav.setFocus(Panel::Tracker);
            f.run();
            f.key(Action::Up);   // na pasek narzedzi (wiersz 0)
            for (int i = 0; i < 12; ++i) {
                f.key(Action::Right);
                CHECK(f.screen.focusRegistered(Panel::Tracker));
            }
            for (int i = 0; i < 12; ++i) {
                f.key(Action::Left);
                CHECK(f.screen.focusRegistered(Panel::Tracker));
            }
        }
    }

    TEST_CASE("panele Playlista i Info sa wezsze (~70%) - wiecej miejsca dla wzorca")
    {
        Screen f;
        f.nav.setFocus(Panel::Tracker);
        f.run();
        // Szerokosc panelu wzorca = reszta okna: powyzej 70% szerokosci przy 1280 px.
        CHECK(f.screen.trackerPanelWidth() > 1280.0f * 0.70f);
        CHECK(f.screen.sidePanelWidth() <= 1280.0f * 0.32f * 0.75f);
        CHECK(f.screen.sidePanelWidth() >= 100.0f);
    }

    TEST_CASE("Info: dlugie teksty sa zawijane, przewijanie w pikselach klawiszami i obciete do zawartosci")
    {
        Screen f;
        auto t = std::make_shared<loader::LoadedTrack>();
        t->gd3.present = true;
        std::string longNote;
        for (int i = 0; i < 60; ++i) longNote += "uwaga numer " + std::to_string(i) + " o bardzo dlugim opisie ";
        t->gd3.notes = longNote;
        t->gd3.trackEn = "Bardzo dluga nazwa utworu ktora nie zmiesci sie w waskim panelu informacji o utworze";
        f.view.track = t;
        f.nav.setFocus(Panel::Info);
        f.run();
        const float contentLong = f.screen.infoContentHeight();
        const float view = f.screen.infoViewHeight();
        CHECK(contentLong > view);                         // zawijanie zwiekszylo wysokosc ponad panel
        CHECK(f.screen.infoScroll() == 0.0f);
        f.key(Action::Down);
        CHECK(f.screen.infoScroll() > 0.0f);
        const float one = f.screen.infoScroll();
        f.key(Action::PageDown);
        CHECK(f.screen.infoScroll() > one);
        for (int i = 0; i < 200; ++i) f.key(Action::PageDown);
        CHECK(f.screen.infoScroll() == doctest::Approx(contentLong - view).epsilon(0.02));   // obciete do konca
        for (int i = 0; i < 400; ++i) f.key(Action::PageUp);
        CHECK(f.screen.infoScroll() == 0.0f);

        t->gd3.notes = "krotka";                           // krotsza notatka: mniejsza wysokosc
        f.run();
        CHECK(f.screen.infoContentHeight() < contentLong);
    }

    TEST_CASE("pasek podpowiedzi domyslnie wylaczony; widoczny tylko przy wlaczonym ustawieniu (takze przy padzie)")
    {
        CHECK_FALSE(settings::UiSettings{}.showButtonHints);
        Screen f;
        f.tapPad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);   // ostatnie wejscie = pad
        REQUIRE(f.nav.lastInputWasPad());
        f.run();
        const int off = ImGui::GetDrawData()->TotalVtxCount;
        f.settings.ui.showButtonHints = true;
        f.run();
        const int on = ImGui::GetDrawData()->TotalVtxCount;
        CHECK(on > off);
        f.settings.ui.showButtonHints = false;
        f.run();
        CHECK(ImGui::GetDrawData()->TotalVtxCount == off);
    }

    TEST_CASE("pad otwiera wbudowana przegladarke, mysz/klawiatura okno systemowe")
    {
        Screen f;
        f.nav.setFocus(Panel::Playlist);
        f.run();
        f.key(Action::Up);               // wiersz przyciskow
        f.key(Action::Right);
        f.key(Action::Right);            // "+ Pliki"
        f.key(Action::Confirm);          // klawiatura/synteza: okno systemowe
        CHECK(f.act.has("open"));
        CHECK_FALSE(f.screen.browserOpen());

        f.tapPad(SDL_GAMEPAD_BUTTON_SOUTH);   // A na padzie: wbudowana
        CHECK(f.screen.browserOpen());
        CHECK(f.nav.overlayActive());
        CHECK(f.screen.browser().mode() == BrowserMode::AddFiles);
        CHECK(f.screen.modalOpen());
        f.tapPad(SDL_GAMEPAD_BUTTON_EAST);    // B na liscie dyskow zamyka
        CHECK_FALSE(f.screen.browserOpen());
        CHECK_FALSE(f.nav.overlayActive());
    }

    TEST_CASE("przegladarka: pad wybiera plik, dodaje folder, LB/RB to strony, zapamietuje folder")
    {
        TempDir t;
        for (int i = 0; i < 40; ++i) {
            std::ofstream(t.path / (L"n" + std::to_wstring(100 + i) + L".vgm"), std::ios::binary) << "x";
        }
        fs::create_directories(t.path / L"podfolder");
        Screen f;
        f.settings.library.recentFolders = {u8(t.path)};
        f.nav.setFocus(Panel::Playlist);
        f.run();
        f.key(Action::Up);
        f.key(Action::Right);
        f.key(Action::Right);
        f.tapPad(SDL_GAMEPAD_BUTTON_SOUTH);
        REQUIRE(f.screen.browserOpen());
        REQUIRE(f.settleBrowser());
        CHECK(f.screen.browser().dir() == t.path);
        CHECK(f.screen.browser().entries().size() == 42);   // "..", podfolder, 40 plikow
        const int sel0 = f.screen.browser().selection();
        f.tapPad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        CHECK(f.screen.browser().selection() == sel0 + 1);
        f.tapPad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);        // RB = strona w dol
        CHECK(f.screen.browser().selection() > sel0 + 3);
        const int afterPage = f.screen.browser().selection();
        f.tapPad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);         // LB = strona w gore
        CHECK(f.screen.browser().selection() < afterPage);
        CHECK(f.nav.focus() == Panel::Playlist);            // LB/RB nie zmieniaja panelu pod przegladarka

        f.tapPad(SDL_GAMEPAD_BUTTON_WEST);                  // X: dodaj biezacy folder
        CHECK_FALSE(f.screen.browserOpen());
        REQUIRE(f.act.added.size() == 1);
        CHECK(f.act.added[0] == t.path);
        REQUIRE_FALSE(f.settings.library.recentFolders.empty());
        CHECK(f.settings.library.recentFolders.front() == u8(t.path));

        f.tapPad(SDL_GAMEPAD_BUTTON_SOUTH);
        REQUIRE(f.screen.browserOpen());
        REQUIRE(f.settleBrowser());
        f.tapPad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);             // z "podfolder" na pierwszy plik
        f.tapPad(SDL_GAMEPAD_BUTTON_NORTH);                 // Y: zaznacz (i w dol)
        f.tapPad(SDL_GAMEPAD_BUTTON_NORTH);
        CHECK(f.screen.browser().markedCount() == 2);
        f.tapPad(SDL_GAMEPAD_BUTTON_SOUTH);                 // A na pliku: zaznaczone + biezacy
        CHECK_FALSE(f.screen.browserOpen());
        CHECK(f.act.added.size() == 3);
    }

    TEST_CASE("przegladarka: eksport M3U pada zapisuje w biezacym folderze pod nazwa playlisty")
    {
        TempDir t;
        Screen f;
        f.settings.library.recentFolders = {u8(t.path)};
        f.nav.setFocus(Panel::Playlist);
        f.run();
        f.key(Action::Up);
        for (int i = 0; i < 5; ++i) f.key(Action::Right);   // "Eksport"
        f.tapPad(SDL_GAMEPAD_BUTTON_SOUTH);
        REQUIRE(f.screen.browserOpen());
        CHECK(f.screen.browser().mode() == BrowserMode::ExportM3u);
        REQUIRE(f.settleBrowser());
        f.tapPad(SDL_GAMEPAD_BUTTON_WEST);                  // X: zapisz tutaj
        CHECK_FALSE(f.screen.browserOpen());
        REQUIRE(f.act.exported.size() == 1);
        CHECK(f.act.exported[0] == t.path / L"Lista.m3u8");
    }

    TEST_CASE("klawiatura w przegladarce: Esc/Enter/Spacja; zmiana panelu Tab nie dziala pod nakladka")
    {
        TempDir t;
        fs::create_directories(t.path / L"sub");
        std::ofstream(t.path / L"sub" / L"a.vgm", std::ios::binary) << "x";
        Screen f;
        f.screen.browser();   // dostep do obiektu
        f.nav.setFocus(Panel::Playlist);
        f.settings.library.recentFolders = {u8(t.path)};
        f.run();
        f.screen.openBrowser(BrowserMode::AddFiles, f.view);
        REQUIRE(f.settleBrowser());
        f.tapKey(SDL_SCANCODE_TAB);
        CHECK(f.nav.focus() == Panel::Playlist);
        f.tapKey(SDL_SCANCODE_DOWN);                        // "sub"
        f.tapKey(SDL_SCANCODE_RETURN);                      // wejscie
        REQUIRE(f.settleBrowser());
        CHECK(f.screen.browser().dir() == t.path / L"sub");
        f.tapKey(SDL_SCANCODE_DOWN);                        // a.vgm
        f.tapKey(SDL_SCANCODE_SPACE);                       // zaznacz
        CHECK(f.screen.browser().markedCount() == 1);
        f.tapKey(SDL_SCANCODE_ESCAPE);                      // w gore
        REQUIRE(f.settleBrowser());
        CHECK(f.screen.browser().dir() == t.path);
        f.tapKey(SDL_SCANCODE_F4);                          // zamknij od razu
        CHECK_FALSE(f.screen.browserOpen());
    }
}
