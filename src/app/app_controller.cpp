#include "app_controller.h"

#include "vgmplay_ui/strings.h"

#include "vgmplay_ui/format.h"
#include "vgmplay_ui/pattern_view.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace vgmplay::app {
namespace {

std::string fileNameUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.filename().u8string();
    return std::string(u8.begin(), u8.end());
}

// Syntetyczne strumienie kanalow (--fake-channel-scopes): sinus/trojkat/prostokat o czestotliwosci
// zaleznej od kanalu, z wolna zmienna obwiednia (zeby VU sie ruszalo). Ciagle w funkcji numeru klatki.
// Kanal nr 5 (przy >= 8 kanalach) jest oznaczony jako niedostepny, by pokazac "n/a".
void fillFakeChannels(ScopeBlock& b, const TrackInfo& info, std::uint32_t sampleRate)
{
    constexpr double kTwoPi = 6.283185307179586;
    const std::size_t nch = info.layout.channels.size();
    const std::size_t n = std::min<std::size_t>(nch, kMaxExtraStreams);
    b.extraStreamCount = static_cast<std::uint8_t>(n);
    for (std::size_t i = 0; i < n; ++i) {
        b.extraDesc[i].deviceIdx = info.layout.channels[i].deviceIdx;
        b.extraDesc[i].channelIdx = info.layout.channels[i].channelIdx;
        const bool na = i == 5 && nch >= 8;
        b.extraDesc[i].flags = na ? 0 : kScopeStreamAvailable;
        const double freq = 110.0 * std::pow(2.0, static_cast<double>(i) / 5.0);
        for (std::uint32_t f = 0; f < b.frames; ++f) {
            const double t = static_cast<double>(b.startFrame + f) / sampleRate;
            const double ph = std::fmod(freq * t, 1.0);
            double w;
            switch (i % 3) {
            case 0: w = std::sin(kTwoPi * ph); break;
            case 1: w = 4.0 * std::fabs(ph - 0.5) - 1.0; break;
            default: w = ph < 0.5 ? 0.8 : -0.8; break;
            }
            const double env = 0.5 + 0.5 * std::sin(kTwoPi * (0.35 + 0.05 * static_cast<double>(i)) * t + static_cast<double>(i));
            b.extra[i][f] = static_cast<float>(0.25 * env * env * w);
        }
    }
}

} // namespace

AppController::AppController(ControllerOptions options) : options_(options)
{
    rowTicks_ = std::clamp<std::uint32_t>(options.rowTicks, 64u, ui::kVgmTickRate);
    view_.patternRowTicks = rowTicks_;
}

AppController::~AppController()
{
    scans_.clear();   // ScanJob: anuluje i dolacza watek
    flushPersistence(true);
    io_.reset();      // dopisuje zalegle zapisy i dolacza watek
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        currentCancel_.requestCancel();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (engine_) engine_->requestStop();
    if (engineThread_.joinable()) engineThread_.join();
    audio_.stop();
    audio_.close();
}

bool AppController::start()
{
    if (started_) return true;
    started_ = true;

    loadSettingsAtStart();

    audio_out::Config cfg;
    cfg.backend = options_.nullAudio ? audio_out::Config::Backend::Null : audio_out::Config::Backend::Default;
    cfg.deviceId = settings_.audio.device;   // [E3] puste = domyslne; nieznane id -> domyslne + komunikat
    auto opened = audio_.open(cfg);
    if (!opened.ok() && !options_.nullAudio) {
        audioError_ = opened.error().message;
        cfg.backend = audio_out::Config::Backend::Null; // UI i silnik dzialaja dalej, bez dzwieku
        cfg.deviceId.clear();
        opened = audio_.open(cfg);
    }
    audioCfg_ = cfg;
    if (opened.ok() && opened.value().fellBackToDefault) {
        audioFellBack_ = true;
        postNotice(ui::tr(ui::Str::SetAudioFallback), {settings_.audio.device});
    }
    const std::uint32_t rate = opened.ok() && opened.value().sampleRate ? opened.value().sampleRate : 48000;

    engine::EngineConfig ec;
    ec.decoder.sampleRate = rate;
    engine_ = std::make_unique<engine::Engine>(ec, pcm_);
    engineThread_ = std::thread([this] { engine_->run(); });
    view_.sampleRate = rate;
    applyRuntimeSettings(settings::Settings{}, true);

    if (opened.ok()) {
        auto s = audio_.start();
        if (!s.ok()) audioError_ = s.error().message;
    } else if (audioError_.empty()) {
        audioError_ = opened.error().message;
    }

    worker_ = std::thread([this] { workerLoop(); });

    IoWorker::Paths paths;
    if (persist_) {
        const std::filesystem::path dir = options_.settingsFile.parent_path();
        paths.settings = options_.settingsFile;
        paths.playlists = dir / L"playlists.json";
        paths.cache = dir / L"library-cache.json";
    }
    paths.allowNetworkPaths = options_.allowNetworkPaths || settings_.library.allowNetworkPaths;   // [E3] efektywnie
    io_ = std::make_unique<IoWorker>(paths, [this] { if (wake_) wake_(); });
    if (persist_) io_->loadPlaylists();
    else installDefaultPlaylists();
    pump();
    return true;
}

// ------------------------------------------------------------------ worker ladowania

void AppController::loadFile(const std::filesystem::path& path) { loadFileEx(path, options_.autoplay); }

void AppController::loadFileEx(const std::filesystem::path& path, bool autoplay)
{
    {
        std::lock_guard lock(mutex_);
        currentCancel_.requestCancel(); // poprzednie zadanie (jesli trwa) nie jest juz potrzebne
        currentCancel_ = CancelToken();
        const std::uint64_t id = ++latestId_;
        latestPath_ = path;
        pending_ = Request{id, path, currentCancel_, autoplay};
    }
    cv_.notify_one();
    if (wake_) wake_();
}

void AppController::workerLoop()
{
    for (;;) {
        std::optional<Request> req;
        std::optional<PatternJob> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || pending_.has_value() || patternJob_.has_value(); });
            if (quit_) return;
            if (pending_) { // ladowanie ma pierwszenstwo przed przeliczaniem wzorca
                req = std::move(pending_);
                pending_.reset();
            } else {
                job = std::move(patternJob_);
                patternJob_.reset();
            }
        }
        if (job) {
            PatternDone pd = buildPatternJob(*job);
            {
                std::lock_guard lock(mutex_);
                patternDone_.push_back(std::move(pd));
            }
            if (wake_) wake_();
            continue;
        }
        Done done;
        done.id = req->id;
        done.path = req->path;
        done.autoplay = req->autoplay;
        try {
            done.result.emplace(loader::loadTrackFromFile(req->path, {}, req->cancel));
        } catch (const std::exception& e) {
            done.result.emplace(makeError(ErrorCode::Internal, e.what()));
        } catch (...) {
            done.result.emplace(makeError(ErrorCode::Internal, "unknown exception in loader"));
        }
        {
            std::lock_guard lock(mutex_);
            done_.push_back(std::move(done));
        }
        if (wake_) wake_();
    }
}

// Watek workera. Funkcja czysta wzgledem stanu kontrolera; nie rzuca (wyjatek -> brak wzorca).
AppController::PatternDone AppController::buildPatternJob(const PatternJob& job)
{
    PatternDone out;
    out.gen = job.gen;
    out.track = job.track;
    out.requestedTicks = job.rowTicks;
    try {
        if (job.fake) {
            if (job.info) {
                const TrackInfo& ti = *job.info;
                std::uint64_t frames = ti.introFrames + ti.loopFrames;
                if (frames == 0) frames = ti.totalFrames;
                out.pattern = ui::makeFakePattern(ti.layout, ui::framesToTicks(frames, job.sampleRate),
                                                  ui::framesToTicks(ti.introFrames, job.sampleRate),
                                                  ti.loopFrames > 0, job.rowTicks);
            }
        } else if (job.track) {
            chipstate::PatternOptions opt;
            opt.rowTicks = job.rowTicks;
            auto r = chipstate::buildPattern(job.track->raw(), job.track->header, job.track->stream, opt);
            if (r.ok()) out.pattern = std::make_shared<const chipstate::Pattern>(std::move(r.value()));
        }
        if (out.pattern) out.hasEffects = ui::patternHasEffects(*out.pattern);
    } catch (...) {
        out.pattern.reset();   // blad wzorca nie blokuje odtwarzania
    }
    return out;
}

// Watek UI: zlecenie przeliczenia wzorca (nowsze zadanie zastepuje poprzednie).
void AppController::requestPattern(const std::shared_ptr<const loader::LoadedTrack>& track,
                                   const std::shared_ptr<const TrackInfo>& info)
{
    if (!track) return;
    PatternJob j;
    j.gen = ++patternGen_;
    j.track = track;
    j.info = info;
    j.rowTicks = rowTicks_;
    j.sampleRate = view_.sampleRate;
    j.fake = options_.fakePattern;
    {
        std::lock_guard lock(mutex_);
        patternJob_ = std::move(j);
    }
    cv_.notify_one();
}

// ------------------------------------------------------------------ watek UI

void AppController::send(engine::Command c)
{
    if (!engine_) return;
    if (outbox_.empty() && engine_->post(c)) return;
    outbox_.push_back(std::move(c));
}

void AppController::flushOutbox()
{
    while (!outbox_.empty() && engine_->post(outbox_.front())) outbox_.pop_front();
}

void AppController::handleDone(Done& d)
{
    {
        std::lock_guard lock(mutex_);
        if (d.id != latestId_) return; // wynik przestarzaly (wygralo pozniejsze zadanie)
        handledId_ = d.id;
    }
    if (!d.result->ok()) {
        if (d.result->error().code == ErrorCode::Cancelled) return;
        error_ = d.result->error();
        errorFile_ = fileNameUtf8(d.path);
        restore_.reset();
        restoreActive_ = false;
        return; // poprzedni utwor gra dalej
    }
    error_.reset();
    errorFile_.clear();
    auto track = std::make_shared<const loader::LoadedTrack>(std::move(d.result->value()));
    inflight_.push_back(track);
    send(engine::cmd::Load{track, d.autoplay});
    if (!options_.fakePattern) requestPattern(track, nullptr);   // worker liczy wzorzec od razu po loaderze
}

void AppController::handleEvent(const engine::EngineEvent& e)
{
    if (const auto* loaded = std::get_if<engine::evt::TrackLoaded>(&e)) {
        if (!inflight_.empty()) {
            track_ = std::move(inflight_.front());
            inflight_.pop_front();
        }
        info_ = loaded->info;
        if (track_) { // tytul GD3 do listy (zanim worker metadanych zdazy); dlugosc zostaje z workera
            const ui::TrackDisplay* old = metaStore_.find(track_->path);
            ui::TrackDisplay d = old ? *old : ui::TrackDisplay{};
            if (track_->gd3.present) {
                d.trackEn = track_->gd3.track(format::Gd3Language::En);
                d.trackJp = track_->gd3.track(format::Gd3Language::Jp);
                d.failed = false;
                metaStore_.put(track_->path, std::move(d));
                syncEntryMeta(track_->path);   // [E3] tytul GD3 trafia tez do wpisow playlist
            }
        }
        if (restore_ && track_) { // wznowienie sesji: przewin tylko dla tej samej sciezki
            if (restore_->positionMs > 0 && ui::pathKey(restore_->path) == ui::pathKey(track_->path)) {
                const std::uint64_t frame = restore_->positionMs * view_.sampleRate / 1000;
                const std::uint64_t limit = info_->introFrames + info_->loopFrames;
                if (limit > 0 && frame < limit) send(engine::cmd::Seek{frame});
            }
            restore_.reset();
        }
        restoreActive_ = false;
        lastSessionTick_ = std::chrono::steady_clock::now();
        markSettingsDirty();   // session.trackPath
        if (options_.fakePattern) requestPattern(track_, info_);   // sztuczny wzorzec potrzebuje ukladu kanalow
        mix_.setLayout(info_->layout); // silnik zeruje MUTE/SOLO przy Load
        if (scopesRequested_) sendChannelScopes(true); // stan silnika po Load nie jest gwarantowany
        error_.reset();
        errorFile_.clear();
    } else if (std::holds_alternative<engine::evt::TrackEnded>(e)) {
        onTrackEnded();
    } else if (const auto* err = std::get_if<engine::evt::Error>(&e)) {
        restore_.reset();
        restoreActive_ = false;
        error_ = err->error;
        if (!inflight_.empty()) { // blad ladowania: silnik zamknal poprzedni utwor
            errorFile_ = fileNameUtf8(inflight_.front()->path);
            inflight_.pop_front();
        } else if (track_) {
            errorFile_ = fileNameUtf8(track_->path);
        }
        track_.reset();
        info_.reset();
        mix_.clear();
        patternGenDone_ = ++patternGen_;   // spozniony wynik wzorca jest przestarzaly
        pattern_.reset();
        patternTrack_.reset();
    }
    // TrackEnded: stan widoczny w snapshot (Ended); Underrun: licznik w snapshot/PcmRing.
}

void AppController::pump()
{
    if (!engine_) return;

    std::vector<Done> done;
    std::vector<PatternDone> patterns;
    std::uint64_t latest = 0, handled = 0;
    std::filesystem::path latestPath;
    {
        std::lock_guard lock(mutex_);
        done.swap(done_);
        patterns.swap(patternDone_);
    }
    handleIo();
    processInbox();
    processScans();
    for (Done& d : done) handleDone(d);
    for (PatternDone& pd : patterns) {
        if (pd.gen != patternGen_) continue;   // przestarzaly (nowszy utwor lub rowTicks)
        patternGenDone_ = pd.gen;
        pattern_ = pd.pattern;
        patternTrack_ = pd.track;
        patternHasEffects_ = pd.hasEffects;
        if (pd.requestedTicks != rowTicks_) { // rowTicks zmienione w czasie liczenia
            const bool sameTrack = pd.track == track_;
            if (!options_.fakePattern || sameTrack) requestPattern(pd.track, sameTrack ? info_ : nullptr);
        }
    }
    flushOutbox();

    engine::EngineEvent ev;
    while (engine_->pollEvent(ev)) handleEvent(ev);

    if (audioNoOutput_ && std::chrono::steady_clock::now() >= audioRetryAt_) switchAudioDevice();   // [E3-sec]
    const audio_out::DeviceEvents de = audio_.pollEvents();
    if (de.lost) {
        const auto r = audio_.reopen();
        if (!r.ok()) audioError_ = r.error().message;
        else if (r.value().sampleRate != view_.sampleRate)
            audioError_ = "audio device changed sample rate; restart the application";
    }

    {
        std::lock_guard lock(mutex_);
        latest = latestId_;
        handled = handledId_;
        latestPath = latestPath_;
    }

    view_.transport = engine_->snapshot();
    if (seekInFlight_ && view_.transport.epoch != seekEpoch_) seekInFlight_ = false;

    for (int guard = 0; guard < 256 && engine_->popScope(*scopeBlock_); ++guard) {
        if (options_.fakeChannelScopes && scopesRequested_ && info_ && scopeBlock_->extraStreamCount == 0)
            fillFakeChannels(*scopeBlock_, *info_, view_.sampleRate);
        scope_->push(*scopeBlock_);
        channelScopes_.push(*scopeBlock_);
    }

    flushPersistence(false);
    if (notice_ && !notice_->needsAck && std::chrono::steady_clock::now() >= noticeUntil_ && notice_->details.empty())
        notice_.reset();
    view_.playlists = &model_;
    view_.meta = &metaStore_;
    view_.settings = &settings_;
    view_.allowNetworkPaths = options_.allowNetworkPaths || settings_.library.allowNetworkPaths;
    view_.networkForcedByFlag = options_.allowNetworkPaths;
    view_.audioDevices = audioDevices_;
    view_.audioDevicesLoaded = audioDevicesLoaded_;
    view_.audioFellBackToDefault = audioFellBack_;
    view_.audioNoOutput = audioNoOutput_;
    view_.repeat = repeat_;
    view_.playlistsLoading = persist_ && !playlistsReady_;
    view_.scan = {};
    for (const ScanEntry& se : scans_) {
        const library::ScanProgress pr = se.job->progress();
        view_.scan.active = true;
        view_.scan.files += pr.filesFound;
        view_.scan.dirs += pr.directoriesVisited;
    }
    view_.notice = notice_;

    view_.volume = volume_;
    view_.loading = (latest != handled) || !inflight_.empty();
    view_.loadingName = view_.loading ? fileNameUtf8(latestPath) : std::string();
    view_.track = track_;
    view_.info = info_;
    view_.error = error_;
    view_.errorFile = errorFile_;
    view_.audioError = audioError_;
    view_.scope = scope_.get();
    view_.channelScopes = &channelScopes_;
    view_.mix = &mix_;
    view_.pattern = (patternTrack_ && patternTrack_ == track_) ? pattern_ : nullptr;
    view_.patternHasEffects = view_.pattern && patternHasEffects_;
    view_.patternBuilding = patternGen_ != patternGenDone_;
    view_.patternRowTicks = rowTicks_;
    if (audio_.isOpen()) {
        const audio_out::DeviceInfo& di = audio_.info();
        view_.audio.device = di.name;
        view_.audio.backend = di.backend;
        view_.audio.sampleRate = di.sampleRate;
        view_.audio.bufferMs = di.bufferMs;
    }
    view_.audio.queuedFrames = pcm_.queuedFrames();
    view_.audio.underruns = pcm_.underruns();
}

std::uint64_t AppController::seekLimit() const
{
    if (view_.transport.totalFrames > 0) return view_.transport.totalFrames;
    return info_ ? info_->introFrames + info_->loopFrames : 0;
}

// ------------------------------------------------------------------ PlayerActions

void AppController::requestOpenFile()
{
    if (openDialog_) openDialog_();
}

void AppController::togglePlay()
{
    using engine::TransportState;
    switch (view_.transport.state) {
    case TransportState::Playing: pause(); break;
    case TransportState::Stopped:
    case TransportState::Paused:
    case TransportState::Ended: play(); break;
    default: break;
    }
}

void AppController::play()
{
    if (view_.hasTrack()) send(engine::cmd::Play{});
}

void AppController::pause()
{
    if (view_.hasTrack()) send(engine::cmd::Pause{});
}

void AppController::stop()
{
    if (view_.hasTrack()) send(engine::cmd::Stop{});
}

void AppController::seekToFrame(std::uint64_t frame)
{
    const std::uint64_t limit = seekLimit();
    if (!view_.hasTrack() || limit == 0) return;
    frame = std::min(frame, limit - 1);
    send(engine::cmd::Seek{frame});
    seekInFlight_ = true;
    seekTarget_ = frame;
    seekEpoch_ = view_.transport.epoch;
}

void AppController::seekBySeconds(double seconds)
{
    const std::uint64_t limit = seekLimit();
    if (!view_.hasTrack() || limit == 0) return;
    // Kolejne seeki (auto-repeat) liczymy od ostatniego celu, dopoki silnik go nie zastosowal.
    const std::uint64_t base = seekInFlight_ ? seekTarget_ : view_.transport.audibleFrame;
    seekToFrame(ui::offsetFrame(base, seconds, view_.sampleRate, limit));
}

void AppController::setVolume(float linear)
{
    volume_ = std::clamp(linear, 0.0f, 1.0f);
    view_.volume = volume_;
    if (settings_.playback.volume != volume_) {
        settings_.playback.volume = volume_;
        markSettingsDirty();
    }
    send(engine::cmd::SetVolume{volume_});
}

void AppController::dismissError()
{
    error_.reset();
    errorFile_.clear();
    audioError_.clear();
}

// ------------------------------------------------------------------ widok kanalow

void AppController::sendMute(DeviceId device)
{
    send(engine::cmd::SetMute{device, mix_.muteMask(device)});
}

void AppController::sendChannelScopes(bool enabled)
{
    send(engine::cmd::SetChannelScopes{enabled});
    ++scopesCommands_;
}

void AppController::setChannelScopesVisible(bool visible)
{
    if (visible == scopesRequested_) return;
    scopesRequested_ = visible;
    sendChannelScopes(visible);
}

void AppController::setPatternRowTicks(std::uint32_t ticks)
{
    ticks = std::clamp<std::uint32_t>(ticks, 64u, ui::kVgmTickRate);
    if (ticks == rowTicks_) return;
    rowTicks_ = ticks;
    view_.patternRowTicks = ticks;
    if (settings_.ui.rowTicks != ticks) {
        settings_.ui.rowTicks = ticks;
        markSettingsDirty();
    }
    if (track_ && patternTrack_ == track_) requestPattern(track_, info_);   // przeliczenie w tle
    // Wzorzec utworu jeszcze ladowanego dostanie nowe rowTicks po odebraniu wyniku (pump).
}

void AppController::toggleChannelMute(int channel)
{
    if (!canEditMix() || !mix_.toggleMute(channel)) return;
    sendMute(mix_.ref(channel).deviceIdx);
}

void AppController::toggleChannelSolo(int channel)
{
    if (!canEditMix() || !mix_.toggleSolo(channel)) return;
    const ChannelRef r = mix_.ref(channel);
    send(engine::cmd::SetSolo{r.deviceIdx, r.channelIdx, mix_.solo(channel)});
}

void AppController::muteAllChannels()
{
    if (!canEditMix()) return;
    mix_.muteAll();
    for (int d = 0; d < mix_.deviceCount(); ++d) sendMute(static_cast<DeviceId>(d));
}

void AppController::unmuteAllChannels()
{
    if (!canEditMix()) return;
    mix_.unmuteAll();
    for (int d = 0; d < mix_.deviceCount(); ++d) sendMute(static_cast<DeviceId>(d));
}

void AppController::clearSolo()
{
    if (!canEditMix()) return;
    mix_.clearSolo();
    for (int d = 0; d < mix_.deviceCount(); ++d)
        send(engine::cmd::SetSolo{static_cast<DeviceId>(d), kChipSumChannel, false});
}

} // namespace vgmplay::app
