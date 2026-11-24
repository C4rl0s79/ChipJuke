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

namespace {

// Identyfikator urzadzenia dla UI/ustawien. WASAPI: wchar_t[] -> UTF-8; inne backendy: hex bajtow.
void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string encodeDeviceId(ma_backend backend, const ma_device_id& id)
{
    std::string out;
    if (backend == ma_backend_wasapi) {
        const wchar_t* w = id.wasapi;
        constexpr std::size_t kMax = sizeof(id.wasapi) / sizeof(id.wasapi[0]);
        for (std::size_t i = 0; i < kMax && w[i] != 0; ++i) {
            std::uint32_t cp = static_cast<std::uint16_t>(w[i]);
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < kMax) {
                const std::uint32_t lo = static_cast<std::uint16_t>(w[i + 1]);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                }
            }
            appendUtf8(out, cp);
        }
        return out;
    }
    static const char* hex = "0123456789abcdef";
    const auto* b = reinterpret_cast<const unsigned char*>(&id);
    for (std::size_t i = 0; i < sizeof(id); ++i) {
        out += hex[b[i] >> 4];
        out += hex[b[i] & 15];
    }
    return out;
}

std::string sanitizeName(const char* s)
{
    std::string out;
    for (const char* p = s; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        out += (c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
    }
    const auto b = out.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    const auto e = out.find_last_not_of(' ');
    return out.substr(b, e - b + 1);
}

ma_backend toMaBackend(Config::Backend b)
{
    return b == Config::Backend::Null ? ma_backend_null : ma_backend_wasapi;
}

struct EnumeratedDevice {
    DeviceEntry entry;
    ma_device_id maId{};
};

Result<std::vector<EnumeratedDevice>> enumerate(ma_context& ctx, ma_backend backend)
{
    std::vector<EnumeratedDevice> out;
    if (backend == ma_backend_null) {
        // Backend null nie ma realnych urzadzen: jedna pozycja stala.
        EnumeratedDevice e;
        e.entry = DeviceEntry{"null", "Null", true};
        out.push_back(std::move(e));
        return out;
    }
    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 captureCount = 0;
    const ma_result res = ma_context_get_devices(&ctx, &playback, &playbackCount, &capture, &captureCount);
    if (res != MA_SUCCESS)
        return makeError(ErrorCode::Io,
                         std::string("ma_context_get_devices failed: ") + ma_result_description(res));
    for (ma_uint32 i = 0; i < playbackCount; ++i) {
        EnumeratedDevice e;
        e.entry.id = encodeDeviceId(backend, playback[i].id);
        e.entry.name = sanitizeName(playback[i].name);
        e.entry.isDefault = playback[i].isDefault != 0;
        e.maId = playback[i].id;
        if (!e.entry.id.empty()) out.push_back(std::move(e));
    }
    return out;
}

} // namespace

Result<std::vector<DeviceEntry>> listDevices(Config::Backend backend)
{
    const ma_backend mb = toMaBackend(backend);
    ma_context ctx{};
    const ma_result res = ma_context_init(&mb, 1, nullptr, &ctx);
    if (res != MA_SUCCESS)
        return makeError(ErrorCode::NotFound,
                         std::string("ma_context_init failed: ") + ma_result_description(res));
    auto devs = enumerate(ctx, mb);
    ma_context_uninit(&ctx);
    if (!devs.ok()) return devs.error();
    std::vector<DeviceEntry> out;
    out.reserve(devs.value().size());
    for (auto& d : devs.value()) out.push_back(std::move(d.entry));
    return out;
}

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

    const ma_backend backends[1] = {toMaBackend(config.backend)};
    ma_result res = ma_context_init(backends, 1, nullptr, &d.context);
    if (res != MA_SUCCESS)
        return makeError(ErrorCode::NotFound,
                         std::string("ma_context_init failed: ") + ma_result_description(res));
    d.contextInit = true;

    // Wybor urzadzenia: pusty id = domyslne (z podazaniem); znane = to urzadzenie; nieznane = domyslne.
    ma_device_id chosenId{};
    bool useChosen = false;
    bool fellBack = false;
    bool found = false;
    if (!config.deviceId.empty()) {
        auto devs = enumerate(d.context, backends[0]);
        if (devs.ok()) {
            for (const auto& e : devs.value()) {
                if (e.entry.id == config.deviceId) {
                    chosenId = e.maId;
                    found = true;
                    // Null: jedyna pozycja to domyslne - bez jawnego id.
                    useChosen = backends[0] != ma_backend_null;
                    break;
                }
            }
        }
        fellBack = !found;
    }

    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.pDeviceID = useChosen ? &chosenId : nullptr;
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
    if (res != MA_SUCCESS && useChosen) {
        // Urzadzenie zniklo/nie da sie otworzyc: ostatnia deska ratunku to domyslne.
        dc.playback.pDeviceID = nullptr;
        useChosen = false;
        found = false;
        fellBack = true;
        res = ma_device_init(&d.context, &dc, &d.device);
    }
    if (res != MA_SUCCESS) {
        d.teardown();
        return makeError(ErrorCode::NotFound,
                         std::string("ma_device_init failed: ") + ma_result_description(res));
    }
    d.deviceInit = true;

    char name[256] = {};
    size_t len = 0;
    if (ma_device_get_name(&d.device, ma_device_type_playback, name, sizeof(name), &len) == MA_SUCCESS)
        d.info.name = sanitizeName(name);
    d.info.deviceId = found ? config.deviceId : std::string();
    d.info.fellBackToDefault = fellBack;
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
