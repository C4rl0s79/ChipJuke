// vgmplay - audio_out: implementacja na miniaudio (jedyne miejsce z MINIAUDIO_IMPLEMENTATION).
#include "vgmplay/audio_out.hpp"

#include <atomic>
#include <cstring>
#include <new>

// Tylko to, czego uzywamy: odtwarzanie f32 przez WASAPI (+ backend null do testow).
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MA_ENABLE_NULL
#define MINIAUDIO_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace vgmplay::audio_out {

struct AudioOut::Impl {
    PcmRing* ring = nullptr;
    Config config;
    DeviceInfo info;
    ma_context context{};
    ma_device device{};
    bool contextInit = false;
    bool deviceInit = false;
    bool started = false;
    bool wasStarted = false;        // do reopen()
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> lost{false};
    std::atomic<bool> rerouted{false};
    std::atomic<bool> interrupted{false};

    // Callbacki miniaudio: wolane z watku audio.
    static void dataCallback(ma_device* dev, void* output, const void* /*input*/, ma_uint32 frames)
    {
        auto* self = static_cast<Impl*>(dev->pUserData);
        self->ring->consume(static_cast<float*>(output), frames);
    }

    static void notificationCallback(const ma_device_notification* n)
    {
        auto* self = static_cast<Impl*>(n->pDevice->pUserData);
        switch (n->type) {
        case ma_device_notification_type_stopped:
            if (!self->stopRequested.load(std::memory_order_relaxed))
                self->lost.store(true, std::memory_order_relaxed);
            break;
        case ma_device_notification_type_rerouted:
            self->rerouted.store(true, std::memory_order_relaxed);
            break;
        case ma_device_notification_type_interruption_began:
            self->interrupted.store(true, std::memory_order_relaxed);
            break;
        default:
            break;
        }
    }

    void teardown() noexcept
    {
        if (deviceInit) {
            stopRequested.store(true, std::memory_order_relaxed);
            ma_device_uninit(&device);
            deviceInit = false;
        }
        if (contextInit) {
            ma_context_uninit(&context);
            contextInit = false;
        }
        started = false;
    }
};

AudioOut::AudioOut(PcmRing& ring) : ring_(ring), impl_(std::make_unique<Impl>())
{
    impl_->ring = &ring;
}

AudioOut::~AudioOut()
{
    close();
}

Result<DeviceInfo> AudioOut::open(const Config& config)
{
    close();
    Impl& d = *impl_;
    d.config = config;
    d.info = DeviceInfo{};
    d.stopRequested.store(false, std::memory_order_relaxed);
    d.lost.store(false, std::memory_order_relaxed);
    d.rerouted.store(false, std::memory_order_relaxed);
    d.interrupted.store(false, std::memory_order_relaxed);

    const ma_backend backends[1] = {
        config.backend == Config::Backend::Null ? ma_backend_null : ma_backend_wasapi};
    ma_result res = ma_context_init(backends, 1, nullptr, &d.context);
    if (res != MA_SUCCESS)
        return makeError(ErrorCode::NotFound,
                         std::string("ma_context_init failed: ") + ma_result_description(res));
    d.contextInit = true;

    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 2;
    dc.playback.shareMode = ma_share_mode_shared;
    dc.sampleRate = config.sampleRate;                     // 0 = natywna
    dc.periodSizeInMilliseconds = config.periodMs;
    dc.periods = config.periods;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.noPreSilencedOutputBuffer = MA_FALSE;
    dc.dataCallback = &Impl::dataCallback;
    dc.notificationCallback = &Impl::notificationCallback;
    dc.pUserData = &d;
    dc.wasapi.usage = ma_wasapi_usage_pro_audio;

    res = ma_device_init(&d.context, &dc, &d.device);
    if (res != MA_SUCCESS) {
        d.teardown();
        return makeError(ErrorCode::NotFound,
                         std::string("ma_device_init failed: ") + ma_result_description(res));
    }
    d.deviceInit = true;

    char name[256] = {};
    size_t len = 0;
    if (ma_device_get_name(&d.device, ma_device_type_playback, name, sizeof(name), &len) == MA_SUCCESS)
        d.info.name = name;
    d.info.backend = ma_get_backend_name(d.device.pContext->backend);
    d.info.sampleRate = d.device.sampleRate;
    d.info.channels = d.device.playback.channels;
    d.info.periodFrames = d.device.playback.internalPeriodSizeInFrames;
    d.info.periods = d.device.playback.internalPeriods;
    const std::uint32_t internalRate =
        d.device.playback.internalSampleRate ? d.device.playback.internalSampleRate : d.device.sampleRate;
    d.info.bufferMs = 1000.0 * static_cast<double>(d.info.periodFrames) *
                      static_cast<double>(d.info.periods) / static_cast<double>(internalRate);
    return d.info;
}

Result<void> AudioOut::start()
{
    Impl& d = *impl_;
    if (!d.deviceInit)
        return makeError(ErrorCode::InvalidArgument, "audio device not open");
    if (d.started)
        return {};
    d.stopRequested.store(false, std::memory_order_relaxed);
    const ma_result res = ma_device_start(&d.device);
    if (res != MA_SUCCESS)
        return makeError(ErrorCode::Io,
                         std::string("ma_device_start failed: ") + ma_result_description(res));
    d.started = true;
    d.wasStarted = true;
    return {};
}

void AudioOut::stop()
{
    Impl& d = *impl_;
    if (!d.deviceInit || !d.started) return;
    d.stopRequested.store(true, std::memory_order_relaxed);
    ma_device_stop(&d.device);
    d.started = false;
    d.wasStarted = false;
}

void AudioOut::close()
{
    impl_->teardown();
}

bool AudioOut::isOpen() const noexcept { return impl_->deviceInit; }
bool AudioOut::isStarted() const noexcept { return impl_->started; }
std::uint32_t AudioOut::sampleRate() const noexcept { return impl_->deviceInit ? impl_->info.sampleRate : 0; }
const DeviceInfo& AudioOut::info() const noexcept { return impl_->info; }

DeviceEvents AudioOut::pollEvents() noexcept
{
    DeviceEvents e;
    e.lost = impl_->lost.exchange(false, std::memory_order_relaxed);
    e.rerouted = impl_->rerouted.exchange(false, std::memory_order_relaxed);
    e.interrupted = impl_->interrupted.exchange(false, std::memory_order_relaxed);
    return e;
}

Result<DeviceInfo> AudioOut::reopen()
{
    const bool wasStarted = impl_->wasStarted || impl_->started || impl_->lost.load();
    const Config cfg = impl_->config;
    auto r = open(cfg);
    if (!r.ok()) return r;
    if (wasStarted) {
        auto s = start();
        if (!s.ok()) return s.error();
    }
    return r;
}

} // namespace vgmplay::audio_out
