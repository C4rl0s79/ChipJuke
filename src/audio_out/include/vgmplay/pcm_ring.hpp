// vgmplay - audio_out: PcmRing
// Pierscien blokow PCM (float, interleaved L,R) miedzy watkiem silnika (producent) a callbackiem
// audio (konsument). Definicja klasy zapowiedzianej w include/vgmplay/engine.hpp.
//
// WATKI
//  * Producent: dokladnie jeden watek (silnika): acquireWrite()/publish()/beginEpoch()/setActive().
//  * Konsument: dokladnie jeden watek (callback audio): consume(). consume() NIE alokuje, nie
//    blokuje, nie loguje i nie robi wywolan systemowych (tylko atomiki i petla po probkach).
//  * Pozostale metody atomowe (volume, audible, underruns, queuedFrames) - dowolny watek.
//
// EPOKA (ADR 0002, "Seek i PcmRing")
//  Kazdy blok niesie `epoch`. beginEpoch(e) (Load/Seek/Stop) publikuje nowa epoke; callback
//  odrzuca bloki innej epoki bez kopiowania, a juz grajaca tresc wycisza krotkim (~2 ms)
//  zaniknieciem ostatniej probki (bez uzycia nieaktualnych danych). Po ciszy/underrunie tresc
//  wchodzi z takim samym narastaniem.
//
// POZYCJA SLYSZANA
//  consume() po kazdym wywolaniu publikuje (epoka, pozycja nastepnej klatki do odtworzenia)
//  w jednym atomiku (epoka w 24 gornych bitach, pozycja w 40 dolnych: 2^40 klatek = lata).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "vgmplay/layout.hpp"   // kMaxBlockFrames

namespace vgmplay {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // padding przez alignas - zamierzony (false sharing)
#endif
class PcmRing {
public:
    static constexpr std::size_t kBlocks = 16;             // potega 2; 16 x 512 klatek ~ 170 ms @ 48 kHz
    static constexpr std::uint32_t kDeclickFrames = 96;    // ~2 ms @ 48 kHz

    struct Block {
        std::uint64_t epoch = 0;
        std::uint64_t startFrame = 0;                      // pozycja pierwszej klatki w utworze
        std::uint32_t frames = 0;                          // 1..kMaxBlockFrames
        float samples[kMaxBlockFrames * 2] = {};           // L,R,L,R...
    };

    PcmRing();
    PcmRing(const PcmRing&) = delete;
    PcmRing& operator=(const PcmRing&) = delete;

    // ---- producent (watek silnika) ----
    // Wolny blok do zapisu albo nullptr (pierscien pelny). Wypelnij epoch/startFrame/frames/samples
    // i wywolaj publish(). Do publish() wolno trzymac wskaznik.
    [[nodiscard]] Block* acquireWrite() noexcept;
    void publish() noexcept;
    // Nowa epoka: bloki starszych epok zostana odrzucone przez konsumenta.
    void beginEpoch(std::uint64_t epoch) noexcept;
    // true = silnik oczekuje danych (Playing); pusty pierscien liczy sie wtedy jako underrun.
    void setActive(bool active) noexcept { active_.store(active, std::memory_order_relaxed); }

    // ---- konsument (callback audio) ----
    // Wypelnia `frames` klatek (interleaved L,R) w `out`. Zawsze zapisuje cale wyjscie.
    void consume(float* out, std::uint32_t frames) noexcept;

    // ---- dowolny watek ----
    // Wzmocnienie liniowe 0..1 stosowane w callbacku (z lagodnym przejsciem w obrebie bloku).
    void setVolume(float gain) noexcept { volume_.store(gain, std::memory_order_relaxed); }
    [[nodiscard]] float volume() const noexcept { return volume_.load(std::memory_order_relaxed); }
    // Klatki w pierscieniu (jeszcze nie zuzyte); przyblizenie z perspektywy obcego watku.
    [[nodiscard]] std::int64_t queuedFrames() const noexcept { return queued_.load(std::memory_order_relaxed); }
    // Liczba callbackow, w ktorych zabraklo danych przy active == true.
    [[nodiscard]] std::uint32_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    struct Audible { std::uint64_t epoch24 = 0; std::uint64_t frame = 0; bool valid = false; };
    [[nodiscard]] Audible audible() const noexcept;
    // Liczba klatek zuzytych (odtworzonych lub odrzuconych) przez konsumenta - do testow/diagnostyki.
    [[nodiscard]] std::uint64_t consumedFrames() const noexcept { return consumed_.load(std::memory_order_relaxed); }

    static constexpr std::uint64_t kAudibleFrameBits = 40;

private:
    static constexpr std::uint64_t kMask = kBlocks - 1;

    std::unique_ptr<Block[]> blocks_;
    alignas(64) std::atomic<std::uint64_t> write_{0};      // pisze producent
    alignas(64) std::atomic<std::uint64_t> read_{0};       // pisze konsument
    alignas(64) std::atomic<std::uint64_t> epoch_{0};
    std::atomic<bool> active_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<std::int64_t> queued_{0};
    std::atomic<std::uint32_t> underruns_{0};
    std::atomic<std::uint64_t> audible_{0};                // 0 = brak danych
    std::atomic<std::uint64_t> consumed_{0};

    // Stan wylacznie konsumenta.
    std::uint32_t cursor_ = 0;          // klatki zuzyte w biezacym bloku
    std::uint32_t tail_ = 0;            // pozostale klatki zaniku
    std::uint32_t fadeIn_ = 0;          // pozostale klatki narastania
    bool hadData_ = false;
    float lastL_ = 0.0f, lastR_ = 0.0f;
    float curVol_ = 1.0f;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace vgmplay
