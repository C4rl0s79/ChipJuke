// AppController: jedyny wlasciciel Engine, AudioOut i PcmRing oraz watkow (silnika i workera
// ladowania). Ekrany UI widza tylko PlayerView (stan) i PlayerActions (zamiary).
//
// Dlaczego w src/app, a nie w src/ui: UI ma zostac bez zaleznosci od silnika/audio/loadera
// (testowalne i wymienne); kontroler to "klej" aplikacji i zalezy od wszystkich modulow.
//
// WATKI
//  * Watek UI: wszystkie metody PlayerActions, pump(), view(). Jedyny watek wolajacy Engine::post/
//    pollEvent/snapshot/popScope (kontrakt SPSC).
//  * Watek silnika: Engine::run() (std::thread, startuje w start()).
//  * Watek workera: loader::loadTrackFromFile (blokujace I/O); wynik trafia do kolejki pod
//    mutexem, odbieranej w pump() na watku UI; dopiero tam powstaje cmd::Load.
//  * loadFile() jest bezpieczne z dowolnego watku (np. callback okna wyboru pliku SDL).
//  * Wzorzec (chipstate::buildPattern) liczy ten sam watek workera, tuz po zaladowaniu pliku i po
//    kazdej zmianie rowTicks (zadanie "latest wins" po numerze generacji); wynik trafia do kolejki
//    pod mutexem i jest odbierany w pump(). Blad lub pusty wzorzec nie blokuja odtwarzania.
//  * Wiele zadan ladowania: wygrywa ostatnie ("latest wins"); poprzednie sa anulowane
//    (CancelToken), a ich wyniki odrzucane po id.
#pragma once

#include "vgmplay/audio_out.hpp"
#include "vgmplay/engine.hpp"
#include "vgmplay/pcm_ring.hpp"
#include "vgmplay_ui/channel_mix.h"
#include "vgmplay_ui/channel_scopes.h"
#include "vgmplay/chipstate.hpp"
#include "vgmplay_ui/player_view.h"
#include "vgmplay_ui/scope_buffer.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace vgmplay::app {

struct ControllerOptions {
    bool nullAudio = false;    // backend "null" miniaudio (bez dzwieku, z prawdziwym zegarem)
    bool autoplay = true;      // po zaladowaniu pliku od razu graj
    // DIAGNOSTYKA: gdy silnik nie dostarcza strumieni kanalow (extraStreamCount == 0), UI dopisuje
    // do bloku scope syntetyczne sinusy na kanal (do testow i zrzutow ekranu). Dziala tylko po
    // setChannelScopesVisible(true), tak jak silnik po cmd::SetChannelScopes.
    bool fakeChannelScopes = false;
    // DIAGNOSTYKA: zamiast chipstate::buildPattern - sztuczny wzorzec z nutami dla kanalow ukladu.
    bool fakePattern = false;
    std::uint32_t rowTicks = 735;   // poczatkowa dlugosc wiersza wzorca (ticki VGM 44100 Hz)
};

class AppController final : public ui::PlayerActions {
public:
    explicit AppController(ControllerOptions options = {});
    ~AppController() override;
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // Otwiera audio (z awaryjnym przejsciem na "null") i uruchamia watek silnika oraz workera.
    // Zawsze zwraca true (kontroler dziala nawet bez urzadzenia); blad audio trafia do view().
    bool start();

    // Callbacki ustawiac przed start(). wake: dowolny watek (budzi petle UI po zakonczeniu ladowania).
    void setWakeCallback(std::function<void()> wake) { wake_ = std::move(wake); }
    // Wywolywany na watku UI po requestOpenFile().
    void setOpenDialogHandler(std::function<void()> handler) { openDialog_ = std::move(handler); }

    // Dowolny watek. Zleca zaladowanie pliku na watku workera.
    void loadFile(const std::filesystem::path& path);

    // Watek UI, raz na klatke: wyniki workera, komendy do silnika, zdarzenia, scope, snapshot.
    void pump();
    [[nodiscard]] const ui::PlayerView& view() const { return view_; }

    // --- ui::PlayerActions (watek UI) ---
    void requestOpenFile() override;
    void togglePlay() override;
    void play() override;
    void pause() override;
    void stop() override;
    void seekToFrame(std::uint64_t frame) override;
    void seekBySeconds(double seconds) override;
    void setVolume(float linear) override;
    void dismissError() override;
    void toggleChannelMute(int channel) override;
    void toggleChannelSolo(int channel) override;
    void muteAllChannels() override;
    void unmuteAllChannels() override;
    void clearSolo() override;
    void setChannelScopesVisible(bool visible) override;
    void setPatternRowTicks(std::uint32_t ticks) override;

    // Stan komendy SetChannelScopes wyslanej do silnika (dla testow): ostatnia wartosc i liczba wyslan.
    [[nodiscard]] bool channelScopesRequested() const { return scopesRequested_; }
    [[nodiscard]] unsigned channelScopesCommandCount() const { return scopesCommands_; }

private:
    struct Request {
        std::uint64_t id = 0;
        std::filesystem::path path;
        CancelToken cancel;
    };
    struct PatternJob {
        std::uint64_t gen = 0;
        std::shared_ptr<const loader::LoadedTrack> track;
        std::shared_ptr<const TrackInfo> info;   // uklad kanalow (tylko dla --fake-pattern)
        std::uint32_t rowTicks = 735;
        std::uint32_t sampleRate = 48000;
        bool fake = false;
    };
    struct PatternDone {
        std::uint64_t gen = 0;
        std::shared_ptr<const loader::LoadedTrack> track;
        std::shared_ptr<const chipstate::Pattern> pattern;   // pusty = blad
        std::uint32_t requestedTicks = 0;
        bool hasEffects = false;
    };
    struct Done {
        std::uint64_t id = 0;
        std::filesystem::path path;
        std::optional<Result<loader::LoadedTrack>> result;
    };

    void workerLoop();
    static PatternDone buildPatternJob(const PatternJob& job);
    void requestPattern(const std::shared_ptr<const loader::LoadedTrack>& track,
                        const std::shared_ptr<const TrackInfo>& info);
    void send(engine::Command c);
    bool canEditMix() const { return info_ && inflight_.empty(); }
    void sendMute(DeviceId device);
    void sendChannelScopes(bool enabled);
    void flushOutbox();
    void handleDone(Done& d);
    void handleEvent(const engine::EngineEvent& e);
    std::uint64_t seekLimit() const;

    ControllerOptions options_;
    std::function<void()> wake_;
    std::function<void()> openDialog_;

    // Kolejnosc deklaracji = odwrotna kolejnosc niszczenia: Engine przed AudioOut przed PcmRing.
    PcmRing pcm_;
    audio_out::AudioOut audio_{pcm_};
    std::unique_ptr<engine::Engine> engine_;
    std::thread engineThread_;
    bool started_ = false;

    // Worker ladowania.
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Request> pending_;      // chroniony mutex_
    std::optional<PatternJob> patternJob_; // chroniony mutex_
    std::vector<PatternDone> patternDone_; // chroniony mutex_
    std::vector<Done> done_;              // chroniony mutex_
    CancelToken currentCancel_;           // chroniony mutex_
    std::uint64_t latestId_ = 0;          // chroniony mutex_
    std::uint64_t handledId_ = 0;         // chroniony mutex_: ostatni id, ktorego wynik odebral UI
    std::filesystem::path latestPath_;    // chroniony mutex_
    bool quit_ = false;                   // chroniony mutex_

    // Stan watku UI.
    std::deque<engine::Command> outbox_;  // komendy czekajace na miejsce w kolejce silnika
    // Utwory przekazane silnikowi (cmd::Load), czekajace na TrackLoaded albo Error - w tej kolejnosci.
    std::deque<std::shared_ptr<const loader::LoadedTrack>> inflight_;
    std::shared_ptr<const loader::LoadedTrack> track_;
    std::shared_ptr<const TrackInfo> info_;
    std::optional<Error> error_;
    std::string errorFile_;
    std::string audioError_;
    float volume_ = 0.8f;
    std::unique_ptr<ui::ScopeBuffer> scope_ = std::make_unique<ui::ScopeBuffer>();
    std::unique_ptr<ScopeBlock> scopeBlock_ = std::make_unique<ScopeBlock>();
    ui::ChannelMix mix_;
    ui::ChannelScopeBuffer channelScopes_;
    bool scopesRequested_ = false;   // ostatnia wartosc zadana przez UI (wyslana do silnika)
    unsigned scopesCommands_ = 0;
    // Wzorzec (watek UI): ostatnia przyjeta generacja, utwor, do ktorego nalezy, i zadane rowTicks.
    std::uint64_t patternGen_ = 0;         // ostatnio zlecona generacja
    std::uint64_t patternGenDone_ = 0;     // ostatnio odebrana generacja
    std::shared_ptr<const chipstate::Pattern> pattern_;
    std::shared_ptr<const loader::LoadedTrack> patternTrack_;
    bool patternHasEffects_ = false;
    std::uint32_t rowTicks_ = 735;
    bool seekInFlight_ = false;
    std::uint64_t seekTarget_ = 0;
    std::uint64_t seekEpoch_ = 0;
    ui::PlayerView view_;
};

} // namespace vgmplay::app
