// vgmplay - kontrakt: layout.hpp
// Opis urzadzen/kanalow utworu oraz ScopeBlock (blok danych oscyloskopu). Wydzielone z
// engine.hpp, zeby decoder.hpp (nizsza warstwa) mogl z nich korzystac bez cyklu zaleznosci;
// engine.hpp re-eksportuje ten naglowek. Bez zaleznosci od libvgm.
#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "vgmplay/chips.hpp"

namespace vgmplay {

// Indeks urzadzenia w ChannelLayout::devices. Wazny tylko w obrebie jednego utworu/epoki.
using DeviceId = std::uint16_t;

// channelIdx == kChipSumChannel oznacza "suma ukladu" (wyjscie calego urzadzenia).
inline constexpr std::uint16_t kChipSumChannel = 0xFFFF;

struct DeviceDesc {
    DeviceId id = 0;                          // == indeks w ChannelLayout::devices
    ChipType type = ChipType::Unknown;
    std::uint8_t instance = 0;                // 0 / 1
    std::string name;                         // nazwa wyswietlana, np. "YM2612"
    std::uint16_t channelCount = 0;           // wg deklaracji rdzenia (np. YM2612: 7)
    std::vector<std::string> channelNames;    // size == channelCount
    std::uint32_t coreId = 0;                 // faktycznie uzyty rdzen (token opaque)
    bool hasLinked = false;                   // uklad polaczony (SSG w OPN, FM w OPL4)
    // Podzial kanalow (dodane w E1): indeksy [0, mainChannelCount) naleza do ukladu glownego
    // (MuteMask::main, bit = indeks), reszta [mainChannelCount, channelCount) do polaczonego
    // (MuteMask::linked, bit = indeks - mainChannelCount). Bez tego silnik nie policzy masek solo.
    std::uint16_t mainChannelCount = 0;
};

struct ChannelRef {
    DeviceId deviceIdx = 0;
    std::uint16_t channelIdx = 0;
    bool operator==(const ChannelRef&) const = default;
};

struct ChannelLayout {
    std::vector<DeviceDesc> devices;
    // Kanaly "plaskie" w stalej kolejnosci (globalChannelId = indeks w tym wektorze).
    std::vector<ChannelRef> channels;
};

// --- ScopeBlock ---
inline constexpr std::uint32_t kMaxBlockFrames = 512;  // maksymalny blok renderu
// E1.5: podniesione z 16 do 32 (YM2608 = 16 kanalow, dwa takie uklady = 32; wiecej kanalow maja m.in.
// C352, YMF278B, ES5503 - patrz "wybor strumieni" nizej). Rozmiar ScopeBlock: 2 x 2 KiB (master) +
// 32 x 2 KiB (extra) ~ 68 KiB; pierscien kScopeRingBlocks (32) ~ 2,1 MiB na stercie, kopia bloku
// przy push/pop ~ 68 KiB x ~94 bloki/s ~ 6 MB/s - pomijalne. Semantyka pol bez zmian.
inline constexpr std::uint32_t kMaxExtraStreams = 32;  // strumienie per kanal (E1.5) / per uklad

inline constexpr std::uint8_t kScopeStreamAvailable = 0x01; // brak flagi = strumien niedostepny

struct ScopeStreamDesc {
    std::uint16_t deviceIdx = 0;
    std::uint16_t channelIdx = kChipSumChannel;   // kChipSumChannel = suma ukladu
    std::uint8_t flags = 0;                       // kScopeStreamAvailable
};

// Jedna porcja do pierscienia oscyloskopu. POD (trywialnie kopiowalny), stala wielkosc,
// bez alokacji. Producent: watek silnika; konsument: watek UI (SpscRing).
// Dane [frames, kMaxBlockFrames) sa nieokreslone.
//
// STRUMIENIE extra (E1.5, cmd::SetChannelScopes{true}; bez wlaczenia extraStreamCount = 0):
//  * extra[i][0..frames) - MONO kanalu opisanego przez extraDesc[i]: (L + R) / 2 wyjscia
//    osobnej kopii emulacji z maska MUTE "wszystko poza tym kanalem" (ADR 0002 pkt 5 (c)). Ta sama
//    skala i ta sama czestotliwosc probkowania co masterL/R (BEZ decymacji), przed glosnoscia
//    i mikro-fade'em. Dla ukladow liniowych sum_i extra[i] ~ (masterL + masterR) / 2.
//  * Strumien jest w fazie z masterem (ta sama klatka = ta sama probka utworu, +-1 klatka), a kopie
//    NIE zaleza od MUTE/SOLO na glownym miksie (wyciszony kanal nadal ma przebieg - jak FT2).
//  * Zestaw extraDesc jest staly dla utworu i ustawien (Load/reconfigure ustawia go na nowo; kolejnosc
//    = kolejnosc globalna ChannelLayout::channels). Wpisy bez kScopeStreamAvailable maja extra[i] == 0.
//  * Po Load / Seek / Stop / wlaczeniu kopie dochodza do pozycji glownego dekodera stopniowo (ok.
//    kilka-kilkadziesiat ms, jedna kopia na iteracje petli silnika); do tego czasu extra[i] = 0
//    (cisza) przy ustawionej fladze. Pierwsze bloki nowej epoki moga wiec miec ciche strumienie.
//  * WYBOR STRUMIENI: najpierw kanaly rozdzielalne (maska MUTE dziala w rdzeniu), w kolejnosci
//    globalnej, do kMaxExtraStreams albo limitu pamieci kopii; potem (jesli zostalo miejsce) kanaly
//    nierozdzielalne z flags == 0. Kanal nieobecny w extraDesc = brak strumienia (UI: "brak").
struct ScopeBlock {
    std::uint64_t epoch = 0;        // generacja (zwiekszana przy Load/Seek); stare bloki odrzuca konsument
    std::uint64_t startFrame = 0;   // pozycja pierwszej klatki w utworze (SampleTime)
    std::uint32_t frames = 0;       // <= kMaxBlockFrames
    std::uint8_t extraStreamCount = 0; // <= kMaxExtraStreams
    ScopeStreamDesc extraDesc[kMaxExtraStreams]{};
    float masterL[kMaxBlockFrames]{};
    float masterR[kMaxBlockFrames]{};
    float extra[kMaxExtraStreams][kMaxBlockFrames]{};
};
static_assert(std::is_trivially_copyable_v<ScopeBlock>);

} // namespace vgmplay
