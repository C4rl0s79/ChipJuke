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
inline constexpr std::uint32_t kMaxExtraStreams = 16;  // rezerwa na strumienie per uklad / kanal

inline constexpr std::uint8_t kScopeStreamAvailable = 0x01; // brak flagi = strumien niedostepny

struct ScopeStreamDesc {
    std::uint16_t deviceIdx = 0;
    std::uint16_t channelIdx = kChipSumChannel;   // kChipSumChannel = suma ukladu
    std::uint8_t flags = 0;                       // kScopeStreamAvailable
};

// Jedna porcja do pierscienia oscyloskopu. POD (trywialnie kopiowalny), stala wielkosc,
// bez alokacji. Producent: watek silnika; konsument: watek UI (SpscRing).
// Pierwsze wydanie: tylko masterL/masterR, extraStreamCount = 0. Strumienie extra (per uklad
// - wariant (b), per kanal - wariant (a)/(c) wg ADR 0002 pkt 5) wypelniane pozniej BEZ zmiany
// ukladu struktury. Dane [frames, kMaxBlockFrames) sa nieokreslone.
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
