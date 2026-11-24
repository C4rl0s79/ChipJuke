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

#include "io_worker.h"
#include "vgmplay/audio_out.hpp"
#include "vgmplay/engine.hpp"
#include "vgmplay/pcm_ring.hpp"
#include "vgmplay_ui/channel_mix.h"
#include "vgmplay_ui/channel_scopes.h"
#include "vgmplay/chipstate.hpp"
#include "vgmplay/library.hpp"
#include "vgmplay/playlist.hpp"
#include "vgmplay/settings.hpp"
#include "vgmplay_ui/track_meta.h"
#include "vgmplay_ui/player_view.h"
#include "vgmplay_ui/scope_buffer.h"

#include <chrono>
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
    std::uint32_t rowTicks = 2205;   // poczatkowa dlugosc wiersza wzorca (ticki VGM 44100 Hz)
    // Plik settings.json (playlists.json i library-cache.json leza w tym samym katalogu). Pusty =
    // brak trwalosci (testy, tryb bez zapisu): nic nie jest czytane ani zapisywane.
    std::filesystem::path settingsFile;
    std::uint32_t saveDelayMs = 1000;   // opoznienie zapisu po ostatniej zmianie (debounce)
    bool allowNetworkPaths = false;     // --allow-network-paths: UNC w playlists.json/settings/cache
    bool noAppDataNotice = false;       // pokaz komunikat "brak AppData, ustawienia nie sa zapisywane"
};

// Prosby o okna systemowe (SDL) - obsluga w main/file_dialog; kontroler tylko prosi.
enum class DialogRequest { AddFiles, AddFolder, ImportM3u, ExportM3u };

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

    void setDialogHandler(std::function<void(DialogRequest)> handler) { dialogHandler_ = std::move(handler); }

    // Dowolny watek. Zleca zaladowanie pliku na watku workera (poza playlista; autoplay z opcji).
    void loadFile(const std::filesystem::path& path);

    // Dowolny watek (callbacki okien SDL, upuszczanie plikow): zlecenia trafiaja do skrzynki i sa
    // wykonywane w pump() na watku UI, gdy playlisty sa wczytane.
    void submitPaths(std::vector<std::filesystem::path> paths);   // pliki i foldery -> aktywna playlista
    void submitFolder(const std::filesystem::path& folder);
    void submitImportM3u(const std::filesystem::path& file);
    void submitExportM3u(const std::filesystem::path& file);
    // Watek UI. Plik z wiersza polecen: dodany do aktywnej playlisty i odtwarzany; sesja nie jest wznawiana.
    void openStartupFile(const std::filesystem::path& file);

    // Watek UI. Stan do odczytu/testow.
    [[nodiscard]] const settings::Settings& settings() const { return settings_; }
    [[nodiscard]] const playlist::PlaylistModel& playlists() const { return model_; }
    [[nodiscard]] playlist::RepeatMode repeat() const { return repeat_; }
    [[nodiscard]] bool playlistsReady() const { return playlistsReady_; }

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
    void playEntry(std::size_t index) override;
    void playNext() override;
    void playPrevious() override;
    void removeEntry(std::size_t index) override;
    void moveEntry(std::size_t from, std::size_t to) override;
    void shufflePlaylist() override;
    void setRepeat(playlist::RepeatMode mode) override;
    void selectPlaylist(std::size_t index) override;
    void createPlaylist(const std::string& name) override;
    void renamePlaylist(std::size_t index, const std::string& name) override;
    void deletePlaylist(std::size_t index) override;
    void requestAddFolder() override;
    void requestImportM3u() override;
    void requestExportM3u() override;
    void cancelScan() override;
    void applySettings(const settings::Settings& s) override;
    void downloadEntry(std::size_t index) override;
    void dismissNotice() override;
    void refreshAudioDevices() override;   // [E3] lista urzadzen odtwarzania (blokuje do ~100 ms)
    void addPaths(std::vector<std::filesystem::path> paths) override { submitPaths(std::move(paths)); }   // [E4]
    void importM3uFile(const std::filesystem::path& file) override { submitImportM3u(file); }
    void exportM3uFile(const std::filesystem::path& file) override { submitExportM3u(file); }

    // Prev w pierwszych kPrevRestartSeconds sekundach -> poprzedni wpis; pozniej poczatek utworu.
    static constexpr double kPrevRestartSeconds = 3.0;

    // Stan komendy SetChannelScopes wyslanej do silnika (dla testow): ostatnia wartosc i liczba wyslan.
    [[nodiscard]] bool channelScopesRequested() const { return scopesRequested_; }
    [[nodiscard]] unsigned channelScopesCommandCount() const { return scopesCommands_; }

private:
    struct Request {
        std::uint64_t id = 0;
        std::filesystem::path path;
        CancelToken cancel;
        bool autoplay = true;
    };
    struct ScanEntry {
        std::unique_ptr<library::ScanJob> job;
        std::filesystem::path root;
        std::string listName;   // playlista docelowa (po nazwie: indeksy moga sie zmienic)
    };
    struct Restore {
        std::filesystem::path path;
        std::uint64_t positionMs = 0;
    };
    struct PatternJob {
        std::uint64_t gen = 0;
        std::shared_ptr<const loader::LoadedTrack> track;
        std::shared_ptr<const TrackInfo> info;   // uklad kanalow (tylko dla --fake-pattern)
        std::uint32_t rowTicks = 2205;
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
        bool autoplay = true;
        std::optional<Result<loader::LoadedTrack>> result;
    };

    void loadFileEx(const std::filesystem::path& path, bool autoplay);
    void workerLoop();

    // --- playlisty, ustawienia, sesja (watek UI) ---
    void loadSettingsAtStart();
    void installDefaultPlaylists();
    void handleIo();
    void handlePlaylistsLoaded(Result<playlist::PlaylistModel>& r);
    void restoreSession();
    void processInbox();
    void processScans();
    void flushPersistence(bool force);
    [[nodiscard]] settings::Settings settingsForSave() const;
    void markSettingsDirty();
    void markPlaylistsDirty();
    void applyRuntimeSettings(const settings::Settings& old, bool initial);
    void switchAudioDevice();                                  // [E3] ponowne otwarcie wyjscia dla settings_.audio.device
    [[nodiscard]] std::optional<playlist::EntryMetaUpdate> entryMetaUpdate(const std::filesystem::path& path) const;
    void syncEntryMeta(const std::filesystem::path& path);     // [E3] tytul/dlugosc do wpisow playlist (updateEntryMeta)
    void loadCursor(bool autoplay);
    void onTrackEnded();
    std::size_t addEntries(std::size_t list, std::vector<playlist::Entry> entries, bool playIfIdle,
                           std::size_t* firstIndex = nullptr);
    std::optional<std::size_t> findList(const std::string& name) const;
    void addAndPlay(const std::filesystem::path& path);
    void addPathsNow(const std::vector<std::filesystem::path>& paths);
    void addFolderNow(const std::filesystem::path& folder);
    void importM3uDone(ImportDone& d);
    void queueMetaFor(const std::vector<playlist::Entry>& entries);
    bool nothingLoaded() const;
    void postNotice(std::string text, std::vector<std::string> details = {}, bool error = false,
                    bool needsAck = false);
    void postInbox(std::function<void()> fn);
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
    audio_out::Config audioCfg_;                       // [E3] ostatnia konfiguracja otwarcia
    std::vector<ui::AudioDeviceItem> audioDevices_;
    bool audioDevicesLoaded_ = false;
    bool audioFellBack_ = false;
    bool audioNoOutput_ = false;                                // [E3-sec] brak dzialajacego wyjscia; ponawiane
    std::chrono::steady_clock::time_point audioRetryAt_{};
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
    std::uint32_t rowTicks_ = 2205;
    bool seekInFlight_ = false;
    std::uint64_t seekTarget_ = 0;
    std::uint64_t seekEpoch_ = 0;
    ui::PlayerView view_;

    // Playlisty / ustawienia / sesja (watek UI, o ile nie zaznaczono inaczej).
    settings::Settings settings_;
    playlist::PlaylistModel model_;
    playlist::RepeatMode repeat_ = playlist::RepeatMode::Off;
    ui::TrackMetaStore metaStore_;
    std::unique_ptr<IoWorker> io_;
    bool persist_ = false;               // settingsFile podany
    bool playlistsReady_ = false;
    bool settingsBlocked_ = false;       // uszkodzony settings.json: brak zapisu do potwierdzenia
    bool playlistsBlocked_ = false;      // uszkodzony playlists.json: brak zapisu do potwierdzenia
    bool settingsDirty_ = false, playlistsDirty_ = false;
    std::chrono::steady_clock::time_point settingsDirtyAt_, playlistsDirtyAt_, lastSessionTick_;
    std::optional<std::filesystem::path> startupFile_;
    std::optional<Restore> restore_;     // przewiniecie po TrackLoaded
    bool restoreActive_ = false;         // sesja jest wznawiana (nie nadpisuj session)
    std::vector<ScanEntry> scans_;
    std::optional<ui::Notice> notice_;
    std::chrono::steady_clock::time_point noticeUntil_;
    std::uint64_t noticeSerial_ = 0;
    std::function<void(DialogRequest)> dialogHandler_;
    std::mutex inboxMutex_;
    std::vector<std::function<void()>> inbox_;   // chroniony inboxMutex_
};

} // namespace vgmplay::app
