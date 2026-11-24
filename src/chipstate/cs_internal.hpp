// vgmplay - modul chipstate: deklaracje wewnetrzne (NIE czesc kontraktu).
// Recorder (komorki wzorca), Host (czas + rejestrator), interfejs dekodera ukladu, pomocnicze funkcje
// (czestotliwosc -> MIDI, skalowanie glosnosci, rejestr instrumentow).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "vgmplay/chipstate.hpp"

namespace vgmplay::chipstate::detail {

inline constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();

// ---------------------------------------------------------------------------------------------
// Recorder: zbiera zdarzenia do wierszy. Czas rosnie monotonicznie (advanceTo). Reguly laczenia zdarzen
// w jednym wierszu: ostatnia nuta wygrywa; key-off po key-on w tym samym wierszu NIE kasuje nuty (nuta
// zostaje, a "===" trafia do nastepnego wiersza); key-off + key-on = key-on; glosnosc: ostatnia wygrywa.
// ---------------------------------------------------------------------------------------------
class Recorder {
public:
    Recorder(std::size_t channels, std::uint32_t rowTicks, std::uint64_t maxRows)
        : n_(channels), rowTicks_(rowTicks ? rowTicks : 735u), maxRows_(maxRows ? maxRows : 1u), cur_(channels),
          pendingOff_(channels, 0), playing_(channels, 0)
    {
    }

    // Przesuwa czas; domyka wiersze. false = limit maxRows (dalsze zdarzenia ignorowane).
    bool advanceTo(std::uint64_t tick)
    {
        if (full_) return false;
        const std::uint64_t target = tick / rowTicks_;
        if (target >= maxRows_) {
            advanceRows(maxRows_ - 1);
            full_ = true;
            truncated_ = true;
            return false;
        }
        advanceRows(target);
        return true;
    }

    void noteOn(std::size_t ch, int midi, std::uint8_t instrument, std::uint8_t volume)
    {
        if (full_ || ch >= n_) return;
        Cell& c = cur_[ch];
        c.note = static_cast<std::int16_t>(midi);
        c.instrument = instrument;
        c.volume = volume;
        playing_[ch] = 1;
        pendingOff_[ch] = 0;
    }
    void noteOff(std::size_t ch)
    {
        if (full_ || ch >= n_ || !playing_[ch]) return;
        playing_[ch] = 0;
        if (cur_[ch].note >= 0) pendingOff_[ch] = 1;   // nuta zagrana w tym wierszu zostaje; "===" w nastepnym
        else cur_[ch].note = kNoteOff;
    }
    void volume(std::size_t ch, std::uint8_t v)
    {
        if (full_ || ch >= n_) return;
        cur_[ch].volume = v;
    }

    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] std::uint64_t currentRow() const noexcept { return curRow_; }

    // Domyka zapis: dokladnie `rows` wierszy (obciete do maxRows). Zwraca komorki.
    std::vector<Cell> finish(std::uint64_t rows, std::uint64_t& rowCount)
    {
        if (rows == 0) rows = 1;
        if (rows > maxRows_) { rows = maxRows_; truncated_ = true; }
        if (!full_ && curRow_ < rows - 1) advanceRows(rows - 1);
        if (curRow_ < rows) flushRow();      // wiersz w toku
        while (curRow_ < rows) flushRow();   // (gdyby czas nie doszedl do konca)
        cells_.resize(static_cast<std::size_t>(rows) * n_);
        rowCount = rows;
        return std::move(cells_);
    }

private:
    void advanceRows(std::uint64_t target)
    {
        while (curRow_ < target) flushRow();
    }
    void flushRow()
    {
        cells_.insert(cells_.end(), cur_.begin(), cur_.end());
        ++curRow_;
        std::fill(cur_.begin(), cur_.end(), Cell{});
        for (std::size_t i = 0; i < n_; ++i) {
            if (pendingOff_[i]) { cur_[i].note = kNoteOff; pendingOff_[i] = 0; }
        }
    }

    std::size_t n_;
    std::uint32_t rowTicks_;
    std::uint64_t maxRows_;
    std::vector<Cell> cells_;
    std::vector<Cell> cur_;
    std::vector<std::uint8_t> pendingOff_;
    std::vector<std::uint8_t> playing_;
    std::uint64_t curRow_ = 0;
    bool full_ = false;
    bool truncated_ = false;
};

// Czas + rejestrator wspolne dla dekoderow.
struct Host {
    Host(std::size_t channels, std::uint32_t rowTicks, std::uint64_t maxRows) : rec(channels, rowTicks, maxRows) {}
    Recorder rec;
    std::uint64_t now = 0;   // ticki 44100 Hz
};

// ---------------------------------------------------------------------------------------------
// Interfejs dekodera ukladu. Dekoder trzyma STAN REJESTROW (nie dzwiek) i zglasza zdarzenia do Recordera.
// ---------------------------------------------------------------------------------------------
class ChipDecoder {
public:
    ChipDecoder(Host& host, std::size_t firstChannel) : host_(host), base_(firstChannel) {}
    virtual ~ChipDecoder() = default;
    ChipDecoder(const ChipDecoder&) = delete;
    ChipDecoder& operator=(const ChipDecoder&) = delete;

    // Zapis rejestru: port (OPN2/OPL3: 0/1), rejestr, wartosc. SN76489: tylko val.
    virtual void write(std::uint8_t port, std::uint8_t reg, std::uint8_t val) = 0;
    // Blok danych ROM (0x67, typ >= 0x80) zaadresowany do tego ukladu.
    virtual void romBlock(std::uint8_t /*type*/, std::uint32_t /*memSize*/, std::uint32_t /*startAddr*/,
                          std::span<const std::uint8_t> /*data*/)
    {
    }
    // Strumien/zapis DAC (tylko YM2612): zapis probki; start strumienia (instrument, koniec) i stop.
    virtual void dacData() {}
    virtual void dacStreamStart(std::uint8_t /*instrument*/, std::uint64_t /*endTick*/) {}
    virtual void dacStreamStop() {}
    // Terminy (np. koniec probki ADPCM, cisza DAC): najblizszy tick, w ktorym dekoder chce zadzialac.
    [[nodiscard]] virtual std::uint64_t deadline() const { return kNever; }
    virtual void onDeadline(std::uint64_t /*when*/) {}

protected:
    Host& host_;
    std::size_t base_;
};

// Fabryki dekoderow (po jednej na rodzine ukladow; implementacje w dec_*.cpp).
[[nodiscard]] std::unique_ptr<ChipDecoder> makeSn76489(Host& h, std::size_t first, std::uint32_t clockHz,
                                                       std::uint32_t flags);
[[nodiscard]] std::unique_ptr<ChipDecoder> makeAy8910(Host& h, std::size_t first, std::uint32_t clockHz);
[[nodiscard]] std::unique_ptr<ChipDecoder> makeYm2612(Host& h, std::size_t first, std::uint32_t clockHz);
[[nodiscard]] std::unique_ptr<ChipDecoder> makeYm2203(Host& h, std::size_t first, std::uint32_t clockHz);
[[nodiscard]] std::unique_ptr<ChipDecoder> makeYm2151(Host& h, std::size_t first, std::uint32_t clockHz);
[[nodiscard]] std::unique_ptr<ChipDecoder> makeOkim6295(Host& h, std::size_t first, std::uint32_t clockHz,
                                                        std::uint32_t flags);
[[nodiscard]] std::unique_ptr<ChipDecoder> makeYm2413(Host& h, std::size_t first, std::uint32_t clockHz);
// OPL2 (YM3812), OPL (YM3526) - 9 kanalow + 5 perkusji; OPL3 (YMF262) - 18 + 5.
[[nodiscard]] std::unique_ptr<ChipDecoder> makeOpl(Host& h, std::size_t first, std::uint32_t clockHz, bool opl3);

// ---------------------------------------------------------------------------------------------
// Pomocnicze.
// ---------------------------------------------------------------------------------------------

// Czestotliwosc [Hz] -> MIDI (A4 = 440 Hz = 69, 60 = C-4), zaokraglenie do najblizszego polotonu.
// -1 gdy poza zakresem 0..127 albo hz <= 0.
[[nodiscard]] inline int midiFromHz(double hz) noexcept
{
    if (!(hz > 0.0)) return -1;
    const double m = std::round(69.0 + 12.0 * std::log2(hz / 440.0));
    if (m < 0.0 || m > 127.0) return -1;
    return static_cast<int>(m);
}
// Jak wyzej, ale obcina do 0..127 (uklady FM: dowolna wartosc rejestru daje jakis ton).
[[nodiscard]] inline int midiFromHzClamped(double hz) noexcept
{
    if (!(hz > 0.0)) return 0;
    const double m = std::round(69.0 + 12.0 * std::log2(hz / 440.0));
    return static_cast<int>(std::clamp(m, 0.0, 127.0));
}
// Poziom 0..maxLevel (liniowo w skali rejestru) -> 0x00..0x40.
[[nodiscard]] inline std::uint8_t scaleVolume(int level, int maxLevel) noexcept
{
    if (maxLevel <= 0) return 0;
    level = std::clamp(level, 0, maxLevel);
    return static_cast<std::uint8_t>(std::lround(level * 64.0 / maxLevel));
}

// Stan kanalu sledzony przez dekoder (co ostatnio zgloszono do Recordera).
struct Tracked {
    bool on = false;
    int midi = -1;
    std::uint8_t inst = kValueNone;
    std::uint8_t vol = kValueNone;
};

// Kanaly "bez key-on" (PSG): zdarzenia z roznicy miedzy stanem zadanym a ostatnio zgloszonym.
inline void emitState(Recorder& rec, std::size_t ch, Tracked& cur, bool audible, int midi, std::uint8_t inst,
                      std::uint8_t vol, bool retrigger = false)
{
    if (!audible || midi < 0) {
        if (cur.on) rec.noteOff(ch);
        cur = Tracked{};
        return;
    }
    if (!cur.on || midi != cur.midi || inst != cur.inst || retrigger) {
        rec.noteOn(ch, midi, inst, vol);
        cur = Tracked{true, midi, inst, vol};
    } else if (vol != cur.vol) {
        rec.volume(ch, vol);
        cur.vol = vol;
    }
}

// Numery instrumentow nadawane w kolejnosci pojawienia sie unikatowych zestawow rejestrow (0..254;
// po wyczerpaniu zakresu nowe zestawy dostaja 254). Klucz = surowe bajty.
class InstrumentBank {
public:
    explicit InstrumentBank(std::uint8_t first = 0) : first_(first) {}
    std::uint8_t id(const std::string& key)
    {
        auto it = map_.find(key);
        if (it != map_.end()) return it->second;
        const unsigned next = first_ + static_cast<unsigned>(map_.size());
        const std::uint8_t v = static_cast<std::uint8_t>(std::min(next, 254u));
        map_.emplace(key, v);
        return v;
    }

private:
    std::uint8_t first_;
    std::unordered_map<std::string, std::uint8_t> map_;
};

// Rdzen PSG typu AY-3-8910 (uzywany takze jako SSG w YM2203). clock = zegar wejsciowy rdzenia AY [Hz].
class AyCore {
public:
    AyCore(Host& host, std::size_t firstChannel, double clock) : host_(host), base_(firstChannel), clock_(clock) {}
    void setClock(double clock)
    {
        clock_ = clock;
        for (int c = 0; c < 3; ++c) update(c);
    }
    void write(std::uint8_t reg, std::uint8_t val);

private:
    void update(int c);
    Host& host_;
    std::size_t base_;
    double clock_;
    std::uint8_t r_[16] = {};
    Tracked tr_[3];
};

} // namespace vgmplay::chipstate::detail
