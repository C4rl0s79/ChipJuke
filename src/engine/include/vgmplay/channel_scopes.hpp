// vgmplay - engine: ChannelScopeBank = dzwiek osobno dla kazdego kanalu (E1.5, ADR 0002 pkt 5 (c)).
//
// Dla kazdego rozdzielalnego kanalu ChannelLayout trzyma OSOBNA instancje dekodera (kopia emulacji):
// ten sam bufor utworu, ta sama konfiguracja rdzeni/petli/wyciszenia, maska MUTE = wszystko poza tym
// kanalem, a pozostale urzadzenia "zamrozone" (setDeviceFrozen - bez kosztu emulacji). Kopie sa
// deterministyczne wzgledem glownego dekodera (ten sam kod libvgm, te same zapisy), wiec po zsynchronizowaniu
// pozycji ich wyjscie jest w fazie z masterem. Glowny miks NIE pochodzi z kopii.
//
// Czestotliwosc: kopie renderuja z TA SAMA czestotliwoscia co master (bez decymacji) - uzasadnienie
// i pomiary w ADR 0002 pkt 5. Wynik: extra[i] = (L + R) / 2 kopii.
//
// WATEK: wszystko wylacznie z watku silnika (jak IDecoder). Zadnej synchronizacji wewnatrz.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "vgmplay/decoder.hpp"
#include "vgmplay/loader.hpp"

namespace vgmplay::engine {

class ChannelScopeBank {
public:
    // Budzet pamieci na wszystkie kopie; kopia kosztuje ~2 x rozmiar rozpakowanego utworu (bufor
    // pliku + dane ROM/PCM w rdzeniach) + ~2 MiB na stan emulacji. Ponad budzet: mniej strumieni.
    static constexpr std::uint64_t kMemoryBudgetBytes = 384ull << 20;
    static constexpr std::uint64_t kPerCopyOverheadBytes = 2ull << 20;
    // Tolerancja przesuniecia kopii wzgledem glownego dekodera po seeku (zaokraglenia tick<->probka).
    static constexpr std::int64_t kMaxSkewFrames = 8;
    // Ile rozjazdow (od ostatniego seeka) toleruje kopia, zanim zostanie uznana za niedostepna.
    static constexpr std::uint32_t kMaxMismatches = 4;

    ChannelScopeBank() = default;
    ChannelScopeBank(const ChannelScopeBank&) = delete;
    ChannelScopeBank& operator=(const ChannelScopeBank&) = delete;

    // Niszczy wszystkie kopie i plan (zero kosztu CPU po tym wywolaniu).
    void reset() noexcept;

    // Wyznacza plan strumieni dla utworu (po open+start glownego dekodera `main`); kopie tworzone sa
    // dopiero w syncStep(). `cfg` = ta sama konfiguracja co glowny dekoder.
    void bind(std::shared_ptr<const loader::LoadedTrack> track, const DecoderConfig& cfg,
              const TrackInfo& info, const IDecoder& main);

    // Pozycja glownego dekodera sie zmienila skokiem (seek/stop/restart): kopie wymagaja synchronizacji.
    void invalidate() noexcept;

    [[nodiscard]] bool needsSync() const noexcept;

    // Synchronizuje (tworzy / przewija do `pos`) kolejne kopie, az skonczy sie `budget` (co najmniej
    // jedna kopia na wywolanie). Zwraca liczbe zsynchronizowanych kopii w tym wywolaniu.
    std::size_t syncStep(SampleTime pos, std::chrono::microseconds budget);

    // Renderuje `frames` klatek we WSZYSTKICH zsynchronizowanych kopiach (ZAWSZE, takze bez `sb`,
    // zeby kopie szly w kroku z glownym dekoderem) i - gdy sb != nullptr - ustawia sb->extra*.
    // `start` = pozycja pierwszej klatki bloku w utworze (renderPos sprzed renderu).
    void render(std::uint32_t frames, SampleTime start, ScopeBlock* sb) noexcept;

    // Diagnostyka / testy.
    [[nodiscard]] bool active() const noexcept { return !streams_.empty(); }
    [[nodiscard]] std::size_t streamCount() const noexcept { return streams_.size(); }
    [[nodiscard]] std::size_t copyCount() const noexcept;      // strumienie z kopia (flags & Available)
    [[nodiscard]] std::size_t syncedCount() const noexcept;
    [[nodiscard]] const ScopeStreamDesc& desc(std::size_t i) const { return streams_[i].desc; }

private:
    struct Stream {
        ScopeStreamDesc desc;
        std::unique_ptr<IDecoder> dec;      // nullptr = brak kopii (nierozdzielalny lub jeszcze nie utworzona)
        MuteMask mask;                      // maska izolujaca kanal na jego urzadzeniu
        bool wanted = false;                // ma miec kopie
        bool synced = false;
        std::int64_t skew = 0;              // pozycja kopii - pozycja glownego dekodera
        std::uint32_t mismatches = 0;
    };

    bool syncOne(Stream& s, SampleTime pos);

    std::shared_ptr<const loader::LoadedTrack> track_;
    DecoderConfig cfg_;
    std::size_t deviceCount_ = 0;
    std::vector<Stream> streams_;
    std::vector<float> scratch_;            // interleaved L,R na blok
    std::vector<DeviceDesc> devices_;       // kopia layoutu (do maski i zamrazania)
};

} // namespace vgmplay::engine
