// vgmplay - kontrakt: decoder.hpp
// Abstrakcja dekodera audio (ADR 0002 pkt 10). Jedyna implementacja w pierwszym wydaniu:
// VgmDecoder (libvgm, w src/engine). Interfejs umozliwia pozniej S98/NSF/SPC oraz eksport
// WAV (ten sam render() w petli offline). Ten NAGLOWEK NIE zalezy od libvgm - tylko
// implementacje w engine.
//
// WATEK: caly IDecoder (open/start/render/seek/...) wolany WYLACZNIE z watku silnika, nigdy
// rownolegle. libvgm nie jest watkowo-bezpieczny. Zaden wskaznik do danych libvgm nie moze
// wyciec poza dekoder (kopiowac od razu).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "vgmplay/chips.hpp"
#include "vgmplay/core.hpp"
#include "vgmplay/format.hpp"
#include "vgmplay/layout.hpp"

namespace vgmplay {

struct ChipStateSnapshot;   // modul chipstate (E4); tu tylko deklaracja wyprzedzajaca

// Konfiguracja dekodera. Niezmienna po open() (zmiana = close()+open()+seek).
struct DecoderConfig {
    std::uint32_t sampleRate = 48000;       // Hz
    std::uint32_t maxBlockFrames = kMaxBlockFrames;   // <= kMaxBlockFrames
    std::uint32_t loopCount = 2;            // 0 = bez konca
    std::uint32_t fadeMs = 1000;            // wyciszenie po przejsciach petli
    // Utwory BEZ petli (decyzja uzytkownika 2026-11-23): Stop = koniec bez wyciszania.
    NonLoopEnd nonLoopEnd = NonLoopEnd::Stop;
    std::uint32_t endSilenceMs = 500;       // krotka cisza ogonowa przy Stop [wartosc robocza]
    // Wybor rdzeni per uklad+instancja; brak wpisu lub coreId==0 = domyslny rdzen vgmplay.
    std::vector<CoreChoice> cores;
};

// Metadane dostepne po open(), bez start(). Wartosci w klatkach dla DecoderConfig::sampleRate.
struct TrackInfo {
    std::string format;                     // "VGM"; w przyszlosci "S98"...
    std::uint32_t versionBcd = 0;
    std::uint64_t introFrames = 0;
    std::uint64_t loopFrames = 0;           // 0 = brak petli
    std::uint64_t totalFrames = 0;          // z WLASNEGO skanu, nie z naglowka
    format::Gd3Tag tags;                    // zsanityzowany UTF-8
    ChannelLayout layout;
    std::vector<Warning> warnings;
};

struct RenderResult {
    std::uint32_t frames = 0;               // faktycznie zapisane klatki (<= maxFrames); reszte bufora dopelnia WOLAJACY zerami lub dekoder
    bool ended = false;                     // koniec utworu (po fade/ciszy)
};

// Maski wyciszenia kanalow (bit 1 = wyciszony), jak chnMute libvgm: main = uklad glowny,
// linked = uklad polaczony.
struct MuteMask {
    std::uint32_t main = 0;
    std::uint32_t linked = 0;
    bool operator==(const MuteMask&) const = default;
};

struct CoreInfo {
    std::uint32_t coreId = 0;               // token opaque (do CoreChoice)
    std::string name;
    std::string author;
};

class IDecoder {
public:
    virtual ~IDecoder() = default;

    // Wlasciciel `raw`: wolajacy (LoadedTrack::raw()); dekoder kopiuje, co potrzebuje.
    // Dekoder waliduje wejscie takze sam (obrona w glebi) - uzywa format::*.
    // Sukces: TrackInfo + stan "otwarty, jeszcze nie wystartowany".
    [[nodiscard]] virtual Result<TrackInfo> open(std::span<const std::uint8_t> raw,
                                                 const DecoderConfig& cfg) = 0;
    // Tworzy urzadzenia, wstepne bloki danych (dummy-render), ZERUJE maski mute.
    [[nodiscard]] virtual Result<void> start() = 0;
    // Renderuje do maxFrames klatek interleaved L,R (float, +-1.0) do `interleavedLR`
    // (rozmiar >= 2*maxFrames). scope: opcjonalny (nullptr = pomin); dekoder wypelnia
    // masterL/R i frames, epoch/startFrame ustawia silnik. BEZ alokacji na zwyklej sciezce.
    // Nie rzuca; blad krytyczny -> ended=true i silnik zdejmuje zdarzenie z last-error.
    [[nodiscard]] virtual RenderResult render(float* interleavedLR, std::uint32_t maxFrames,
                                              ScopeBlock* scope) = 0;
    [[nodiscard]] virtual Result<void> seek(SampleTime frame) = 0;
    virtual void fadeOut() = 0;
    // chnMute (nie 'disable' libvgm - patrz ADR pkt 4).
    virtual void setMute(DeviceId device, MuteMask mask) = 0;
    // Pozycja RENDEROWANA (nie slyszana).
    [[nodiscard]] virtual SampleTime positionFrames() const = 0;
    [[nodiscard]] virtual const ChipStateSnapshot* chipState() const = 0;  // nullptr = brak
    [[nodiscard]] virtual std::vector<CoreInfo> availableCores(ChipType type) const = 0;
    virtual void close() = 0;
};

} // namespace vgmplay
