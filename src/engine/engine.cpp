// vgmplay - engine: fasada dla UI + petla watku silnika. Patrz include/vgmplay/engine.hpp.
// WATEK: Impl::* (poza post/pollEvent/snapshot/popScope/requestStop) wylacznie z watku silnika.
#include "vgmplay/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <new>
#include <string>
#include <vector>

#include <windows.h>

#include "vgmplay/channel_scopes.hpp"
#include "vgmplay/pcm_ring.hpp"
#include "vgmplay/timeline.hpp"
#include "vgmplay/vgm_decoder.hpp"

namespace vgmplay::engine {
namespace {

constexpr std::uint32_t kFadeMs = 5;            // mikro-fade po Load/Seek/Play i przed Pause
constexpr std::uint32_t kTargetBufferMs = 60;   // ile audio trzymamy w PcmRing
constexpr std::uint32_t kMaxBlocksPerPass = 8;  // tyle blokow renderu miedzy sprawdzeniami komend
constexpr std::uint32_t kLoopWaitMs = 2;
// Budzet czasu na synchronizacje kopii kanalowych w jednej iteracji petli (tworzenie/przewiniecie).
constexpr std::chrono::microseconds kScopeSyncBudget{3000};
// Po Load/Seek/Stop/reconfigure kopie synchronizujemy od razu (zanim ruszy audio / zanim pompa wyrenderuje
// pierwszy blok), zeby byly w fazie z masterem (ta sama sciezka "seek" w glownym dekoderze i kopiach).
// Gdy nie zmiesci sie w tym budzecie, reszta dochodzi stopniowo w petli.
constexpr std::chrono::microseconds kScopeSyncNowBudget{100000};

template <class... Fs> struct Overloaded : Fs... { using Fs::operator()...; };
template <class... Fs> Overloaded(Fs...) -> Overloaded<Fs...>;

// Precyzyjne oczekiwanie (~1-2 ms) niezaleznie od globalnej rozdzielczosci zegara systemu.
class PreciseSleeper {
public:
    PreciseSleeper()
    {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
    }
    ~PreciseSleeper() { if (timer_) CloseHandle(timer_); }
    PreciseSleeper(const PreciseSleeper&) = delete;
    PreciseSleeper& operator=(const PreciseSleeper&) = delete;

    void sleepMs(std::uint32_t ms) const
    {
        if (!timer_) { Sleep(ms); return; }
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(ms) * 10000;
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer_, ms + 50);
        else
            Sleep(ms);
    }

private:
    HANDLE timer_ = nullptr;
};

[[nodiscard]] float volumeCurve(float linear) noexcept
{
    const float x = std::clamp(linear, 0.0f, 1.0f);
    return x * x;       // krzywa kwadratowa: percepcyjnie rowniejszy suwak niz liniowy
}

[[nodiscard]] constexpr std::uint32_t fullMask(std::uint16_t channels) noexcept
{
    return channels >= 32 ? 0xFFFFFFFFu : ((1u << channels) - 1u);
}

} // namespace

struct Engine::Impl {
    Impl(const EngineConfig& c, PcmRing& p) : cfg(c), pcm(p), scopeBlock(std::make_unique<ScopeBlock>())
    {
        cfg.decoder.maxBlockFrames = std::clamp<std::uint32_t>(cfg.decoder.maxBlockFrames, 64, kMaxBlockFrames);
    }

    EngineConfig cfg;
    PcmRing& pcm;
    SpscRing<Command, kCommandQueueSize> cmds;
    SpscRing<EngineEvent, kEventQueueSize> events;
    SpscRing<ScopeBlock, kScopeRingBlocks> scopeRing;
    TransportAtomics tr;
    std::atomic<bool> stopFlag{false};

    // ---- stan watku silnika ----
    std::unique_ptr<IDecoder> dec;
    std::shared_ptr<const loader::LoadedTrack> track;
    std::shared_ptr<const TrackInfo> info;
    PlaybackTimeline tl;
    std::unique_ptr<ScopeBlock> scopeBlock;
    TransportState state = TransportState::Idle;
    std::uint64_t epoch = 0;
    SampleTime renderPos = 0;
    std::uint32_t fadeInTotal = 0;
    std::uint32_t fadeInLeft = 0;
    std::uint32_t lastUnderruns = 0;
    std::vector<MuteMask> userMute, solo, applied;
    ChannelScopeBank scopes;        // E1.5: kopie kanalowe (puste, gdy wylaczone)
    bool scopesEnabled = false;

    // ---------------------------------------------------------------- zdarzenia / stan
    void postEvent(EngineEvent&& e) { (void)events.tryPush(std::move(e)); }

    void postError(ErrorCode code, std::string msg)
    {
        postEvent(evt::Error{makeError(code, std::move(msg))});
    }

    void setState(TransportState s)
    {
        state = s;
        tr.state.store(static_cast<std::uint8_t>(s), std::memory_order_relaxed);
    }

    void publishPosition()
    {
        tr.renderedFrame.store(renderPos, std::memory_order_relaxed);
    }

    // Nowa epoka: odrzuca zawartosc PcmRing i oznacza poczatek nowej sekwencji pozycji.
    void newEpoch(SampleTime startPos)
    {
        ++epoch;
        tr.epoch.store(epoch, std::memory_order_release);
        tr.audibleFrame.store(startPos, std::memory_order_relaxed);   // pozycja do czasu pierwszego bloku
        pcm.beginEpoch(epoch);
        fadeInTotal = fadeInLeft = std::max<std::uint32_t>(1, cfg.decoder.sampleRate * kFadeMs / 1000);
    }

    [[nodiscard]] SampleTime audiblePosition() const
    {
        const PcmRing::Audible a = pcm.audible();
        if (a.valid && a.epoch24 == (epoch & 0xFFFFFFull)) return a.frame;
        return tr.audibleFrame.load(std::memory_order_relaxed);
    }

    void closeTrack()
    {
        scopes.reset();
        if (dec) dec->close();
        dec.reset();
        info.reset();
        track.reset();
        tl = PlaybackTimeline{};
        userMute.clear();
        solo.clear();
        applied.clear();
    }

    // ---------------------------------------------------------------- ladowanie
    // Konfiguracja dekodera dla biezacego utworu: ROM-y szukane tez w katalogu utworu (po katalogach z cfg).
    [[nodiscard]] DecoderConfig trackDecoderConfig() const
    {
        DecoderConfig c = cfg.decoder;
        if (track && !track->path.empty()) c.romDirs.push_back(track->path.parent_path());
        return c;
    }

    // Otwiera dekoder dla `track`, ustawia os czasu i (opcjonalnie) przewija do startPos.
    // Zwraca false i ustawia Failed, gdy sie nie udalo (blad juz zgloszony).
    bool openTrack(SampleTime startPos, bool announce)
    {
        dec = makeVgmDecoder();
        const auto raw = track->raw();
        auto opened = dec->open(raw, trackDecoderConfig());
        if (!opened.ok()) { fail(opened.error()); return false; }
        auto started = dec->start();
        if (!started.ok()) { fail(started.error()); return false; }

        TrackInfo ti = std::move(opened).value();
        // Dane z wlasnego skanu i tagi z LoadedTrack nadpisuja przyblizenia dekodera.
        TimelineInput in;
        in.totalTicks = track->stream.totalSamples ? track->stream.totalSamples : track->header.totalSamples;
        in.hasLoop = track->header.hasLoop();
        in.loopStartTick = track->stream.loopStartSample;
        in.loopCount = modifiedLoopCount(cfg.decoder.loopCount, track->header.loopBase,
                                         track->header.loopModifier);
        in.sampleRate = cfg.decoder.sampleRate;
        in.fadeMs = cfg.decoder.fadeMs;
        in.nonLoopEnd = cfg.decoder.nonLoopEnd;
        in.endSilenceMs = cfg.decoder.endSilenceMs;
        tl = computeTimeline(in);

        ti.introFrames = tl.introFrames;
        ti.loopFrames = tl.loopFrames;
        ti.totalFrames = tl.onePassFrames;
        ti.tags = track->gd3;
        if (track->header.versionBcd) ti.versionBcd = track->header.versionBcd;
        std::vector<Warning> w = track->warnings;
        w.insert(w.end(), std::make_move_iterator(ti.warnings.begin()),
                 std::make_move_iterator(ti.warnings.end()));
        ti.warnings = std::move(w);

        const std::size_t devCount = ti.layout.devices.size();
        userMute.assign(devCount, MuteMask{});
        solo.assign(devCount, MuteMask{});
        applied.assign(devCount, MuteMask{});

        info = std::make_shared<const TrackInfo>(std::move(ti));
        tr.totalFrames.store(tl.totalFrames, std::memory_order_relaxed);

        renderPos = 0;
        if (startPos > 0) {
            const SampleTime target = clampSeek(startPos);
            auto s = dec->seek(target);
            if (!s.ok()) { fail(s.error()); return false; }
            renderPos = dec->positionFrames();
        }
        publishPosition();
        if (scopesEnabled) scopes.bind(track, trackDecoderConfig(), *info, *dec);
        if (announce) postEvent(evt::TrackLoaded{info});
        return true;
    }

    void fail(const Error& e)
    {
        scopes.reset();
        pcm.setActive(false);
        setState(TransportState::Failed);
        postEvent(evt::Error{e});
        if (dec) { dec->close(); dec.reset(); }
    }

    [[nodiscard]] SampleTime clampSeek(SampleTime frame) const
    {
        if (tl.seekLimit == ~std::uint64_t{0}) return frame;
        return std::min<SampleTime>(frame, tl.seekLimit ? tl.seekLimit - 1 : 0);
    }

    // ---------------------------------------------------------------- komendy
    void onLoad(const cmd::Load& c)
    {
        pcm.setActive(false);
        setState(TransportState::Loading);
        newEpoch(0);
        closeTrack();
        if (!c.track || !c.track->bytes || c.track->raw().empty()) {
            fail(makeError(ErrorCode::InvalidArgument, "Load: empty track"));
            return;
        }
        track = c.track;
        renderPos = 0;
        if (!openTrack(0, true)) return;
        syncScopesNow();
        setState(c.autoplay ? TransportState::Playing : TransportState::Stopped);
        pcm.setActive(c.autoplay);
    }

    void onPlay()
    {
        if (!dec) return;
        if (state == TransportState::Ended) {
            if (!doSeek(0)) return;
        } else if (state != TransportState::Stopped && state != TransportState::Paused) {
            return;
        }
        fadeInTotal = fadeInLeft = std::max<std::uint32_t>(1, cfg.decoder.sampleRate * kFadeMs / 1000);
        setState(TransportState::Playing);
        pcm.setActive(true);
    }

    void onPause()
    {
        if (!dec || state != TransportState::Playing) return;
        pcm.setActive(false);
        setState(TransportState::Paused);
        // Ogon z zanikiem po tresci, ktora juz czeka w pierscieniu: bez klikniecia i bez skoku pozycji.
        if (PcmRing::Block* blk = pcm.acquireWrite()) {
            const std::uint32_t n = std::max<std::uint32_t>(1, cfg.decoder.sampleRate * kFadeMs / 1000);
            if (renderInto(*blk, std::min(n, cfg.decoder.maxBlockFrames), /*fadeOut*/ true, /*scope*/ false))
                pcm.publish();
        }
    }

    void onStop()
    {
        if (!dec) return;
        pcm.setActive(false);
        newEpoch(0);
        auto s = dec->seek(0);
        if (!s.ok()) { fail(s.error()); return; }
        renderPos = dec->positionFrames();
        scopes.invalidate();
        syncScopesNow();
        publishPosition();
        setState(TransportState::Stopped);
    }

    bool doSeek(SampleTime frame)
    {
        const SampleTime target = clampSeek(frame);
        newEpoch(target);
        auto s = dec->seek(target);
        if (!s.ok()) { fail(s.error()); return false; }
        renderPos = dec->positionFrames();
        scopes.invalidate();
        syncScopesNow();
        tr.audibleFrame.store(renderPos, std::memory_order_relaxed);
        publishPosition();
        updateLoopStatus(renderPos);
        return true;
    }

    void onSeek(const cmd::Seek& c)
    {
        if (!dec || state == TransportState::Loading || state == TransportState::Failed) return;
        const bool wasPlaying = state == TransportState::Playing;
        if (!doSeek(c.frame)) return;
        if (!wasPlaying) setState(TransportState::Paused);
    }

    // Zmiana ustawien niezmiennych w IDecoder (petle, wyciszenie, koniec): close+open+seek (decoder.hpp).
    void reconfigure()
    {
        if (!dec || !track) return;
        const TransportState keep = state;
        const SampleTime pos = audiblePosition();
        pcm.setActive(false);
        newEpoch(pos);
        dec->close();
        dec.reset();
        if (!openTrack(pos, false)) return;
        applyAllMasks();
        syncScopesNow();
        // Po skroceniu os czasu pozycja moze byc poza koncem - clampSeek obcial ja w openTrack.
        setState(keep == TransportState::Ended ? TransportState::Paused : keep);
        pcm.setActive(state == TransportState::Playing);
        updateLoopStatus(renderPos);
    }

    // Synchronizuje kopie kanalowe z glownym dekoderem teraz (stan po seeku jest wtedy identyczny).
    void syncScopesNow()
    {
        if (dec && scopes.needsSync()) scopes.syncStep(renderPos, kScopeSyncNowBudget);
    }

    void onSetChannelScopes(bool enabled)
    {
        if (enabled == scopesEnabled) return;
        scopesEnabled = enabled;
        if (!enabled) { scopes.reset(); return; }
        if (dec && info && track) scopes.bind(track, trackDecoderConfig(), *info, *dec);
    }

    void onSetMute(const cmd::SetMute& c)
    {
        if (c.device >= userMute.size()) return;
        userMute[c.device] = c.mask;
        applyMasks(c.device);
    }

    void onSetSolo(const cmd::SetSolo& c)
    {
        if (c.device >= solo.size() || !info) return;
        const DeviceDesc& d = info->layout.devices[c.device];
        MuteMask& s = solo[c.device];
        const std::uint16_t linkedCount = static_cast<std::uint16_t>(d.channelCount - d.mainChannelCount);
        if (c.channel == kChipSumChannel) {
            s = c.enabled ? MuteMask{fullMask(d.mainChannelCount), fullMask(linkedCount)} : MuteMask{};
        } else if (c.channel < d.mainChannelCount) {
            const std::uint32_t bit = 1u << c.channel;
            s.main = c.enabled ? (s.main | bit) : (s.main & ~bit);
        } else if (c.channel < d.channelCount) {
            const std::uint32_t bit = 1u << (c.channel - d.mainChannelCount);
            s.linked = c.enabled ? (s.linked | bit) : (s.linked & ~bit);
        } else {
            return;
        }
        applyAllMasks();     // solo dziala globalnie (wycisza tez inne uklady)
    }

    // Maska efektywna = wyciszenia uzytkownika + (jesli ktorekolwiek solo) wszystko poza solo.
    [[nodiscard]] MuteMask effectiveMask(std::size_t d, bool anySolo) const
    {
        MuteMask m = userMute[d];
        if (anySolo) {
            const DeviceDesc& dd = info->layout.devices[d];
            const std::uint16_t linkedCount = static_cast<std::uint16_t>(dd.channelCount - dd.mainChannelCount);
            m.main |= ~solo[d].main & fullMask(dd.mainChannelCount);
            m.linked |= ~solo[d].linked & fullMask(linkedCount);
        }
        return m;
    }

    [[nodiscard]] bool anySolo() const
    {
        return std::any_of(solo.begin(), solo.end(),
                           [](const MuteMask& m) { return m.main != 0 || m.linked != 0; });
    }

    void applyMasks(std::size_t d)
    {
        if (!dec || d >= userMute.size()) return;
        const MuteMask m = effectiveMask(d, anySolo());
        if (m == applied[d]) return;
        dec->setMute(static_cast<DeviceId>(d), m);
        applied[d] = m;
    }

    void applyAllMasks()
    {
        for (std::size_t d = 0; d < userMute.size(); ++d) applyMasks(d);
    }

    // ---------------------------------------------------------------- render
    void updateLoopStatus(SampleTime pos)
    {
        std::uint32_t loopIdx = 0;
        if (tl.loopFrames > 0 && pos >= tl.introFrames + tl.loopFrames)
            loopIdx = static_cast<std::uint32_t>((pos - tl.introFrames) / tl.loopFrames);
        tr.loopIndex.store(loopIdx, std::memory_order_relaxed);
        tr.fadeActive.store(tl.fadeFrames > 0 && !tl.infinite && pos >= tl.fadeStartFrame,
                            std::memory_order_relaxed);
    }

    // Renderuje jeden blok do `blk` (bez publish). false = nic nie zapisano.
    bool renderInto(PcmRing::Block& blk, std::uint32_t maxFrames, bool fadeOut, bool withScope)
    {
        ScopeBlock* sb = withScope ? scopeBlock.get() : nullptr;
        const SampleTime start = renderPos;
        RenderResult rr;
        try {
            rr = dec->render(blk.samples, maxFrames, sb);
        } catch (const std::bad_alloc&) {
            postError(ErrorCode::OutOfMemory, "out of memory in render");
            rr.ended = true;
        } catch (const std::exception& e) {
            postError(ErrorCode::DecoderFailure, e.what());
            rr.ended = true;
        }
        if (auto err = dec->takeLastError()) postEvent(evt::Error{std::move(*err)});

        const std::uint32_t n = rr.frames;
        if (n > 0) {
            // Mikro-fade (wejscie po Load/Seek/Play) lub zanik (przed pauza).
            float* s = blk.samples;
            if (fadeOut) {
                for (std::uint32_t i = 0; i < n; ++i) {
                    const float g = 1.0f - static_cast<float>(i + 1) / static_cast<float>(n);
                    s[2 * i] *= g;
                    s[2 * i + 1] *= g;
                }
            } else if (fadeInLeft > 0) {
                const std::uint32_t k = std::min(n, fadeInLeft);
                for (std::uint32_t i = 0; i < k; ++i) {
                    const float g = static_cast<float>(fadeInTotal - fadeInLeft + i) /
                                    static_cast<float>(fadeInTotal);
                    s[2 * i] *= g;
                    s[2 * i + 1] *= g;
                }
                fadeInLeft -= k;
            }
            blk.epoch = epoch;
            blk.startFrame = start;
            blk.frames = n;
            // Kopie kanalowe ZAWSZE w kroku z glownym dekoderem (takze ogon po Pause bez scope).
            if (scopes.active()) scopes.render(n, start, sb);
            if (sb) {
                sb->epoch = epoch;
                sb->startFrame = start;
                (void)scopeRing.tryPush(*sb);
            }
            renderPos = start + n;
            publishPosition();
            updateLoopStatus(start);
        }
        if (rr.ended) onTrackEnded();
        return n > 0;
    }

    void onTrackEnded()
    {
        pcm.setActive(false);
        if (state == TransportState::Playing || state == TransportState::Paused) {
            setState(TransportState::Ended);
            postEvent(evt::TrackEnded{});
        }
    }

    void pump()
    {
        const std::int64_t target = static_cast<std::int64_t>(cfg.decoder.sampleRate) * kTargetBufferMs / 1000;
        for (std::uint32_t pass = 0; pass < kMaxBlocksPerPass; ++pass) {
            if (state != TransportState::Playing || !dec) return;
            if (pcm.queuedFrames() >= target) return;
            PcmRing::Block* blk = pcm.acquireWrite();
            if (!blk) return;
            if (renderInto(*blk, cfg.decoder.maxBlockFrames, false, true)) pcm.publish();
        }
    }

    void pollUnderruns()
    {
        const std::uint32_t u = pcm.underruns();
        tr.underruns.store(u, std::memory_order_relaxed);
        if (u != lastUnderruns) {
            postEvent(evt::Underrun{u - lastUnderruns});
            lastUnderruns = u;
        }
    }

    // ---------------------------------------------------------------- petla
    void handle(Command& c)
    {
        std::visit(Overloaded{
            [&](cmd::Load& v) { onLoad(v); },
            [&](cmd::Play&) { onPlay(); },
            [&](cmd::Pause&) { onPause(); },
            [&](cmd::Stop&) { onStop(); },
            [&](cmd::Seek& v) { onSeek(v); },
            [&](cmd::SetLoopCount& v) {
                if (cfg.decoder.loopCount != v.count) { cfg.decoder.loopCount = v.count; reconfigure(); }
            },
            [&](cmd::SetFade& v) {
                if (cfg.decoder.fadeMs != v.ms) { cfg.decoder.fadeMs = v.ms; reconfigure(); }
            },
            [&](cmd::SetVolume& v) { pcm.setVolume(volumeCurve(v.linear)); },
            [&](cmd::SetMute& v) { onSetMute(v); },
            [&](cmd::SetSolo& v) { onSetSolo(v); },
            [&](cmd::SetEndBehavior& v) {
                if (cfg.decoder.nonLoopEnd != v.value) { cfg.decoder.nonLoopEnd = v.value; reconfigure(); }
            },
            [&](cmd::SetChannelScopes& v) { onSetChannelScopes(v.enabled); },
        }, c);
    }

    void guarded(Command& c)
    {
        try {
            handle(c);
        } catch (const std::bad_alloc&) {
            fail(makeError(ErrorCode::OutOfMemory, "out of memory handling command"));
        } catch (const std::exception& e) {
            fail(makeError(ErrorCode::Internal, std::string("exception handling command: ") + e.what()));
        }
    }

    void loop()
    {
        PreciseSleeper sleeper;
        Command c;
        while (!stopFlag.load(std::memory_order_acquire)) {
            while (cmds.tryPop(c)) guarded(c);
            try {
                if (dec && scopes.needsSync() && state != TransportState::Loading && state != TransportState::Failed)
                    scopes.syncStep(renderPos, kScopeSyncBudget);
                pump();
            } catch (const std::bad_alloc&) {
                fail(makeError(ErrorCode::OutOfMemory, "out of memory in render loop"));
            } catch (const std::exception& e) {
                fail(makeError(ErrorCode::Internal, std::string("exception in render loop: ") + e.what()));
            }
            pollUnderruns();
            sleeper.sleepMs(kLoopWaitMs);
        }
        pcm.setActive(false);
    }
};

// ------------------------------------------------------------------------------------------------

Engine::Engine(const EngineConfig& config, PcmRing& pcm) : impl_(std::make_unique<Impl>(config, pcm)) {}
Engine::~Engine() = default;

bool Engine::post(Command command) { return impl_->cmds.tryPush(std::move(command)); }
bool Engine::pollEvent(EngineEvent& out) { return impl_->events.tryPop(out); }
bool Engine::popScope(ScopeBlock& out) { return impl_->scopeRing.tryPop(out); }

TransportSnapshot Engine::snapshot() const noexcept
{
    const TransportAtomics& t = impl_->tr;
    TransportSnapshot s;
    s.state = static_cast<TransportState>(t.state.load(std::memory_order_relaxed));
    s.epoch = t.epoch.load(std::memory_order_acquire);
    s.renderedFrame = t.renderedFrame.load(std::memory_order_relaxed);
    // Pozycja slyszana: z klatek zuzytych przez callback (PcmRing), o ile naleza do biezacej epoki;
    // do tego czasu - pozycja startowa epoki (zapisana przez silnik w TransportAtomics::audibleFrame).
    const PcmRing::Audible a = impl_->pcm.audible();
    s.audibleFrame = (a.valid && a.epoch24 == (s.epoch & 0xFFFFFFull))
                         ? a.frame
                         : t.audibleFrame.load(std::memory_order_relaxed);
    s.totalFrames = t.totalFrames.load(std::memory_order_relaxed);
    s.loopIndex = t.loopIndex.load(std::memory_order_relaxed);
    s.underruns = impl_->pcm.underruns();
    s.fadeActive = t.fadeActive.load(std::memory_order_relaxed);
    return s;
}

void Engine::requestStop() noexcept { impl_->stopFlag.store(true, std::memory_order_release); }

void Engine::run()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    impl_->loop();
}

} // namespace vgmplay::engine
