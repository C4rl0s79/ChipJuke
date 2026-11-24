#include "app_controller.h"

#include "vgmplay_ui/format.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace vgmplay::app {
namespace {

std::string fileNameUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.filename().u8string();
    return std::string(u8.begin(), u8.end());
}

} // namespace

AppController::AppController(ControllerOptions options) : options_(options) {}

AppController::~AppController()
{
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

    audio_out::Config cfg;
    cfg.backend = options_.nullAudio ? audio_out::Config::Backend::Null : audio_out::Config::Backend::Default;
    auto opened = audio_.open(cfg);
    if (!opened.ok() && !options_.nullAudio) {
        audioError_ = opened.error().message;
        cfg.backend = audio_out::Config::Backend::Null; // UI i silnik dzialaja dalej, bez dzwieku
        opened = audio_.open(cfg);
    }
    const std::uint32_t rate = opened.ok() && opened.value().sampleRate ? opened.value().sampleRate : 48000;

    engine::EngineConfig ec;
    ec.decoder.sampleRate = rate;
    engine_ = std::make_unique<engine::Engine>(ec, pcm_);
    engineThread_ = std::thread([this] { engine_->run(); });
    view_.sampleRate = rate;
    send(engine::cmd::SetVolume{volume_});

    if (opened.ok()) {
        auto s = audio_.start();
        if (!s.ok()) audioError_ = s.error().message;
    } else if (audioError_.empty()) {
        audioError_ = opened.error().message;
    }

    worker_ = std::thread([this] { workerLoop(); });
    pump();
    return true;
}

// ------------------------------------------------------------------ worker ladowania

void AppController::loadFile(const std::filesystem::path& path)
{
    {
        std::lock_guard lock(mutex_);
        currentCancel_.requestCancel(); // poprzednie zadanie (jesli trwa) nie jest juz potrzebne
        currentCancel_ = CancelToken();
        const std::uint64_t id = ++latestId_;
        latestPath_ = path;
        pending_ = Request{id, path, currentCancel_};
    }
    cv_.notify_one();
    if (wake_) wake_();
}

void AppController::workerLoop()
{
    for (;;) {
        Request req;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || pending_.has_value(); });
            if (quit_) return;
            req = std::move(*pending_);
            pending_.reset();
        }
        Done done;
        done.id = req.id;
        done.path = req.path;
        try {
            done.result.emplace(loader::loadTrackFromFile(req.path, {}, req.cancel));
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
        return; // poprzedni utwor gra dalej
    }
    error_.reset();
    errorFile_.clear();
    auto track = std::make_shared<const loader::LoadedTrack>(std::move(d.result->value()));
    inflight_.push_back(track);
    send(engine::cmd::Load{track, options_.autoplay});
}

void AppController::handleEvent(const engine::EngineEvent& e)
{
    if (const auto* loaded = std::get_if<engine::evt::TrackLoaded>(&e)) {
        if (!inflight_.empty()) {
            track_ = std::move(inflight_.front());
            inflight_.pop_front();
        }
        info_ = loaded->info;
        error_.reset();
        errorFile_.clear();
    } else if (const auto* err = std::get_if<engine::evt::Error>(&e)) {
        error_ = err->error;
        if (!inflight_.empty()) { // blad ladowania: silnik zamknal poprzedni utwor
            errorFile_ = fileNameUtf8(inflight_.front()->path);
            inflight_.pop_front();
        } else if (track_) {
            errorFile_ = fileNameUtf8(track_->path);
        }
        track_.reset();
        info_.reset();
    }
    // TrackEnded: stan widoczny w snapshot (Ended); Underrun: licznik w snapshot/PcmRing.
}

void AppController::pump()
{
    if (!engine_) return;

    std::vector<Done> done;
    std::uint64_t latest = 0, handled = 0;
    std::filesystem::path latestPath;
    {
        std::lock_guard lock(mutex_);
        done.swap(done_);
    }
    for (Done& d : done) handleDone(d);
    flushOutbox();

    engine::EngineEvent ev;
    while (engine_->pollEvent(ev)) handleEvent(ev);

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

    for (int guard = 0; guard < 256 && engine_->popScope(*scopeBlock_); ++guard) scope_->push(*scopeBlock_);

    view_.volume = volume_;
    view_.loading = (latest != handled) || !inflight_.empty();
    view_.loadingName = view_.loading ? fileNameUtf8(latestPath) : std::string();
    view_.track = track_;
    view_.info = info_;
    view_.error = error_;
    view_.errorFile = errorFile_;
    view_.audioError = audioError_;
    view_.scope = scope_.get();
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
    send(engine::cmd::SetVolume{volume_});
}

void AppController::dismissError()
{
    error_.reset();
    errorFile_.clear();
    audioError_.clear();
}

} // namespace vgmplay::app
