// vgmplay - kontrakt: engine.hpp
// Silnik odtwarzania: fasada dla UI + petla watku silnika (ADR 0002 pkt 10).
// Naglowek NIE zalezy od libvgm (pimpl); libvgm tylko w src/engine.
//
// MODEL WATKOW
//  * Watek UI: post(), pollEvent(), snapshot(), popScope(), requestStop(). Jeden watek UI
//    (jedyny producent kolejki komend i jedyny konsument kolejki zdarzen/scope).
//  * Watek silnika: run() (blokuje do requestStop()). Jedyny watek dotykajacy IDecoder/libvgm.
//    Miekki RT: render blokami <= kMaxBlockFrames do PcmRing i ScopeRing.
//  * Watek audio (callback miniaudio): tylko kopiuje z PcmRing (modul audio_out, ADR 0003) i
//    aktualizuje TransportAtomics::audibleFrame; nie wola Engine.
//  * Komunikacja: SpscRing komend (UI->silnik), zdarzen (silnik->UI), scope (silnik->UI).
//
// WLASNOSC
//  * Engine posiada kolejki, dekoder i TransportAtomics. PcmRing nalezy do audio_out i musi
//    przezyc Engine (przekazany przez referencje).
//  * LoadedTrack w komendzie Load wspoldzielony przez shared_ptr<const> - UI nie zwalnia go
//    "pod" silnikiem.
//  * Wyjatki (std::bad_alloc) lapane wokol Load/Seek/Render w silniku -> EngineEvent::Error.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

#include "vgmplay/core.hpp"
#include "vgmplay/decoder.hpp"
#include "vgmplay/layout.hpp"     // ChannelLayout, ScopeBlock (re-eksport)
#include "vgmplay/loader.hpp"

namespace vgmplay {
class PcmRing; // audio_out (ADR 0003); definicja poza zamrozonym kontraktem E0
}

namespace vgmplay::engine {

inline constexpr std::size_t kCommandQueueSize = 256;  // potega 2
inline constexpr std::size_t kEventQueueSize = 256;    // potega 2
inline constexpr std::size_t kScopeRingBlocks = 32;    // potega 2

// ---------------- Komendy (UI -> silnik) ----------------
namespace cmd {
struct Load { std::shared_ptr<const loader::LoadedTrack> track; bool autoplay = true; };
struct Play {};
struct Pause {};
struct Stop {};
struct Seek { SampleTime frame = 0; };   // w ramach [0, koniec ostatniej petli)
struct SetLoopCount { std::uint32_t count = 2; };    // 0 = bez konca
struct SetFade { std::uint32_t ms = 1000; };
struct SetVolume { float linear = 1.0f; };           // 0..1, krzywa po stronie silnika
struct SetMute { DeviceId device = 0; MuteMask mask; };
// Solo: silnik trzyma stan i wylicza maski (ADR pkt 4e). channel == kChipSumChannel i
// enabled=false czysci solo na urzadzeniu.
struct SetSolo { DeviceId device = 0; std::uint16_t channel = kChipSumChannel; bool enabled = false; };
struct SetEndBehavior { NonLoopEnd value = NonLoopEnd::Stop; };
} // namespace cmd

using Command = std::variant<cmd::Load, cmd::Play, cmd::Pause, cmd::Stop, cmd::Seek,
                             cmd::SetLoopCount, cmd::SetFade, cmd::SetVolume, cmd::SetMute,
                             cmd::SetSolo, cmd::SetEndBehavior>;

// ---------------- Zdarzenia (silnik -> UI) ----------------
// Tworzone poza sciezka audio (Load/koniec/blad); moga alokowac.
namespace evt {
// Odstepstwo od szkicu: TrackInfo (zawiera ChannelLayout) w shared_ptr, by nie kopiowac
// wektorow przez pierscien.
struct TrackLoaded { std::shared_ptr<const TrackInfo> info; };
struct TrackEnded {};
struct Error { vgmplay::Error error; };
struct Underrun { std::uint32_t count = 0; };    // skumulowane od ostatniego zdarzenia
} // namespace evt

using EngineEvent = std::variant<evt::TrackLoaded, evt::TrackEnded, evt::Error, evt::Underrun>;

// ---------------- Stan transportu ----------------
enum class TransportState : std::uint8_t { Idle = 0, Loading, Stopped, Playing, Paused, Ended, Failed };

// Atomics zapisywane przez silnik/callback audio (relaxed), czytane przez UI.
// Pola sa niezalezne (brak spojnosci miedzy nimi); jedynie `epoch` pozwala odrzucic
// pozycje sprzed seeka/loadu: UI porownuje epoch z epoka swojej komendy.
struct TransportAtomics {
    std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(TransportState::Idle)};
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<std::uint64_t> renderedFrame{0};   // pozycja renderowana
    std::atomic<std::uint64_t> audibleFrame{0};    // pozycja slyszana (po stronie callbacku)
    std::atomic<std::uint64_t> totalFrames{0};     // 0 = nieznana / nieskonczony
    std::atomic<std::uint32_t> loopIndex{0};
    std::atomic<std::uint32_t> underruns{0};
    std::atomic<bool> fadeActive{false};
};

// Zwykla kopia (po jednym odczycie atomowym na pole).
struct TransportSnapshot {
    TransportState state = TransportState::Idle;
    std::uint64_t epoch = 0;
    SampleTime renderedFrame = 0;
    SampleTime audibleFrame = 0;
    SampleTime totalFrames = 0;
    std::uint32_t loopIndex = 0;
    std::uint32_t underruns = 0;
    bool fadeActive = false;
};

struct EngineConfig {
    DecoderConfig decoder;           // wartosci poczatkowe (sampleRate z urzadzenia audio)
};

class Engine {
public:
    Engine(const EngineConfig& config, PcmRing& pcm);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // --- tylko watek UI ---
    // false = kolejka komend pelna (komenda NIE przyjeta; UI moze ponowic w nastepnej klatce).
    [[nodiscard]] bool post(Command command);
    // false = brak zdarzen.
    [[nodiscard]] bool pollEvent(EngineEvent& out);
    [[nodiscard]] TransportSnapshot snapshot() const noexcept;
    // Pobiera kolejny blok oscyloskopu; false = brak. Bloki z nieaktualnym epoch odrzuca UI.
    [[nodiscard]] bool popScope(ScopeBlock& out);

    // --- dowolny watek ---
    void requestStop() noexcept;   // konczy run()

    // --- tylko watek silnika ---
    // Petla: wyczerp komendy -> renderuj blok -> PcmRing/ScopeRing; blokuje do requestStop().
    void run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vgmplay::engine
