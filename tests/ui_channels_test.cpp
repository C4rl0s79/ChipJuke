// Testy widoku kanalow (E1.5): MUTE/SOLO (maski), strumienie kanalow, VU, siatka ramek, nawigacja
// (klawisze/pad), routing akcji w PlayerScreen oraz AppController (SetChannelScopes, reset przy Load).
#include <doctest/doctest.h>

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app_controller.h"
#include "vgmplay_ui/channel_mix.h"
#include "vgmplay_ui/channel_scopes.h"
#include "vgmplay_ui/nav_input.h"
#include "vgmplay_ui/player_screen.h"
#include "vgmplay_ui/scope_buffer.h"

using namespace vgmplay;
using namespace vgmplay::ui;

namespace {

const std::filesystem::path kData = VGMPLAY_TEST_DATA_DIR;

// Uklad: urzadzenie 0 = 9 kanalow (6 glownych + 3 polaczone), urzadzenie 1 = 4 kanaly glowne.
ChannelLayout makeLayout()
{
    ChannelLayout l;
    DeviceDesc a;
    a.id = 0;
    a.channelCount = 9;
    a.mainChannelCount = 6;
    a.hasLinked = true;
    DeviceDesc b;
    b.id = 1;
    b.channelCount = 4;
    b.mainChannelCount = 4;
    l.devices = {a, b};
    for (std::uint16_t c = 0; c < 9; ++c) l.channels.push_back({0, c});
    for (std::uint16_t c = 0; c < 4; ++c) l.channels.push_back({1, c});
    return l;
}

ScopeBlock* makeBlock(std::vector<std::unique_ptr<ScopeBlock>>& keep, std::uint64_t epoch, std::uint64_t start,
                      std::uint32_t frames)
{
    keep.push_back(std::make_unique<ScopeBlock>());
    ScopeBlock* b = keep.back().get();
    b->epoch = epoch;
    b->startFrame = start;
    b->frames = frames;
    return b;
}

void addStream(ScopeBlock& b, std::uint16_t dev, std::uint16_t ch, bool available, float (*gen)(std::uint64_t))
{
    const std::size_t i = b.extraStreamCount++;
    b.extraDesc[i] = {dev, ch, static_cast<std::uint8_t>(available ? kScopeStreamAvailable : 0)};
    for (std::uint32_t f = 0; f < b.frames; ++f) b.extra[i][f] = gen ? gen(b.startFrame + f) : 0.0f;
}

float sine100(std::uint64_t f) { return 0.5f * std::sin(2.0f * 3.14159265f * static_cast<float>(f % 100) / 100.0f); }
float loud(std::uint64_t) { return 0.8f; }

} // namespace

TEST_SUITE("ui/channel_mix")
{
    TEST_CASE("maski MUTE: bit = indeks (glowny) lub indeks - mainChannelCount (polaczony)")
    {
        ChannelMix m;
        m.setLayout(makeLayout());
        REQUIRE(m.channelCount() == 13);
        CHECK(m.deviceCount() == 2);
        m.toggleMute(2);   // dev0, glowny, bit 2
        m.toggleMute(7);   // dev0, kanal 7 -> polaczony, bit 1
        m.toggleMute(9);   // dev1, kanal 0
        CHECK(m.muteMask(0) == MuteMask{0b100u, 0b10u});
        CHECK(m.muteMask(1) == MuteMask{0b1u, 0u});
        CHECK(m.muteMask(5) == MuteMask{});        // nieistniejace urzadzenie
        m.toggleMute(2);
        CHECK(m.muteMask(0) == MuteMask{0u, 0b10u});
        CHECK(m.muted(7));
        CHECK_FALSE(m.muted(2));
    }

    TEST_CASE("solo: slyszalne tylko kanaly solo, MUTE ma pierwszenstwo")
    {
        ChannelMix m;
        m.setLayout(makeLayout());
        CHECK(m.audible(0));
        m.toggleSolo(3);
        CHECK(m.anySolo());
        CHECK(m.audible(3));
        CHECK_FALSE(m.audible(0));
        CHECK_FALSE(m.audible(10));   // inne urzadzenie tez (solo globalne)
        m.toggleMute(3);
        CHECK_FALSE(m.audible(3));    // solo + mute = cisza
        m.toggleSolo(3);
        CHECK_FALSE(m.anySolo());
        CHECK(m.audible(0));
    }

    TEST_CASE("ogolne wylaczniki: mute all / unmute all / clear solo")
    {
        ChannelMix m;
        m.setLayout(makeLayout());
        m.muteAll();
        CHECK(m.anyMuted());
        CHECK(m.muteMask(0) == MuteMask{0b111111u, 0b111u});
        CHECK(m.muteMask(1) == MuteMask{0b1111u, 0u});
        m.toggleSolo(0);
        m.toggleSolo(12);
        m.unmuteAll();
        CHECK_FALSE(m.anyMuted());
        CHECK(m.muteMask(0) == MuteMask{});
        CHECK(m.anySolo());           // unmute all nie rusza solo
        m.clearSolo();
        CHECK_FALSE(m.anySolo());
        CHECK_FALSE(m.solo(0));
    }

    TEST_CASE("reset przy nowym utworze i kanaly poza zakresem")
    {
        ChannelMix m;
        m.setLayout(makeLayout());
        m.toggleMute(1);
        m.toggleSolo(2);
        m.setLayout(makeLayout());    // nowy utwor
        CHECK_FALSE(m.anyMuted());
        CHECK_FALSE(m.anySolo());
        CHECK_FALSE(m.toggleMute(-1));
        CHECK_FALSE(m.toggleSolo(13));
        CHECK_FALSE(m.muted(99));
        m.clear();
        CHECK(m.channelCount() == 0);
        CHECK_FALSE(m.toggleMute(0));
    }

    TEST_CASE("kanaly >= 32 w urzadzeniu nie psuja maski")
    {
        ChannelLayout l;
        DeviceDesc d;
        d.channelCount = 40;
        d.mainChannelCount = 40;
        l.devices = {d};
        for (std::uint16_t c = 0; c < 40; ++c) l.channels.push_back({0, c});
        ChannelMix m;
        m.setLayout(l);
        m.toggleMute(35);
        m.toggleMute(31);
        CHECK(m.muteMask(0) == MuteMask{1u << 31, 0u});
    }
}

TEST_SUITE("ui/channel_scopes")
{
    TEST_CASE("strumienie: opis (device, channel) niezaleznie od pozycji; flaga dostepnosci")
    {
        std::vector<std::unique_ptr<ScopeBlock>> keep;
        ChannelScopeBuffer cs;
        ScopeBlock* b = makeBlock(keep, 1, 0, 256);
        addStream(*b, 1, 2, true, sine100);
        addStream(*b, 0, 5, false, nullptr);
        cs.push(*b);
        CHECK(cs.live(1, 2));
        CHECK_FALSE(cs.live(0, 5));   // niedostepny
        CHECK_FALSE(cs.live(0, 0));   // nieznany
        CHECK(cs.liveCount() == 1);

        // Kolejny blok w innej kolejnosci strumieni - ten sam kanal, ciaglosc zachowana.
        ScopeBlock* b2 = makeBlock(keep, 1, 256, 256);
        addStream(*b2, 0, 5, true, loud);
        addStream(*b2, 1, 2, true, sine100);
        cs.push(*b2);
        CHECK(cs.live(1, 2));
        CHECK(cs.live(0, 5));

        // Strumien zniknal z bloku (np. SetChannelScopes{false}): martwy -> "n/a".
        ScopeBlock* b3 = makeBlock(keep, 1, 512, 256);
        cs.push(*b3);
        CHECK(cs.liveCount() == 0);
    }

    TEST_CASE("nowsza epoka zeruje, starsza jest odrzucana")
    {
        std::vector<std::unique_ptr<ScopeBlock>> keep;
        ChannelScopeBuffer cs;
        ScopeBlock* b = makeBlock(keep, 2, 0, 128);
        addStream(*b, 0, 0, true, loud);
        cs.push(*b);
        REQUIRE(cs.live(0, 0));
        ScopeBlock* old = makeBlock(keep, 1, 128, 128);
        addStream(*old, 0, 1, true, loud);
        cs.push(*old);
        CHECK(cs.epoch() == 2);
        CHECK_FALSE(cs.live(0, 1));
        ScopeBlock* nw = makeBlock(keep, 3, 0, 128);
        addStream(*nw, 0, 1, true, loud);
        cs.push(*nw);
        CHECK(cs.epoch() == 3);
        CHECK_FALSE(cs.live(0, 0));
        CHECK(cs.live(0, 1));
    }

    TEST_CASE("extract: trigger na narastajacym przejsciu przez srodek; szczyt okna")
    {
        std::vector<std::unique_ptr<ScopeBlock>> keep;
        ChannelScopeBuffer cs;
        for (std::uint64_t s = 0; s < 4096; s += 512) {
            ScopeBlock* b = makeBlock(keep, 1, s, 512);
            addStream(*b, 0, 0, true, sine100);
            cs.push(*b);
        }
        std::vector<float> w;
        REQUIRE(cs.extract(0, 0, 1000, 256, w));
        REQUIRE(w.size() == 256);
        // Okno zaczyna sie na narastajacym przejsciu przez zero: okres 100 klatek, start na f%100==0.
        CHECK(std::fabs(w[0]) < 0.06f);
        CHECK(w[3] > w[0]);
        float pk = 0.0f;
        REQUIRE(cs.peak(0, 0, 1000, 480, pk));
        CHECK(pk == doctest::Approx(0.5f).epsilon(0.02));
        CHECK_FALSE(cs.extract(0, 1, 1000, 256, w));      // brak strumienia
        CHECK_FALSE(cs.extract(0, 0, 1000, 4000, w));     // za dlugie okno
    }

    TEST_CASE("extract przy koncu danych cofa okno, nie zwraca smieci")
    {
        std::vector<std::unique_ptr<ScopeBlock>> keep;
        ChannelScopeBuffer cs;
        for (std::uint64_t s = 0; s < 2048; s += 512) {
            ScopeBlock* b = makeBlock(keep, 1, s, 512);
            addStream(*b, 0, 0, true, loud);
            cs.push(*b);
        }
        std::vector<float> w;
        REQUIRE(cs.extract(0, 0, 100000, 300, w));   // pozycja daleko za koncem
        for (float v : w) CHECK(v == doctest::Approx(0.8f));
    }
}

TEST_SUITE("ui/vu")
{
    TEST_CASE("poziom z szczytu: skala dB")
    {
        CHECK(vuLevelFromPeak(0.0f) == 0.0f);
        CHECK(vuLevelFromPeak(-1.0f) == 0.0f);
        CHECK(vuLevelFromPeak(1.0f) == doctest::Approx(1.0f));
        CHECK(vuLevelFromPeak(4.0f) == doctest::Approx(1.0f));       // obciecie
        CHECK(vuLevelFromPeak(0.5f) == doctest::Approx(1.0f - 6.0206f / kVuFloorDb).epsilon(0.001));
        CHECK(vuLevelFromPeak(1e-4f) == 0.0f);                       // ponizej progu
        CHECK(vuLevelFromPeak(0.1f) < vuLevelFromPeak(0.2f));
    }

    TEST_CASE("atak natychmiastowy, opadanie liniowe, szczyt trzymany")
    {
        VuMeter m;
        m.update(0.8f, 0.016f);
        CHECK(m.level == doctest::Approx(0.8f));      // atak w jednej klatce
        CHECK(m.peak == doctest::Approx(0.8f));
        m.update(0.0f, 0.1f);
        CHECK(m.level == doctest::Approx(0.8f - VuMeter::kFallPerSec * 0.1f));
        CHECK(m.peak == doctest::Approx(0.8f));       // trzymany
        for (int i = 0; i < 10; ++i) m.update(0.0f, 0.1f);
        CHECK(m.level == 0.0f);
        CHECK(m.peak < 0.8f);                         // po czasie trzymania opada
        for (int i = 0; i < 20; ++i) m.update(0.0f, 0.1f);
        CHECK(m.peak == 0.0f);
        CHECK_FALSE(m.active());
    }

    TEST_CASE("opadanie nie schodzi ponizej celu; wiekszy cel = nowy atak")
    {
        VuMeter m;
        m.update(0.9f, 0.01f);
        m.update(0.5f, 1.0f);                         // dluga klatka: nie ponizej celu
        CHECK(m.level == doctest::Approx(0.5f));
        m.update(0.7f, 0.01f);
        CHECK(m.level == doctest::Approx(0.7f));
    }

    TEST_CASE("liczba zapalonych segmentow")
    {
        CHECK(VuMeter::litSegments(0.0f, 16) == 0);
        CHECK(VuMeter::litSegments(0.001f, 16) == 1);
        CHECK(VuMeter::litSegments(0.5f, 16) == 8);
        CHECK(VuMeter::litSegments(1.0f, 16) == 16);
        CHECK(VuMeter::litSegments(2.0f, 16) == 16);
        CHECK(VuMeter::litSegments(0.5f, 0) == 0);
    }

    TEST_CASE("VuBank: kanaly i master niezaleznie")
    {
        VuBank bank;
        bank.resize(3);
        const float master[2] = {0.9f, 0.2f};
        bank.update({0.5f, 0.0f, 1.0f}, master, 0.016f);
        CHECK(bank.channel(0).level == doctest::Approx(0.5f));
        CHECK(bank.channel(1).level == 0.0f);
        CHECK(bank.channel(2).level == doctest::Approx(1.0f));
        CHECK(bank.masterL().level == doctest::Approx(0.9f));
        CHECK(bank.masterR().level == doctest::Approx(0.2f));
        CHECK(bank.active());
        bank.reset();
        CHECK_FALSE(bank.active());
    }
}

TEST_SUITE("ui/channel_grid")
{
    TEST_CASE("siatka dla 1..24 kanalow miesci sie w obszarze i ma sensowne proporcje")
    {
        const float sizes[][2] = {{540.0f, 250.0f}, {900.0f, 400.0f}, {1400.0f, 700.0f}, {300.0f, 600.0f}};
        for (const auto& sz : sizes) {
            for (int n = 1; n <= 24; ++n) {
                const GridLayout g = chooseScopeGrid(n, sz[0], sz[1], 3.0f);
                INFO("n=" << n << " w=" << sz[0] << " h=" << sz[1]);
                REQUIRE(g.cols >= 1);
                CHECK(g.cols * g.rows >= n);
                CHECK((g.rows - 1) * g.cols < n);                       // bez pustych wierszy
                CHECK(g.cols * g.cellW + (g.cols - 1) * g.gap <= sz[0] + 0.5f);
                CHECK(g.rows * g.cellH + (g.rows - 1) * g.gap <= sz[1] + 0.5f);
                CHECK(g.cellW >= 1.0f);
                CHECK(g.cellH >= 1.0f);
            }
        }
    }

    TEST_CASE("typowe uklady: szeroki obszar -> wiecej kolumn, 4 kanaly w jednym lub dwoch rzedach")
    {
        const GridLayout g4 = chooseScopeGrid(4, 900.0f, 300.0f, 3.0f);
        CHECK(g4.rows <= 2);
        const GridLayout g24 = chooseScopeGrid(24, 900.0f, 400.0f, 3.0f);
        CHECK(g24.cols >= 4);
        CHECK(g24.cols * g24.rows >= 24);
        CHECK(g24.cellW >= g24.cellH);                                 // ramki szersze niz wyzsze
        const GridLayout tall = chooseScopeGrid(6, 300.0f, 600.0f, 3.0f);
        CHECK(tall.cols <= 2);                                         // waski, wysoki obszar
        CHECK(chooseScopeGrid(0, 100.0f, 100.0f, 3.0f).cols == 0);
        CHECK(chooseScopeGrid(3, 0.0f, 100.0f, 3.0f).cols == 0);
    }

    TEST_CASE("komorki nie nachodza na siebie")
    {
        const GridLayout g = chooseScopeGrid(11, 800.0f, 300.0f, 4.0f);
        for (int i = 0; i + 1 < 11; ++i) {
            if ((i + 1) % g.cols != 0) CHECK(g.cellX(i + 1) >= g.cellX(i) + g.cellW + g.gap - 0.01f);
            else CHECK(g.cellY(i + 1) >= g.cellY(i) + g.cellH + g.gap - 0.01f);
        }
    }

    TEST_CASE("kursor kanalu po siatce")
    {
        CHECK(moveGridCursor(0, 1, 0, 4, 10) == 1);
        CHECK(moveGridCursor(9, 1, 0, 4, 10) == 9);       // koniec
        CHECK(moveGridCursor(0, -1, 0, 4, 10) == 0);
        CHECK(moveGridCursor(1, 0, 1, 4, 10) == 5);
        CHECK(moveGridCursor(6, 0, 1, 4, 10) == 6);       // brak komorki ponizej (indeks 10)
        CHECK(moveGridCursor(6, 0, -1, 4, 10) == 2);
        CHECK(moveGridCursor(2, 0, -1, 4, 10) == 2);
        CHECK(moveGridCursor(5, 0, 1, 4, 0) == 0);
        CHECK(moveGridCursor(50, 0, 0, 4, 10) == 9);      // obciecie
    }
}

TEST_SUITE("ui/channel_nav")
{
    SDL_Event key(SDL_Scancode sc, bool shift, bool down = true)
    {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = sc;
        e.key.mod = shift ? SDL_KMOD_LSHIFT : 0;
        return e;
    }
    SDL_Event pad(Uint8 button, bool down = true)
    {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = 7;
        e.gbutton.button = button;
        return e;
    }

    TEST_CASE("klawiatura: M/S/U/C/R, cyfry = kanaly, Shift+cyfra = solo, F4 = widok")
    {
        NavInput n;
        n.handleEvent(key(SDL_SCANCODE_M, false), 0);
        n.handleEvent(key(SDL_SCANCODE_S, false), 0);
        n.handleEvent(key(SDL_SCANCODE_U, false), 0);
        n.handleEvent(key(SDL_SCANCODE_C, false), 0);
        n.handleEvent(key(SDL_SCANCODE_R, false), 0);
        n.handleEvent(key(SDL_SCANCODE_F4, false), 0);
        n.handleEvent(key(SDL_SCANCODE_1, false), 0);
        n.handleEvent(key(SDL_SCANCODE_0, false), 0);
        n.handleEvent(key(SDL_SCANCODE_3, true), 0);
        const auto& ev = n.events();
        REQUIRE(ev.size() == 9);
        CHECK(ev[0].action == Action::ToggleMute);
        CHECK(ev[1].action == Action::ToggleSolo);
        CHECK(ev[2].action == Action::UnmuteAll);
        CHECK(ev[3].action == Action::ClearSolo);
        CHECK(ev[4].action == Action::ResetMix);
        CHECK(ev[5].action == Action::ToggleChannelsView);
        CHECK((ev[6].action == Action::MuteChannel && ev[6].arg == 0));
        CHECK((ev[7].action == Action::MuteChannel && ev[7].arg == 9));
        CHECK((ev[8].action == Action::SoloChannel && ev[8].arg == 2));
    }

    TEST_CASE("pad: X = solo, Y = reset, Select = widok; A (Accept) zostaje; bez fokusu okna nic")
    {
        NavInput n;
        n.handleEvent(pad(SDL_GAMEPAD_BUTTON_WEST), 0);
        n.handleEvent(pad(SDL_GAMEPAD_BUTTON_NORTH), 0);
        n.handleEvent(pad(SDL_GAMEPAD_BUTTON_BACK), 0);
        n.handleEvent(pad(SDL_GAMEPAD_BUTTON_SOUTH), 0);
        REQUIRE(n.events().size() == 4);
        CHECK(n.events()[0].action == Action::ToggleSolo);
        CHECK(n.events()[1].action == Action::ResetMix);
        CHECK(n.events()[2].action == Action::ToggleChannelsView);
        CHECK(n.events()[3].action == Action::Accept);

        NavInput nf;
        SDL_Event lost{};
        lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        nf.handleEvent(lost, 0);
        nf.handleEvent(pad(SDL_GAMEPAD_BUTTON_WEST), 0);
        nf.handleEvent(key(SDL_SCANCODE_M, false), 0);
        CHECK(nf.events().empty());
    }

    TEST_CASE("puszczenie klawisza nie jest gubione: kolejne nacisniecie dziala")
    {
        NavInput n;
        n.handleEvent(key(SDL_SCANCODE_M, false), 0);
        n.handleEvent(key(SDL_SCANCODE_M, false), 10);   // bez puszczenia: ignorowane
        CHECK(n.events().size() == 1);
        n.handleEvent(key(SDL_SCANCODE_M, false, false), 20);
        n.handleEvent(key(SDL_SCANCODE_M, false), 30);
        CHECK(n.events().size() == 2);
    }
}

// ------------------------------------------------------------------ PlayerScreen (naglowkowy ImGui)

namespace {

struct FakeActions final : PlayerActions {
    std::vector<std::string> log;
    int visible = -1;   // ostatnia deklaracja widocznosci (-1 = nie wolano); UI wola ja co klatke
    void requestOpenFile() override {}
    void togglePlay() override {}
    void play() override {}
    void pause() override {}
    void stop() override {}
    void seekToFrame(std::uint64_t) override {}
    void seekBySeconds(double) override {}
    void setVolume(float) override {}
    void dismissError() override {}
    void toggleChannelMute(int c) override { log.push_back("mute" + std::to_string(c)); }
    void toggleChannelSolo(int c) override { log.push_back("solo" + std::to_string(c)); }
    void muteAllChannels() override { log.push_back("muteall"); }
    void unmuteAllChannels() override { log.push_back("unmuteall"); }
    void clearSolo() override { log.push_back("clearsolo"); }
    void setChannelScopesVisible(bool v) override { visible = v ? 1 : 0; }
};

struct ImGuiHeadless {
    ImGuiHeadless()
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
    ~ImGuiHeadless() { ImGui::DestroyContext(ctx); }
    ImGuiContext* ctx;
};

PlayerView makeView(ChannelMix& mix, ChannelScopeBuffer& cs)
{
    auto info = std::make_shared<TrackInfo>();
    info->layout = makeLayout();
    mix.setLayout(info->layout);
    PlayerView v;
    v.info = info;
    v.track = std::make_shared<loader::LoadedTrack>();
    v.mix = &mix;
    v.channelScopes = &cs;
    return v;
}

} // namespace

TEST_SUITE("ui/channel_screen")
{
    TEST_CASE("PlayerScreen: akcje kanalu tylko w panelu Tracker, na aktywnym kanale")
    {
        ImGuiHeadless g;
        ChannelMix mix;
        ChannelScopeBuffer cs;
        PlayerView v = makeView(mix, cs);
        FakeActions act;
        PlayerScreen screen;
        NavInput nav;

        auto run = [&] {
            ImGui::NewFrame();
            screen.frame(nav, v, act, {});
            ImGui::Render();
        };
        run();
        CHECK(act.visible == 1);                 // widok kanalow widoczny przy zaladowanym utworze

        // Fokus na Transport: M/S sa ignorowane.
        act.log.clear();
        nav.setFocus(Panel::Transport);
        nav.events().push_back({Action::ToggleMute, false});
        nav.events().push_back({Action::ToggleSolo, false});
        run();
        CHECK(act.log.empty());

        // Fokus na Tracker: M = mute aktywnego kanalu (0), Right przesuwa, S = solo, Enter = mute.
        nav.setFocus(Panel::Tracker);
        nav.events().push_back({Action::ToggleMute, false});
        nav.events().push_back({Action::Right, false});
        nav.events().push_back({Action::ToggleSolo, false});
        nav.events().push_back({Action::Accept, false});
        run();
        REQUIRE(act.log.size() == 3);
        CHECK(act.log[0] == "mute0");
        CHECK(act.log[1] == "solo1");
        CHECK(act.log[2] == "mute1");

        act.log.clear();
        nav.events().push_back({Action::UnmuteAll, false});
        nav.events().push_back({Action::ClearSolo, false});
        nav.events().push_back({Action::ResetMix, false});
        nav.events().push_back({Action::MuteChannel, false, 4});
        nav.events().push_back({Action::SoloChannel, false, 99});   // poza zakresem: ignorowane
        run();
        const std::vector<std::string> want = {"unmuteall", "clearsolo", "unmuteall", "clearsolo", "mute4"};
        CHECK(act.log == want);
    }

    TEST_CASE("PlayerScreen: F4 przelacza widok; w widoku wzorca Up/Down nie rusza kanalu")
    {
        ImGuiHeadless g;
        ChannelMix mix;
        ChannelScopeBuffer cs;
        PlayerView v = makeView(mix, cs);
        FakeActions act;
        PlayerScreen screen;
        NavInput nav;
        nav.setFocus(Panel::Tracker);
        CHECK(screen.scopesView());
        nav.events().push_back({Action::ToggleChannelsView, false});
        ImGui::NewFrame();
        screen.frame(nav, v, act, {});
        ImGui::Render();
        CHECK_FALSE(screen.scopesView());
        nav.events().push_back({Action::Down, false});
        nav.events().push_back({Action::ToggleMute, false});
        ImGui::NewFrame();
        screen.frame(nav, v, act, {});
        ImGui::Render();
        CHECK(act.log.back() == "mute0");
    }

    TEST_CASE("PlayerScreen: rysowanie 4..24 kanalow w obu widokach bez bledow (headless)")
    {
        ImGuiHeadless g;
        for (int n : {0, 1, 4, 13, 24}) {
            for (bool scopes : {true, false}) {
                auto info = std::make_shared<TrackInfo>();
                DeviceDesc d;
                d.channelCount = static_cast<std::uint16_t>(n);
                d.mainChannelCount = d.channelCount;
                info->layout.devices = {d};
                for (int c = 0; c < n; ++c) info->layout.channels.push_back({0, static_cast<std::uint16_t>(c)});
                ChannelMix mix;
                mix.setLayout(info->layout);
                ChannelScopeBuffer cs;
                PlayerView v;
                v.info = info;
                v.track = std::make_shared<loader::LoadedTrack>();
                v.mix = &mix;
                v.channelScopes = &cs;
                v.transport.state = engine::TransportState::Playing;
                v.transport.epoch = 1;
                std::vector<std::unique_ptr<ScopeBlock>> keep;
                for (std::uint64_t s = 0; s < 4096; s += 512) {
                    ScopeBlock* b = makeBlock(keep, 1, s, 512);
                    for (int c = 0; c < std::min(n, 16); ++c) addStream(*b, 0, static_cast<std::uint16_t>(c), c != 2, sine100);
                    cs.push(*b);
                }
                FakeActions act;
                PlayerScreen screen;
                screen.setScopesView(scopes);
                NavInput nav;
                for (int f = 0; f < 3; ++f) {
                    ImGui::NewFrame();
                    screen.frame(nav, v, act, {});
                    ImGui::Render();
                }
                CHECK(screen.wantsAnimation(v));
            }
        }
    }
}

// ------------------------------------------------------------------ AppController

namespace {

template <class Pred>
bool pumpUntil(app::AppController& c, Pred p, int timeoutMs = 8000)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(timeoutMs)) {
        c.pump();
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_SUITE("app/channels")
{
    TEST_CASE("SetChannelScopes wysylany tylko przy zmianie widocznosci")
    {
        app::ControllerOptions opt;
        opt.nullAudio = true;
        app::AppController c(opt);
        REQUIRE(c.start());
        CHECK_FALSE(c.channelScopesRequested());
        CHECK(c.channelScopesCommandCount() == 0);
        c.setChannelScopesVisible(false);               // bez zmiany
        CHECK(c.channelScopesCommandCount() == 0);
        c.setChannelScopesVisible(true);
        c.setChannelScopesVisible(true);                // powtorzenie: bez komendy
        CHECK(c.channelScopesRequested());
        CHECK(c.channelScopesCommandCount() == 1);
        c.setChannelScopesVisible(false);
        c.setChannelScopesVisible(false);
        CHECK_FALSE(c.channelScopesRequested());
        CHECK(c.channelScopesCommandCount() == 2);
    }

    TEST_CASE("po zaladowaniu utworu stan zadany jest ponawiany (Load nie gwarantuje stanu silnika)")
    {
        app::ControllerOptions opt;
        opt.nullAudio = true;
        opt.autoplay = false;
        app::AppController c(opt);
        REQUIRE(c.start());
        c.setChannelScopesVisible(true);
        const unsigned before = c.channelScopesCommandCount();
        c.loadFile(kData / "valid" / "sn76489_scale_v100.vgm");
        REQUIRE(pumpUntil(c, [&] { return c.view().hasTrack(); }));
        CHECK(c.channelScopesCommandCount() == before + 1);
        CHECK(c.channelScopesRequested());
    }

    TEST_CASE("MUTE/SOLO: stan w widoku, ogolne wylaczniki, reset przy Load, ignorowanie bez utworu")
    {
        app::ControllerOptions opt;
        opt.nullAudio = true;
        opt.autoplay = false;
        app::AppController c(opt);
        REQUIRE(c.start());
        c.toggleChannelMute(0);                         // brak utworu: ignorowane
        REQUIRE(c.view().mix != nullptr);
        CHECK(c.view().mix->channelCount() == 0);

        c.loadFile(kData / "valid" / "dual_chip_ym2612_sn76489_v151.vgm");
        REQUIRE(pumpUntil(c, [&] { return c.view().hasTrack(); }));
        const ChannelMix& mix = *c.view().mix;
        REQUIRE(mix.channelCount() == static_cast<int>(c.view().info->layout.channels.size()));
        REQUIRE(mix.channelCount() > 3);

        c.toggleChannelMute(1);
        c.toggleChannelSolo(2);
        CHECK(mix.muted(1));
        CHECK(mix.solo(2));
        CHECK_FALSE(mix.audible(0));
        c.toggleChannelMute(999);                       // poza zakresem
        c.clearSolo();
        CHECK_FALSE(mix.anySolo());
        CHECK(mix.muted(1));
        c.muteAllChannels();
        CHECK(mix.muted(0));
        c.unmuteAllChannels();
        CHECK_FALSE(mix.anyMuted());

        c.toggleChannelMute(0);
        c.toggleChannelSolo(1);
        c.loadFile(kData / "valid" / "sn76489_scale_v100.vgm");   // nowy utwor
        REQUIRE(pumpUntil(c, [&] { return c.view().track && c.view().track->path.filename() == "sn76489_scale_v100.vgm" &&
                                          c.view().info; }));
        CHECK_FALSE(c.view().mix->anyMuted());
        CHECK_FALSE(c.view().mix->anySolo());
        CHECK(c.view().mix->channelCount() == static_cast<int>(c.view().info->layout.channels.size()));
    }

    TEST_CASE("--fake-channel-scopes: syntetyczne strumienie tylko po wlaczeniu widoku kanalow")
    {
        app::ControllerOptions opt;
        opt.nullAudio = true;
        opt.fakeChannelScopes = true;
        app::AppController c(opt);
        REQUIRE(c.start());
        c.loadFile(kData / "valid" / "dual_chip_ym2612_sn76489_v151.vgm");
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == engine::TransportState::Playing &&
                                          c.view().transport.audibleFrame > 4800; }));
        // Widok ukryty: brak strumieni (silnik nie liczy, fake tez nie).
        CHECK(c.view().channelScopes->liveCount() == 0);
        c.setChannelScopesVisible(true);
        REQUIRE(pumpUntil(c, [&] { return c.view().channelScopes->liveCount() > 0; }));
        CHECK(c.view().channelScopes->epoch() == c.view().transport.epoch);
        const auto& ref = c.view().info->layout.channels[0];
        CHECK(c.view().channelScopes->live(ref.deviceIdx, ref.channelIdx));
        c.setChannelScopesVisible(false);
        REQUIRE(pumpUntil(c, [&] { return c.view().channelScopes->liveCount() == 0; }));
    }
}
