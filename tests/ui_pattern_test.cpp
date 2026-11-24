// Testy widoku wzorca w stylu FT2 (E1.5): pozycja -> wiersz (z petla), notacja komorek, mapowanie
// kolumn na ChannelLayout, presety rowTicks, wzmocnienie przebiegow/VU, przeliczanie wzorca w
// AppController po zmianie rowTicks oraz rysowanie PlayerScreen (headless).
#include <doctest/doctest.h>

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app_controller.h"
#include "vgmplay_ui/channel_mix.h"
#include "vgmplay_ui/channel_scopes.h"
#include "vgmplay_ui/nav_input.h"
#include "vgmplay_ui/pattern_view.h"
#include "vgmplay_ui/player_screen.h"

using namespace vgmplay;
using namespace vgmplay::ui;

namespace {

const std::filesystem::path kData = VGMPLAY_TEST_DATA_DIR;

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

// Uklad: YM2612 (6 kanalow) + SN76489 (4 kanaly).
ChannelLayout twoChipLayout()
{
    ChannelLayout l;
    DeviceDesc a;
    a.id = 0;
    a.type = ChipType::YM2612;
    a.channelCount = 6;
    a.mainChannelCount = 6;
    DeviceDesc b;
    b.id = 1;
    b.type = ChipType::SN76496;
    b.channelCount = 4;
    b.mainChannelCount = 4;
    l.devices = {a, b};
    for (std::uint16_t c = 0; c < 6; ++c) l.channels.push_back({0, c});
    for (std::uint16_t c = 0; c < 4; ++c) l.channels.push_back({1, c});
    return l;
}

chipstate::PatternChannel pc(ChipType t, std::uint8_t inst, std::uint16_t idx, bool decoded = true)
{
    chipstate::PatternChannel c;
    c.type = t;
    c.instance = inst;
    c.channelIdx = idx;
    c.decoded = decoded;
    return c;
}

} // namespace

TEST_SUITE("ui/pattern_position")
{
    TEST_CASE("klatki silnika -> ticki 44100 Hz (bez przepelnien)")
    {
        CHECK(framesToTicks(48000, 48000) == 44100);
        CHECK(framesToTicks(24000, 48000) == 22050);
        CHECK(framesToTicks(44100, 44100) == 44100);
        CHECK(framesToTicks(1, 0) == 0);
        // ~ 10^13 klatek (setki tysiecy godzin): wynik bez przepelnienia uint64
        CHECK(framesToTicks(10'000'000'000'000ull, 48000) == 9'187'500'000'000ull);
    }

    TEST_CASE("wiersz = ticki / rowTicks; kursor stoi, wzorzec sie przesuwa")
    {
        PatternTiming t;
        t.rowTicks = 735;
        t.rowCount = 1000;
        CHECK(rowAtFrame(0, 48000, t) == 0);
        CHECK(rowAtFrame(799, 48000, t) == 0);     // 799 klatek = 734 ticki
        CHECK(rowAtFrame(800, 48000, t) == 1);     // 800 klatek = 735 tickow
        CHECK(rowAtFrame(48000, 48000, t) == 60);  // 1 s = 60 wierszy
        t.rowTicks = 882;                          // 1/50 s
        CHECK(rowAtFrame(48000, 48000, t) == 50);
    }

    TEST_CASE("koniec danych: bez petli ostatni wiersz, z petla zawijanie od loopStartRow")
    {
        PatternTiming t;
        t.rowTicks = 735;
        t.rowCount = 100;
        t.loopStartRow = 40;                       // dlugosc petli = 60 wierszy
        t.hasLoop = false;
        CHECK(rowAtFrame(48000ull * 3, 48000, t) == 99);
        t.hasLoop = true;
        const auto frameOfRow = [&](std::uint64_t row) { return row * 735 * 48000 / 44100 + 1; };
        CHECK(rowAtFrame(frameOfRow(99), 48000, t) == 99);
        CHECK(rowAtFrame(frameOfRow(100), 48000, t) == 40);   // pierwszy wiersz po koncu = poczatek petli
        CHECK(rowAtFrame(frameOfRow(101), 48000, t) == 41);
        CHECK(rowAtFrame(frameOfRow(159), 48000, t) == 99);
        CHECK(rowAtFrame(frameOfRow(160), 48000, t) == 40);   // kolejne przejscie petli
        CHECK(rowAtFrame(frameOfRow(100 + 60 * 7 + 5), 48000, t) == 45);
    }

    TEST_CASE("przypadki brzegowe: pusty wzorzec, petla poza danymi")
    {
        PatternTiming t;
        CHECK(rowAtFrame(123456, 48000, t) == 0);             // rowCount == 0
        t.rowCount = 10;
        t.hasLoop = true;
        t.loopStartRow = 10;                                   // poczatek petli poza danymi: jak brak petli
        CHECK(rowAtFrame(48000ull * 100, 48000, t) == 9);
        CHECK_FALSE(displayRow(10, t).has_value());
    }

    TEST_CASE("displayRow: wiersze przed koncem pokazuja juz poczatek petli")
    {
        PatternTiming t;
        t.rowCount = 100;
        t.loopStartRow = 40;
        t.hasLoop = true;
        CHECK_FALSE(displayRow(-1, t).has_value());
        CHECK(displayRow(0, t) == 0);
        CHECK(displayRow(99, t) == 99);
        CHECK(displayRow(100, t) == 40);
        CHECK(displayRow(103, t) == 43);
        CHECK(displayRow(160, t) == 40);
        t.hasLoop = false;
        CHECK_FALSE(displayRow(100, t).has_value());
    }

    TEST_CASE("timingOf bierze dane z wzorca")
    {
        chipstate::Pattern p;
        p.rowTicks = 882;
        p.rowCount = 7;
        p.loopStartRow = 3;
        const PatternTiming t = timingOf(p, true);
        CHECK(t.rowTicks == 882);
        CHECK(t.rowCount == 7);
        CHECK(t.loopStartRow == 3);
        CHECK(t.hasLoop);
        p.rowTicks = 0;
        CHECK(timingOf(p, false).rowTicks == kDefaultRowTicks);
    }
}

TEST_SUITE("ui/pattern_cells")
{
    TEST_CASE("nuty MIDI -> notacja FT2 (60 = C-4)")
    {
        char b[4];
        formatNote(60, b);
        CHECK(std::string(b) == "C-4");
        formatNote(61, b);
        CHECK(std::string(b) == "C#4");
        formatNote(69, b);
        CHECK(std::string(b) == "A-4");
        formatNote(12, b);
        CHECK(std::string(b) == "C-0");
        formatNote(127, b);
        CHECK(std::string(b) == "G-9");
        formatNote(11, b);
        CHECK(std::string(b) == "B-_");                       // oktawa -1: "_" zamiast cyfry
        formatNote(1, b);
        CHECK(std::string(b) == "C#_");
        formatNote(0, b);
        CHECK(std::string(b) == "C-_");
        formatNote(chipstate::kNoteNone, b);
        CHECK(std::string(b) == "---");
        formatNote(chipstate::kNoteOff, b);
        CHECK(std::string(b) == "===");
        formatNote(200, b);
        CHECK(std::string(b) == "???");
        formatNote(-7, b);
        CHECK(std::string(b) == "???");
    }

    TEST_CASE("komorka: instrument/glosnosc hex 2 cyfry, brak = .., efekt = znak + parametr")
    {
        chipstate::Cell c;
        CellText t = formatCell(c);
        CHECK(std::string(t.note) == "---");
        CHECK(std::string(t.inst) == "..");
        CHECK(std::string(t.vol) == "..");
        CHECK(std::string(t.fx) == "...");
        CHECK_FALSE(cellHasEvent(c));

        c.note = 72;
        c.instrument = 0x0A;
        c.volume = 0x40;
        c.effect = 4;
        c.effectParam = 0x37;
        t = formatCell(c);
        CHECK(std::string(t.note) == "C-5");
        CHECK(std::string(t.inst) == "0A");
        CHECK(std::string(t.vol) == "40");
        CHECK(std::string(t.fx) == "437");
        CHECK(cellHasEvent(c));

        c.effect = 10;                                           // 10..35 -> A..Z
        c.effectParam = 0xFF;
        CHECK(std::string(formatCell(c).fx) == "AFF");
        c.effect = 40;
        CHECK(std::string(formatCell(c).fx) == "?FF");

        chipstate::Cell off;
        off.note = chipstate::kNoteOff;
        CHECK(std::string(formatCell(off).note) == "===");
        CHECK(cellHasEvent(off));
        chipstate::Cell volOnly;
        volOnly.volume = 0x20;
        CHECK(cellHasEvent(volOnly));
        CHECK(std::string(formatCell(volOnly).note) == "---");
    }

    TEST_CASE("szerokosc pol jest stala")
    {
        for (int note : {-2, -1, 0, 11, 12, 60, 127}) {
            chipstate::Cell c;
            c.note = static_cast<std::int16_t>(note);
            c.instrument = note > 0 ? 0xFE : 0xFF;
            c.volume = note > 0 ? 0x00 : 0xFF;
            const CellText t = formatCell(c);
            CHECK(std::string(t.note).size() == 3);
            CHECK(std::string(t.inst).size() == 2);
            CHECK(std::string(t.vol).size() == 2);
            CHECK(std::string(t.fx).size() == 3);
        }
    }

    TEST_CASE("patternHasEffects")
    {
        chipstate::Pattern p;
        p.channels = {pc(ChipType::YM2612, 0, 0)};
        p.cells.assign(5, chipstate::Cell{});
        CHECK_FALSE(patternHasEffects(p));
        p.cells[3].effectParam = 1;
        CHECK(patternHasEffects(p));
    }
}

TEST_SUITE("ui/pattern_columns")
{
    TEST_CASE("kolumny wzorca <-> ChannelLayout po (typ, instancja, indeks kanalu)")
    {
        const ChannelLayout l = twoChipLayout();
        chipstate::Pattern p;
        // kolejnosc kolumn celowo inna niz kanalow globalnych
        p.channels = {pc(ChipType::SN76496, 0, 0), pc(ChipType::YM2612, 0, 2), pc(ChipType::YM2612, 0, 0),
                      pc(ChipType::SN76496, 0, 3)};
        const std::vector<int> m = mapPatternColumns(l, &p);
        REQUIRE(m.size() == 10);
        CHECK(m[0] == 2);    // YM2612 #0 kanal 0
        CHECK(m[1] == -1);   // brak kolumny
        CHECK(m[2] == 1);    // YM2612 kanal 2
        CHECK(m[6] == 0);    // SN76496 kanal 0
        CHECK(m[9] == 3);    // SN76496 kanal 3
        CHECK(m[7] == -1);
    }

    TEST_CASE("instancja ukladu rozroznia kolumny; decoded=false -> n/a")
    {
        ChannelLayout l;
        DeviceDesc a, b;
        a.id = 0; a.type = ChipType::YM2151; a.instance = 0; a.channelCount = 2; a.mainChannelCount = 2;
        b.id = 1; b.type = ChipType::YM2151; b.instance = 1; b.channelCount = 2; b.mainChannelCount = 2;
        l.devices = {a, b};
        l.channels = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
        chipstate::Pattern p;
        p.channels = {pc(ChipType::YM2151, 1, 1), pc(ChipType::YM2151, 0, 0), pc(ChipType::YM2151, 1, 0, false)};
        const std::vector<int> m = mapPatternColumns(l, &p);
        REQUIRE(m.size() == 4);
        CHECK(m[0] == 1);
        CHECK(m[1] == -1);
        CHECK(m[2] == -1);   // niezdekodowany
        CHECK(m[3] == 0);
    }

    TEST_CASE("brak wzorca albo pusty wzorzec (zaslepka): wszystkie kanaly n/a")
    {
        const ChannelLayout l = twoChipLayout();
        for (int v : mapPatternColumns(l, nullptr)) CHECK(v == -1);
        chipstate::Pattern empty;
        for (int v : mapPatternColumns(l, &empty)) CHECK(v == -1);
    }

    TEST_CASE("kanal spoza listy urzadzen nie wywraca mapowania")
    {
        ChannelLayout l = twoChipLayout();
        l.channels.push_back({9, 0});
        chipstate::Pattern p;
        p.channels = {pc(ChipType::YM2612, 0, 0)};
        const std::vector<int> m = mapPatternColumns(l, &p);
        REQUIRE(m.size() == 11);
        CHECK(m[10] == -1);
    }
}

TEST_SUITE("ui/pattern_rowticks")
{
    TEST_CASE("presety: 1/120, 1/60, 1/50 w gore i w dol")
    {
        CHECK(kDefaultRowTicks == 2205);   // 1/20 s (decyzja uzytkownika 2026-11-24)
        CHECK(stepRowTicks(735, 1) == 368);      // krotszy wiersz: 1/120 s
        CHECK(stepRowTicks(735, -1) == 882);     // 1/50 s
        CHECK(stepRowTicks(368, 1) == 368);      // obciete do zakresu
        CHECK(stepRowTicks(kRowTickPresets[kRowTickPresetCount - 1], -1) == kRowTickPresets[kRowTickPresetCount - 1]);
        CHECK(stepRowTicks(740, -1) == 882);     // wartosc spoza listy: najblizszy preset
        CHECK(stepRowTicks(0, 1) == 1470);       // 0 = domyslny (2205 = 1/20 s) -> krotszy: 1/30 s
    }

    TEST_CASE("etykiety")
    {
        CHECK(rowTicksLabel(735) == "1/60 s");
        CHECK(rowTicksLabel(882) == "1/50 s");
        CHECK(rowTicksLabel(368) == "1/120 s");
        CHECK(rowTicksLabel(4410) == "1/10 s");
        CHECK(rowTicksLabel(0) == "1/20 s");
    }
}

TEST_SUITE("ui/pattern_fake")
{
    TEST_CASE("sztuczny wzorzec: ksztalt zgodny z kontraktem i niezalezny od rowTicks")
    {
        const ChannelLayout l = twoChipLayout();
        const std::uint64_t total = 44100ull * 10;
        const auto a = makeFakePattern(l, total, 44100ull * 4, true, 735);
        const auto b = makeFakePattern(l, total, 44100ull * 4, true, 1470);
        REQUIRE(a);
        CHECK(a->rowTicks == 735);
        CHECK(a->rowCount == total / 735 + 1);
        CHECK(a->loopStartRow == 44100ull * 4 / 735);
        CHECK(a->channels.size() == 10);
        CHECK(a->cells.size() == a->rowCount * 10);
        CHECK(a->channels[0].type == ChipType::YM2612);
        CHECK(a->channels[6].type == ChipType::SN76496);
        CHECK(a->channels[6].channelIdx == 0);
        CHECK_FALSE(a->channels[5].decoded);      // >= 8 kanalow: kanal 5 = n/a
        // Liczba zdarzen kanalu 0 podobna dla obu rowTicks (zdarzenia zaleza od czasu).
        auto count = [](const chipstate::Pattern& p, std::size_t ch) {
            std::size_t n = 0;
            for (std::uint64_t r = 0; r < p.rowCount; ++r)
                if (cellHasEvent(p.at(r, ch))) ++n;
            return n;
        };
        const std::size_t na = count(*a, 0), nb = count(*b, 0);
        CHECK(na > 20);
        CHECK(nb > 20);
        CHECK(nb <= na);
        CHECK(count(*a, 5) == 0);                 // niezdekodowany: puste komorki
        const auto none = makeFakePattern(l, total, 0, false, 735);
        CHECK(none->loopStartRow == 0);
    }
}

TEST_SUITE("ui/scope_gain")
{
    TEST_CASE("auto-wzmocnienie: szczyt 0,26 bez przyciecia, cichy kanal wzmocniony, granice")
    {
        ScopeAutoGain g;
        float k = g.update(0.26f, 0.016f);
        CHECK(0.26f * k <= ScopeAutoGain::kFill + 1e-4f);
        CHECK(k == doctest::Approx(ScopeAutoGain::kFill / 0.26f).epsilon(0.01));
        ScopeAutoGain q;
        CHECK(q.update(0.06f, 0.016f) == doctest::Approx(ScopeAutoGain::kFill / 0.06f).epsilon(0.01));
        ScopeAutoGain z;
        CHECK(z.update(0.0f, 0.016f) == ScopeAutoGain::kMaxGain);    // cisza: bez dzielenia przez zero
        ScopeAutoGain l;
        CHECK(l.update(1.0f, 0.016f) >= ScopeAutoGain::kMinGain);    // nigdy ponizej 1
        CHECK(l.update(1.0f, 0.016f) * 1.0f <= 1.0f);                // pelna skala nie przycina
    }

    TEST_CASE("obwiednia rosnie natychmiast, opada wolno (wzmocnienie nie pompuje)")
    {
        ScopeAutoGain g;
        g.update(0.5f, 0.016f);
        const float k0 = g.gain();
        g.update(0.1f, 0.1f);                                        // ciszej po glosnym dzwieku
        CHECK(g.gain() > k0);
        CHECK(g.gain() < 1.1f * k0);                                 // po 100 ms prawie bez zmiany
        for (int i = 0; i < 100; ++i) g.update(0.1f, 0.1f);          // 10 s
        CHECK(g.gain() == doctest::Approx(ScopeAutoGain::kFill / 0.1f).epsilon(0.05));
        const float before = g.gain();
        g.update(0.4f, 0.016f);                                      // nowy glosny dzwiek: natychmiast mniej
        CHECK(g.gain() < before * 0.5f);
        CHECK(0.4f * g.gain() <= ScopeAutoGain::kFill + 1e-4f);
    }

    TEST_CASE("VU kanalu: skala konczy sie wyzej niz master")
    {
        // typowe amplitudy strumieni kanalow: szczyt 0,26 prawie na gorze, 0,06 srodek, 0,02 jeszcze widac
        const float loud = vuLevelFromPeak(0.26f, kChannelVuTopDb);
        const float mid = vuLevelFromPeak(0.06f, kChannelVuTopDb);
        const float quiet = vuLevelFromPeak(0.02f, kChannelVuTopDb);
        CHECK(loud > 0.85f);
        CHECK(loud < 1.0f);
        CHECK(mid > 0.45f);
        CHECK(mid < 0.7f);
        CHECK(quiet > 0.2f);
        CHECK(vuLevelFromPeak(0.0005f, kChannelVuTopDb) == 0.0f);
        // master (szczyt 0,58) nie przyklejony do gory
        CHECK(vuLevelFromPeak(0.58f) < 0.95f);
        CHECK(vuLevelFromPeak(0.58f) > 0.8f);
        CHECK(vuLevelFromPeak(1.0f) == doctest::Approx(1.0f));
    }
}

TEST_SUITE("app/pattern")
{
    TEST_CASE("wzorzec liczony w tle po zaladowaniu; zmiana rowTicks przelicza wzorzec")
    {
        app::ControllerOptions opt;
        opt.nullAudio = true;
        app::AppController c(opt);
        REQUIRE(c.start());
        CHECK(c.view().patternRowTicks == kDefaultRowTicks);
        c.setPatternRowTicks(735);
        c.loadFile(kData / "valid" / "ym2612_melody_loop_v150_gd3jp.vgm");
        REQUIRE(pumpUntil(c, [&] { return c.view().pattern != nullptr; }));
        CHECK(c.view().pattern->rowTicks == 735);
        CHECK(c.view().pattern->rowCount > 1);
        CHECK_FALSE(c.view().patternBuilding);
        const std::uint64_t rows735 = c.view().pattern->rowCount;

        c.setPatternRowTicks(1470);
        CHECK(c.view().patternRowTicks == 1470);
        REQUIRE(pumpUntil(c, [&] { return c.view().pattern && c.view().pattern->rowTicks == 1470; }));
        const std::uint64_t rows1470 = c.view().pattern->rowCount;
        CHECK(rows1470 < rows735);
        CHECK(rows1470 * 2 + 2 >= rows735);
        CHECK(rows1470 * 2 <= rows735 + 2);
        CHECK_FALSE(c.view().patternBuilding);

        c.setPatternRowTicks(1470);                       // bez zmiany: bez przeliczania
        c.pump();
        CHECK_FALSE(c.view().patternBuilding);
        c.setPatternRowTicks(1);                          // obciete do zakresu
        CHECK(c.view().patternRowTicks >= 64);
    }

    TEST_CASE("zmiana rowTicks w trakcie liczenia pierwszego wzorca konczy na zadanej wartosci")
    {
        app::ControllerOptions opt;
        opt.nullAudio = true;
        app::AppController c(opt);
        REQUIRE(c.start());
        c.loadFile(kData / "valid" / "ym2612_melody_loop_v150_gd3jp.vgm");
        c.setPatternRowTicks(882);
        REQUIRE(pumpUntil(c, [&] { return c.view().pattern && c.view().pattern->rowTicks == 882; }));
        CHECK_FALSE(c.view().patternBuilding);
    }

    TEST_CASE("--fake-pattern: kolumny z ukladu, mapowanie i petla; nowy utwor odrzuca stary wzorzec")
    {
        app::ControllerOptions opt;
        opt.nullAudio = true;
        opt.fakePattern = true;
        app::AppController c(opt);
        REQUIRE(c.start());
        c.loadFile(kData / "valid" / "dual_chip_ym2612_sn76489_v151.vgm");
        REQUIRE(pumpUntil(c, [&] { return c.view().pattern != nullptr && c.view().info; }));
        const auto& layout = c.view().info->layout;
        CHECK(c.view().pattern->channels.size() == layout.channels.size());
        const std::vector<int> m = mapPatternColumns(layout, c.view().pattern.get());
        int mapped = 0;
        for (int v : m) mapped += v >= 0 ? 1 : 0;
        CHECK(mapped >= static_cast<int>(layout.channels.size()) - 1);   // najwyzej kanal n/a
        CHECK(c.view().patternHasEffects);

        c.loadFile(kData / "valid" / "sn76489_scale_v100.vgm");
        REQUIRE(pumpUntil(c, [&] {
            return c.view().track && c.view().track->path.filename() == "sn76489_scale_v100.vgm" && c.view().pattern;
        }));
        CHECK(c.view().pattern->channels.size() == c.view().info->layout.channels.size());
    }
}

// ------------------------------------------------------------------ PlayerScreen z wzorcem (headless)

namespace {

struct NullActions final : PlayerActions {
    void requestOpenFile() override {}
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

} // namespace

TEST_SUITE("ui/pattern_screen")
{
    TEST_CASE("PlayerScreen: wiersz pod kursorem = pozycja slyszana, takze po petli; kolumny przewijaja sie")
    {
        Headless g;
        auto info = std::make_shared<TrackInfo>();
        info->layout = twoChipLayout();
        info->introFrames = 48000;
        info->loopFrames = 48000;
        ChannelMix mix;
        mix.setLayout(info->layout);
        ChannelScopeBuffer cs;
        PlayerView v;
        v.info = info;
        v.track = std::make_shared<loader::LoadedTrack>();
        v.mix = &mix;
        v.channelScopes = &cs;
        v.sampleRate = 48000;
        v.transport.state = engine::TransportState::Paused;
        v.transport.epoch = 1;
        v.pattern = makeFakePattern(info->layout, 44100ull * 2, 44100, true, 735);   // 120 wierszy + 1, petla od 60
        v.patternHasEffects = true;
        NullActions act;
        PlayerScreen screen;
        NavInput nav;
        auto run = [&] {
            ImGui::NewFrame();
            screen.frame(nav, v, act, {});
            ImGui::Render();
        };

        v.transport.audibleFrame = 0;
        run();
        CHECK(screen.currentRow() == 0);
        v.transport.audibleFrame = 48000;                // 1 s
        run();
        CHECK(screen.currentRow() == 60);
        v.transport.audibleFrame = 48000 * 2 + 8000;     // po koncu danych: 2,17 s -> petla
        run();
        CHECK(screen.currentRow() >= 60);
        CHECK(screen.currentRow() < 121);
        CHECK(screen.visibleColumns() >= 1);
        CHECK(screen.visibleColumns() <= 10);

        // Wiele kanalow: kursor kanalu przewija kolumny.
        for (int i = 0; i < 12; ++i) {
            nav.setFocus(Panel::Tracker);
            nav.events().push_back({Action::Right, false});
            run();
        }
        CHECK(screen.firstColumn() + screen.visibleColumns() >= 10);
        v.pattern.reset();                                // brak wzorca: komunikat, bez bledu
        run();
        CHECK(screen.currentRow() == -1);
        v.patternBuilding = true;
        run();
    }

    TEST_CASE("PlayerScreen: wzorzec zaslepki (kanaly puste) rysuje kolumny n/a")
    {
        Headless g;
        auto info = std::make_shared<TrackInfo>();
        info->layout = twoChipLayout();
        ChannelMix mix;
        mix.setLayout(info->layout);
        ChannelScopeBuffer cs;
        PlayerView v;
        v.info = info;
        v.track = std::make_shared<loader::LoadedTrack>();
        v.mix = &mix;
        v.channelScopes = &cs;
        auto p = std::make_shared<chipstate::Pattern>();
        p->rowCount = 500;                                // jak zaslepka: bez kanalow i komorek
        v.pattern = p;
        NullActions act;
        PlayerScreen screen;
        NavInput nav;
        for (int f = 0; f < 3; ++f) {
            ImGui::NewFrame();
            screen.frame(nav, v, act, {});
            ImGui::Render();
        }
        CHECK(screen.currentRow() == 0);
    }
}
