// Kontrolowany most miedzy UI a logika: ekrany czytaja PlayerView (kopia stanu przygotowana
// przez AppController raz na klatke) i wywoluja PlayerActions (zamiary uzytkownika).
// Ekrany nigdy nie widza Engine, AudioOut ani watkow; cala logika jest po stronie src/app.
#pragma once

#include "vgmplay/chipstate.hpp"
#include "vgmplay/core.hpp"
#include "vgmplay/decoder.hpp"
#include "vgmplay/engine.hpp"
#include "vgmplay/loader.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace vgmplay::ui {

class ScopeBuffer;
class ChannelScopeBuffer;
class ChannelMix;

struct AudioDiag {
    std::string device;
    std::string backend;
    std::uint32_t sampleRate = 0;
    double bufferMs = 0.0;
    std::int64_t queuedFrames = 0;   // klatki w PcmRing
    std::uint32_t underruns = 0;     // z PcmRing (callbacki bez danych przy Playing)
};

struct PlayerView {
    engine::TransportSnapshot transport;
    std::uint32_t sampleRate = 48000;   // czestotliwosc silnika (klatki -> sekundy)
    float volume = 1.0f;                // 0..1, ostatnio ustawiona

    bool loading = false;               // plik jest wczytywany (worker) lub przekazany silnikowi
    std::string loadingName;            // nazwa pliku UTF-8

    std::shared_ptr<const loader::LoadedTrack> track; // pusty = nic nie zaladowano
    std::shared_ptr<const TrackInfo> info;            // uklad kanalow (po zdarzeniu TrackLoaded)

    std::optional<Error> error;         // ostatni blad ladowania/odtwarzania
    std::string errorFile;              // nazwa pliku, ktorego dotyczy blad (UTF-8)
    std::string audioError;             // problem z urzadzeniem audio (puste = brak)

    AudioDiag audio;
    const ScopeBuffer* scope = nullptr;
    const ChannelScopeBuffer* channelScopes = nullptr;  // strumienie kanalow (po SetChannelScopes)
    const ChannelMix* mix = nullptr;                    // MUTE/SOLO kanalow biezacego utworu

    // Wzorzec (E1.5): liczony w tle po zaladowaniu i po zmianie rowTicks. Pusty wskaznik = brak
    // (budowany, blad, nic nie zaladowano) - odtwarzanie nie zalezy od wzorca. Wzorzec dotyczy
    // zawsze biezacego utworu; jego rowTicks moze byc przez chwile inny niz patternRowTicks
    // (przeliczanie w tle) - mapowanie pozycji uzywa pattern->rowTicks.
    std::shared_ptr<const chipstate::Pattern> pattern;
    bool patternBuilding = false;
    bool patternHasEffects = false;                     // czy rysowac kolumne efektow
    std::uint32_t patternRowTicks = 2205;                // zadana dlugosc wiersza (ticki VGM, 44100 Hz)

    [[nodiscard]] bool hasTrack() const { return track && info; }
};

class PlayerActions {
public:
    virtual ~PlayerActions() = default;

    virtual void requestOpenFile() = 0;       // okno wyboru pliku
    virtual void togglePlay() = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seekToFrame(std::uint64_t frame) = 0;
    virtual void seekBySeconds(double seconds) = 0;
    virtual void setVolume(float linear) = 0;
    virtual void dismissError() = 0;

    // Widok kanalow (E1.5). Indeks = kanal globalny (ChannelLayout::channels); poza zakresem i w czasie
    // ladowania utworu: ignorowane. Stan MUTE/SOLO jest w PlayerView::mix.
    virtual void toggleChannelMute(int channel) = 0;
    virtual void toggleChannelSolo(int channel) = 0;
    virtual void muteAllChannels() = 0;
    virtual void unmuteAllChannels() = 0;
    virtual void clearSolo() = 0;
    // UI deklaruje, czy widok kanalow (oscyloskopy/VU) jest widoczny; kontroler wysyla komende do
    // silnika tylko przy zmianie.
    virtual void setChannelScopesVisible(bool visible) = 0;
    // Dlugosc wiersza wzorca w tickach VGM (44100 Hz); kontroler przelicza wzorzec w tle.
    virtual void setPatternRowTicks(std::uint32_t ticks) = 0;
};

} // namespace vgmplay::ui
