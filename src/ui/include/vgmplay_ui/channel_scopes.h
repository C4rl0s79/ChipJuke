// Dane i logika widoku kanalow (bez ImGui, tylko watek UI):
//  * ChannelScopeBuffer - historia strumieni kanalow z ScopeBlock::extra (oscyloskopy FT2, VU),
//  * VuMeter / VuBank   - wskazniki poziomu w stylu ProTrackera (szybki atak, powolne opadanie),
//  * chooseScopeGrid    - uklad ramek oscyloskopow dla N kanalow,
//  * moveGridCursor     - ruch kursora kanalu po siatce.
//
// ZALEZNOSC OD KONTRAKTU (include/vgmplay/layout.hpp): konsument czyta wylacznie
// ScopeBlock::{epoch, startFrame, frames, extraStreamCount, extraDesc[i].{deviceIdx, channelIdx,
// flags & kScopeStreamAvailable}, extra[i][0..frames)}. Strumien identyfikuje para (deviceIdx,
// channelIdx), nie pozycja i - kolejnosc/liczba strumieni moze sie zmieniac miedzy blokami.
// Strumien jest "zywy", gdy ostatni blok go zawieral z flaga dostepnosci; w przeciwnym razie UI
// pokazuje "n/a".
#pragma once

#include "vgmplay/layout.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vgmplay::ui {

class ChannelScopeBuffer {
public:
    static constexpr std::size_t kCapacity = 1u << 15; // klatek na strumien (potega 2)
    static constexpr std::size_t kMaxSlots = 64;       // strumienie ponad limit sa ignorowane

    void reset();
    void push(const ScopeBlock& block);

    std::uint64_t epoch() const { return epoch_; }
    // Strumien (urzadzenie, kanal) dostepny w ostatnim bloku.
    bool live(DeviceId device, std::uint16_t channel) const;
    std::size_t liveCount() const;
    std::size_t slotCount() const { return slots_.size(); }

    // Okno `count` klatek (wartosci -1..1) z triggerem: pierwsze narastajace przejscie przez
    // srodek zakresu (min+max)/2 w [audible, audible + count). false = brak danych / strumien martwy.
    bool extract(DeviceId device, std::uint16_t channel, std::uint64_t audibleFrame, std::size_t count,
                 std::vector<float>& out) const;
    // Szczyt |x| w oknie `count` klatek zaczynajacym sie od pozycji slyszanej (okno cofane, gdy
    // pozycja jest przy koncu danych). false = brak danych / strumien martwy.
    bool peak(DeviceId device, std::uint16_t channel, std::uint64_t audibleFrame, std::size_t count,
              float& out) const;

private:
    struct Slot {
        DeviceId device = 0;
        std::uint16_t channel = 0;
        bool valid = false;            // ostatni opis mial flage dostepnosci
        std::uint64_t begin = 0, end = 0;
        std::vector<float> data;       // kCapacity (alokowane przy pierwszym uzyciu)
    };
    const Slot* find(DeviceId device, std::uint16_t channel) const;
    bool isLive(const Slot& s) const { return s.valid && s.end == lastEnd_ && s.end > s.begin; }

    std::vector<Slot> slots_;
    std::uint64_t epoch_ = 0;
    std::uint64_t lastEnd_ = 0;
};

// Poziom 0..1 ze szczytu amplitudy (liniowa, 1.0 = pelna skala): skala dB od (topDb - kVuFloorDb)
// do topDb. Master: topDb = 0 (szczyt mastera ok. 0,6 = -4 dB). Kanaly: strumienie kanalow sa
// cichsze niz suma (szczyt do ok. 0,26, typowo 0,06-0,2), wiec ich skala konczy sie wyzej
// (kChannelVuTopDb) - glosny kanal dobija do gory, a kanal o szczycie 0,02 nadal zapala segmenty.
inline constexpr float kVuFloorDb = 36.0f;
inline constexpr float kChannelVuTopDb = -9.0f;
float vuLevelFromPeak(float peak, float topDb = 0.0f);

// Automatyczne wzmocnienie przebiegu kanalu (wspolne dla wszystkich ramek, per kanal): obwiednia
// szczytu rosnie natychmiast, opada wolno (kEnvDecaySec), a wzmocnienie = kFill / obwiednia w
// granicach [kMinGain, kMaxGain]. Dzieki temu glosny kanal nie przycina sie, a cichy jest czytelny;
// wolne opadanie nie "pompuje" obrazu (jak AGC w trackerach/odtwarzaczach, a nie normalizacja
// klatka po klatce). Stale wzmocnienie jest zawsze kompromisem: x4 przycina szczyty 0,26 i niemal
// nie pokazuje kanalow o szczycie 0,02.
struct ScopeAutoGain {
    static constexpr float kFill = 0.85f;        // czesc polowy wysokosci zajmowana przez szczyt
    static constexpr float kMinGain = 1.0f;
    static constexpr float kMaxGain = 16.0f;
    static constexpr float kEnvDecaySec = 1.5f;  // stala czasowa opadania obwiedni

    float env = 0.0f;
    // windowPeak: szczyt |x| w rysowanym oknie; zwraca wzmocnienie do uzycia w tej klatce.
    float update(float windowPeak, float dtSeconds);
    float gain() const;
};

// Wskaznik poziomu: atak natychmiastowy, opadanie liniowe (kFallPerSec / s), szczyt trzymany
// kPeakHoldSec, potem opada szybciej niz poziom.
struct VuMeter {
    static constexpr float kFallPerSec = 1.6f;
    static constexpr float kPeakHoldSec = 0.6f;
    static constexpr float kPeakFallPerSec = 1.0f;

    float level = 0.0f;
    float peak = 0.0f;
    float peakAge = 0.0f;

    void update(float target, float dtSeconds);
    bool active() const { return level > 0.0f || peak > 0.0f; }
    // Liczba zapalonych segmentow z `segments` (>0 dla level > 0).
    static int litSegments(float level, int segments);
};

class VuBank {
public:
    void resize(std::size_t channels);
    void update(const std::vector<float>& targets, const float master[2], float dtSeconds);
    void reset();
    bool active() const;
    const VuMeter& channel(std::size_t i) const { return channels_[i]; }
    std::size_t size() const { return channels_.size(); }
    const VuMeter& masterL() const { return master_[0]; }
    const VuMeter& masterR() const { return master_[1]; }

private:
    std::vector<VuMeter> channels_;
    VuMeter master_[2];
};

struct GridLayout {
    int cols = 0, rows = 0;
    float cellW = 0.0f, cellH = 0.0f;
    float gap = 0.0f;
    // Lewy gorny rog komorki `i` wzgledem lewego gornego rogu obszaru.
    float cellX(int i) const { return static_cast<float>(i % cols) * (cellW + gap); }
    float cellY(int i) const { return static_cast<float>(i / cols) * (cellH + gap); }
};

// Dobiera liczbe kolumn tak, by ramki byly jak najwieksze i miescily sie w (w x h); proporcje
// ramki 1:1..4:1 sa preferowane. Wynik zawsze miesci sie w obszarze (komorki sie kurcza).
GridLayout chooseScopeGrid(int count, float w, float h, float gap);

// Ruch kursora po siatce `cols` kolumn: dx/dy w komorkach; wynik zawsze w [0, count).
int moveGridCursor(int current, int dx, int dy, int cols, int count);

} // namespace vgmplay::ui
