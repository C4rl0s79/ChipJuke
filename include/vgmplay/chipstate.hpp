// vgmplay - kontrakt: chipstate.hpp (E1.5, decyzja uzytkownika 2026-11-24)
// Wzorzec w stylu trackera (nuty, instrumenty, glosnosci) odczytany z ZAPISOW REJESTROW w
// strumieniu komend VGM - bez libvgm i bez odtwarzania. Liczony raz po zaladowaniu utworu
// (watek workera), potem UI tylko przewija go razem z pozycja sluchana.
//
// Kanaly: (chip, instance, channelIdx) w TEJ SAMEJ kolejnosci co ChannelLayout silnika
// (DeviceDesc: kanaly ukladu glownego [0, mainChannelCount), potem polaczonego), zeby UI
// moglo laczyc kolumny wzorca z oscyloskopami i MUTE/SOLO po (type, instance, channelIdx).
// Kanaly bez dekodera (uklad nieobslugiwany) maja decoded=false i puste komorki.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "vgmplay/chips.hpp"
#include "vgmplay/core.hpp"
#include "vgmplay/format.hpp"

namespace vgmplay::chipstate {

inline constexpr std::int16_t kNoteNone = -1;     // pusta komorka
inline constexpr std::int16_t kNoteOff = -2;      // wylaczenie nuty (key-off), FT2: "==="
inline constexpr std::uint8_t kValueNone = 0xFF;  // brak instrumentu / glosnosci w komorce

// Jedna komorka wzorca (kanal x wiersz). note: numer MIDI 0..127, kNoteNone, kNoteOff.
// Notacja w UI: oktawa = note/12 - 1 (60 = "C-4"), kNoteOff = "===", instrument i volume
// jako 2 cyfry hex. effect: 0 = brak, 1..35 = znak 1-9, A-Z jak w FT2 (wiecej nie wyswietlane).
// Komorka niesie zdarzenia, ktore
// zaszly w czasie wiersza; zmiana glosnosci bez nowej nuty = tylko volume.
struct Cell {
    std::int16_t note = kNoteNone;
    std::uint8_t instrument = kValueNone;  // numer "instrumentu" (np. patch FM, typ szumu) 0..254
    std::uint8_t volume = kValueNone;      // 0x00..0x40 jak w FT2 (0x40 = maks.), kValueNone = brak zmiany
    std::uint8_t effect = 0;               // 0 = brak; kody efektow: wolne do uzycia przez dekoder (opisz)
    std::uint8_t effectParam = 0;
};

struct PatternChannel {
    ChipType type = ChipType::Unknown;
    std::uint8_t instance = 0;
    std::uint16_t channelIdx = 0;      // indeks jak w DeviceDesc (main, potem linked)
    std::string name;                  // np. "FM 1", "PSG 2", "Noise", "ADPCM 3"
    bool decoded = false;              // false = brak dekodera dla tego ukladu/kanalu
};

struct Pattern {
    std::uint32_t rowTicks = 2205;     // dlugosc wiersza w tickach VGM (44100 Hz); 2205 = 1/20 s (decyzja uzytkownika 2026-11-24)
    std::uint64_t rowCount = 0;        // wiersze od 0 do konca danych (jedno przejscie)
    std::uint64_t loopStartRow = 0;    // wiersz poczatku petli (gdy header.hasLoop())
    std::vector<PatternChannel> channels;
    std::vector<Cell> cells;           // row-major: cells[row * channels.size() + ch]
    std::vector<Warning> warnings;     // np. obciecie do maxRows
    [[nodiscard]] const Cell& at(std::uint64_t row, std::size_t ch) const
    {
        return cells[static_cast<std::size_t>(row) * channels.size() + ch];
    }
};

struct PatternOptions {
    std::uint32_t rowTicks = 2205;         // 1/20 s domyslnie; UI moze zmienic (np. 735 = 1/60 s)
    std::uint64_t maxRows = 1'000'000;     // limit pamieci (obciecie + ostrzezenie)
};

// Buduje wzorzec z ZWALIDOWANEGO bufora (LoadedTrack::raw(); format::validateCommandStream juz
// przeszlo). Funkcja czysta, bez alokacji ponad wynik, O(rozmiar). Watek: dowolny (worker).
// Bledy: tylko LimitExceeded / OutOfMemory / Internal; nieobslugiwane uklady to decoded=false.
[[nodiscard]] Result<Pattern> buildPattern(std::span<const std::uint8_t> raw,
                                           const format::HeaderInfo& header,
                                           const format::StreamReport& stream,
                                           const PatternOptions& options = {});

} // namespace vgmplay::chipstate
