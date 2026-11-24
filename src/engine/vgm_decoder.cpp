// vgmplay - engine: VgmDecoder (IDecoder) na PlayerA + TapVgmPlayer. Patrz ADR 0002.
// WATEK: wszystko wylacznie z watku silnika. Granica wyjatkow: std::bad_alloc/std::exception
// z libvgm lapane tu i zamieniane na Result/ended.
#include "vgmplay/vgm_decoder.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include "chip_map.hpp"
#include "tap_vgm_player.hpp"
#include "vgmplay/format.hpp"
#include "vgmplay/timeline.hpp"

namespace vgmplay::engine {
namespace {

using detail::TapVgmPlayer;

constexpr std::uint32_t kMinRate = 8000;
constexpr std::uint32_t kMaxRate = 768000;
constexpr float kInvInt32 = 1.0f / 2147483648.0f;

// Pierscien ostatnich komunikatow libvgm: staly rozmiar, bez alokacji (callback leci synchronicznie
// z wnetrza Render/Seek).
struct LogRing {
    static constexpr std::size_t kCount = 8;
    static constexpr std::size_t kLen = 160;
    char msg[kCount][kLen] = {};
    std::size_t next = 0;
    std::size_t total = 0;

    void add(const char* tag, const char* text) noexcept
    {
        char* dst = msg[next % kCount];
        std::snprintf(dst, kLen, "%s: %s", tag ? tag : "player", text ? text : "");
        // zdejmij konczacy znak nowej linii
        for (std::size_t i = std::strlen(dst); i > 0 && (dst[i - 1] == '\n' || dst[i - 1] == '\r'); --i)
            dst[i - 1] = '\0';
        ++next;
        ++total;
    }
    [[nodiscard]] std::string tail(std::size_t n = 3) const
    {
        std::string s;
        const std::size_t have = std::min(total, kCount);
        const std::size_t take = std::min(n, have);
        for (std::size_t k = take; k > 0; --k) {
            if (!s.empty()) s += " | ";
            s += msg[(next - k) % kCount];
        }
        return s;
    }
};

struct DevRef {
    std::uint8_t devId = 0;
    std::uint8_t instance = 0;
    std::uint16_t mainChannels = 0;
    std::uint16_t linkedChannels = 0;
    MuteMask mask{};            // ostatnio ustawiona maska (do ponownego zastosowania po zmianie zamrozenia)
    bool frozen = false;        // E1.5: Render pomija emulacje (muteOpts.disable) - tylko kopie kanalowe
};

class VgmDecoder final : public IDecoder {
public:
    VgmDecoder() = default;
    ~VgmDecoder() override { closeImpl(); }

    Result<TrackInfo> open(std::span<const std::uint8_t> raw, const DecoderConfig& cfg) override
    {
        closeImpl();
        try {
            return openImpl(raw, cfg);
        } catch (const std::bad_alloc&) {
            closeImpl();
            return makeError(ErrorCode::OutOfMemory, "out of memory while opening track");
        } catch (const std::exception& e) {
            closeImpl();
            return makeError(ErrorCode::DecoderFailure, std::string("exception in libvgm: ") + e.what());
        }
    }

    Result<void> start() override
    {
        if (!pa_) return makeError(ErrorCode::InvalidArgument, "start() before open()");
        try {
            if (started_) return {};
            const UINT8 r = pa_->Start();
            if (r >= 0x80) return makeError(ErrorCode::DecoderFailure, "PlayerA::Start failed");
            started_ = true;
            // Uklady, ktorych rdzen nie wystartowal, libvgm pomija po cichu - tu to bledem.
            if (tap_->startedDeviceCount() != devs_.size()) {
                return makeError(ErrorCode::DecoderFailure,
                                 "some chip cores failed to start: " + log_.tail());
            }
            // Maski mute zawsze od zera (opcje urzadzen libvgm zyja dluzej niz utwor).
            for (std::size_t i = 0; i < devs_.size(); ++i) { devs_[i].frozen = false; applyMute(i, MuteMask{}); }
            // Dummy-render: poczatkowe bloki danych (0x67) ladowane tu, nie w czasie rzeczywistym.
            (void)pa_->Render(0, nullptr);
            pos_ = pa_->GetCurPos(PLAYPOS_SAMPLE);
            ended_ = false;
            fadeRequested_ = false;
            return {};
        } catch (const std::bad_alloc&) {
            return makeError(ErrorCode::OutOfMemory, "out of memory in start()");
        } catch (const std::exception& e) {
            return makeError(ErrorCode::DecoderFailure, std::string("exception in libvgm: ") + e.what());
        }
    }

    RenderResult render(float* out, std::uint32_t maxFrames, ScopeBlock* scope) override
    {
        RenderResult res;
        if (!started_ || ended_ || !out || maxFrames == 0) {
            res.ended = ended_;
            return res;
        }
        const std::uint32_t frames = std::min(maxFrames, cfg_.maxBlockFrames);
        try {
            // Tryb "wyciszenie na koncu utworu bez petli" (opcja; domyslnie wylaczony).
            if (nonLoopFade_ && !fadeRequested_ && pos_ + frames >= fadeStartNonLoop_) {
                (void)pa_->FadeOut();
                fadeRequested_ = true;
            }
            const UINT32 bytes = pa_->Render(frames * 8u, scratch_.data());
            const std::uint32_t n = std::min<std::uint32_t>(bytes / 8u, frames);
            const std::int32_t* s = scratch_.data();
            for (std::uint32_t i = 0; i < n; ++i) {
                const float l = static_cast<float>(s[2 * i + 0]) * kInvInt32;
                const float r = static_cast<float>(s[2 * i + 1]) * kInvInt32;
                out[2 * i + 0] = l;
                out[2 * i + 1] = r;
                if (scope) {
                    scope->masterL[i] = l;
                    scope->masterR[i] = r;
                }
            }
            if (scope) {
                scope->frames = n;
                scope->extraStreamCount = 0;
            }
            pos_ += n;
            res.frames = n;
            if (pa_->GetState() & PLAYSTATE_FIN) {
                ended_ = true;
                res.ended = true;
            }
        } catch (const std::bad_alloc&) {
            ended_ = true;
            res.ended = true;
            lastError_ = "out of memory in render()";
        } catch (const std::exception& e) {
            ended_ = true;
            res.ended = true;
            lastError_ = std::string("exception in libvgm: ") + e.what();
        }
        return res;
    }

    Result<void> seek(SampleTime frame) override
    {
        if (!started_) return makeError(ErrorCode::InvalidArgument, "seek() before start()");
        try {
            const UINT32 target = static_cast<UINT32>(
                std::min<SampleTime>(frame, std::numeric_limits<UINT32>::max()));
            const UINT8 r = pa_->Seek(PLAYPOS_SAMPLE, target);
            if (r >= 0x80) return makeError(ErrorCode::DecoderFailure, "PlayerA::Seek failed");
            pos_ = pa_->GetCurPos(PLAYPOS_SAMPLE);
            ended_ = (pa_->GetState() & PLAYSTATE_FIN) != 0;
            fadeRequested_ = nonLoopFade_ && pos_ >= fadeStartNonLoop_;
            return {};
        } catch (const std::bad_alloc&) {
            return makeError(ErrorCode::OutOfMemory, "out of memory in seek()");
        } catch (const std::exception& e) {
            return makeError(ErrorCode::DecoderFailure, std::string("exception in libvgm: ") + e.what());
        }
    }

    void fadeOut() override
    {
        if (pa_ && started_) (void)pa_->FadeOut();
    }

    void setMute(DeviceId device, MuteMask mask) override
    {
        if (!pa_ || device >= devs_.size()) return;
        applyMute(device, mask);
    }

    bool channelIsolatable(DeviceId device, std::uint16_t channel) const override
    {
        if (!started_ || !tap_ || device >= devs_.size() || channel >= 32) return false;
        const DevRef& r = devs_[device];
        const unsigned sup = tap_->muteSupport(r.devId, r.instance);
        if (channel < r.mainChannels) return (sup & 1u) != 0;
        if (channel < r.mainChannels + r.linkedChannels) {
            return (sup & 2u) != 0 && channel - r.mainChannels < 32;
        }
        return false;
    }

    void setDeviceFrozen(DeviceId device, bool frozen) override
    {
        if (!pa_ || device >= devs_.size()) return;
        devs_[device].frozen = frozen;
        applyMute(device, devs_[device].mask);
    }

    SampleTime positionFrames() const override { return pos_; }
    const ChipStateSnapshot* chipState() const override { return nullptr; }

    std::vector<CoreInfo> availableCores(ChipType type) const override
    {
        std::vector<CoreInfo> out;
        std::uint8_t devId = 0;
        if (!detail::chipTypeToDevId(type, devId)) return out;
        const DEV_DECL* decl = SndEmu_GetDevDecl(devId, nullptr, 0);
        if (!decl) return out;
        for (const DEV_DEF* const* c = decl->cores; *c; ++c) {
            CoreInfo ci;
            ci.coreId = (*c)->coreID;
            ci.name = (*c)->name ? (*c)->name : "";
            ci.author = (*c)->author ? (*c)->author : "";
            out.push_back(std::move(ci));
        }
        return out;
    }

    void close() override { closeImpl(); }

    std::optional<Error> takeLastError() override
    {
        if (lastError_.empty()) return std::nullopt;
        Error e = makeError(ErrorCode::DecoderFailure, std::move(lastError_));
        lastError_.clear();
        return e;
    }

private:
    void closeImpl() noexcept
    {
        // Kolejnosc: PlayerA (Stop + UnloadFile) przed DataLoader_Deinit - VGMPlayer trzyma
        // wskaznik do danych DataLoadera do UnloadFile().
        try { pa_.reset(); } catch (...) {}
        tap_ = nullptr;
        if (dl_) { DataLoader_Deinit(dl_); dl_ = nullptr; }
        devs_.clear();
        scratch_.clear();
        scratch_.shrink_to_fit();
        started_ = false;
        ended_ = false;
        pos_ = 0;
        fadeRequested_ = false;
        nonLoopFade_ = false;
        lastError_.clear();
    }

    void applyMute(std::size_t idx, const MuteMask& m)
    {
        devs_[idx].mask = m;
        PLR_MUTE_OPTS mo{};
        // chnMute, nie "disable" (zamrazanie emulacji) - ADR pkt 4a; "disable" (bity 0/1 = glowny/polaczony)
        // tylko na zadanie dla kopii kanalowych (setDeviceFrozen, E1.5).
        mo.disable = devs_[idx].frozen ? 0x03 : 0x00;
        mo.chnMute[0] = m.main;
        mo.chnMute[1] = m.linked;
        (void)tap_->SetDeviceMuting(PLR_DEV_ID(devs_[idx].devId, devs_[idx].instance), mo);
    }

    static void logCallback(void* userParam, PlayerBase* /*player*/, UINT8 level, UINT8 /*srcType*/,
                            const char* srcTag, const char* message)
    {
        if (level > PLRLOG_WARN) return;
        static_cast<VgmDecoder*>(userParam)->log_.add(srcTag, message);
    }

    Result<TrackInfo> openImpl(std::span<const std::uint8_t> raw, const DecoderConfig& cfg)
    {
        if (raw.size() < 0x38)
            return makeError(ErrorCode::Truncated, "VGM data shorter than the minimal header");
        if (std::memcmp(raw.data(), "Vgm ", 4) != 0)
            return makeError(ErrorCode::BadSignature, "missing \"Vgm \" signature", 0);
        if (raw.size() > std::numeric_limits<UINT32>::max() - format::kParserPadding)
            return makeError(ErrorCode::TooLarge, "VGM data too large");
        if (cfg.sampleRate < kMinRate || cfg.sampleRate > kMaxRate)
            return makeError(ErrorCode::InvalidArgument, "unsupported output sample rate");

        // Obrona w glebi (ADR 0002 pkt 8): nawet gdy wolajacy twierdzi, ze bufor jest zwalidowany,
        // powtarzamy kontrole naglowka i pre-skan strumienia komend (O(rozmiar), czyste funkcje format).
        // Wejscie z LoadedTrack::raw() przechodzi je bez zmian; surowy bufor z zewnatrz (np. test, fuzz)
        // nie dotrze do libvgm bez walidacji.
        auto hdrRes = format::parseVgmHeader(raw);
        if (!hdrRes) return hdrRes.error();
        const format::HeaderInfo& vhdr = hdrRes.value();
        auto scanRes = format::validateCommandStream(raw, vhdr);
        if (!scanRes) return scanRes.error();
        const format::StreamReport& scan = scanRes.value();

        cfg_ = cfg;
        if (cfg_.maxBlockFrames == 0 || cfg_.maxBlockFrames > kMaxBlockFrames)
            cfg_.maxBlockFrames = kMaxBlockFrames;
        log_ = LogRing{};

        TrackInfo info;
        info.format = "VGM";
        info.warnings = vhdr.warnings;
        info.warnings.insert(info.warnings.end(), scan.warnings.begin(), scan.warnings.end());
        if (auto g = format::parseGd3(raw, vhdr); g) {
            info.tags = std::move(g).value();
            if (info.tags.truncated) info.warnings.push_back({WarningCode::TagTruncated, vhdr.gd3Offset, "GD3 field truncated"});
            if (info.tags.sanitized) info.warnings.push_back({WarningCode::TagSanitized, vhdr.gd3Offset, "GD3 text sanitized"});
        } else {
            info.warnings.push_back({WarningCode::Gd3Invalid, g.error().byteOffset, "GD3 tag invalid: " + g.error().message});
        }

        pa_ = std::make_unique<PlayerA>();
        tap_ = new TapVgmPlayer();
        pa_->RegisterPlayerEngine(tap_);               // PlayerA przejmuje wlasnosc
        if (pa_->SetOutputSettings(cfg_.sampleRate, 2, 32, cfg_.maxBlockFrames) != 0)
            return makeError(ErrorCode::Internal, "PlayerA::SetOutputSettings rejected");
        pa_->SetLogCallback(&VgmDecoder::logCallback, this);

        // Kopia z dopelnieniem (H1/H2); libvgm kopiuje ja do wlasnego bufora w DataLoader_Load.
        {
            std::vector<std::uint8_t> padded;
            padded.reserve(raw.size() + format::kParserPadding);
            padded.assign(raw.begin(), raw.end());
            padded.resize(raw.size() + format::kParserPadding, 0);
            // Lacki wspolne z loaderem: eofOfs widziany przez libvgm == eof parsera; blok PCM w/za punktem petli
            // wylacza petle (plik gra raz; decyzja 2026-11-23).
            format::applyLibvgmPatches(std::span<std::uint8_t>(padded.data(), raw.size()), vhdr, scan);
            dl_ = MemoryLoader_Init(padded.data(), static_cast<UINT32>(padded.size()));
            if (!dl_) return makeError(ErrorCode::OutOfMemory, "MemoryLoader_Init failed");
            if (DataLoader_Load(dl_) != 0)
                return makeError(ErrorCode::Io, "DataLoader_Load failed");
        }
        const UINT8 lr = pa_->LoadFile(dl_);
        if (lr >= 0x80) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "libvgm rejected the file (code 0x%02X)", lr);
            return makeError(lr == 0xF0 ? ErrorCode::BadSignature : ErrorCode::BadHeader,
                             std::string(buf) + (log_.total ? ": " + log_.tail() : ""));
        }

        const VGM_HEADER* hdr = tap_->GetFileHeader();
        const bool hasLoop = hdr->loopOfs != 0;
        PLR_SONG_INFO song{};
        (void)tap_->GetSongInfo(song);
        info.versionBcd = hdr->fileVer;

        // Konfiguracja petli/wyciszenia/ciszy.
        PlayerA::Config pc = pa_->GetConfiguration();
        pc.loopCount = tap_->GetModifiedLoopCount(cfg_.loopCount);
        pc.fadeSmpls = static_cast<UINT32>(static_cast<std::uint64_t>(cfg_.sampleRate) * cfg_.fadeMs / 1000u);
        // Cisza koncowa tylko dla utworow bez petli (po wyciszeniu petli konczymy od razu).
        const bool nonLoopStop = !hasLoop && cfg_.nonLoopEnd == NonLoopEnd::Stop;
        pc.endSilenceSmpls = nonLoopStop
            ? static_cast<UINT32>(static_cast<std::uint64_t>(cfg_.sampleRate) * cfg_.endSilenceMs / 1000u)
            : 0u;
        pa_->SetConfiguration(pc);

        // Czasy z WLASNEGO skanu strumienia (nie z naglowka, ktory moze klamac).
        (void)song;
        info.totalFrames = ticksToFrames(scan.totalSamples, cfg_.sampleRate);
        if (hasLoop && vhdr.hasLoop() && !scan.loopDisabled && scan.loopStartSample < scan.totalSamples) {
            info.introFrames = ticksToFrames(scan.loopStartSample, cfg_.sampleRate);
            info.loopFrames = info.totalFrames - std::min(info.introFrames, info.totalFrames);
        } else {
            info.introFrames = info.totalFrames;
        }
        nonLoopFade_ = !hasLoop && cfg_.nonLoopEnd == NonLoopEnd::FadeOut;
        const std::uint64_t fadeFrames = static_cast<std::uint64_t>(pc.fadeSmpls);
        fadeStartNonLoop_ = info.totalFrames > fadeFrames ? info.totalFrames - fadeFrames : 0;

        // Uklady, rdzenie, kanaly.
        if (auto r = buildLayoutAndCores(info); !r.ok()) return r.error();

        scratch_.assign(static_cast<std::size_t>(cfg_.maxBlockFrames) * 2u, 0);
        return info;
    }

    // Wyznacza layout i ustawia emuCore wg polityki (przed Start - opcje czytane przy Start).
    Result<void> buildLayoutAndCores(TrackInfo& info)
    {
        std::vector<PLR_DEV_INFO> list;
        if (tap_->GetSongDeviceInfo(list) >= 0x80)
            return makeError(ErrorCode::Internal, "GetSongDeviceInfo failed");

        alignas(8) std::uint8_t dummyCfg[256] = {};   // dla ukladow polaczonych (devCfg == NULL przed Start)

        for (std::size_t i = 0; i < list.size(); ++i) {
            const PLR_DEV_INFO& d = list[i];
            if (d.parentIdx != static_cast<UINT32>(-1)) continue;       // polaczone dolaczamy do rodzica
            if (!d.devDecl || !d.devCfg)
                return makeError(ErrorCode::DecoderFailure, "chip without a built-in device declaration");

            const std::uint8_t devId = static_cast<std::uint8_t>(d.type);
            const std::uint8_t inst = static_cast<std::uint8_t>(d.instance);
            const ChipType ctype = detail::devIdToChipType(devId, d.devCfg->flags);
            if (!d.devDecl->cores[0]) {
                // Deklaracja bez rdzeni (np. ES5506 - zaslepka libvgm): uklad bedzie niemy.
                info.warnings.push_back(Warning{WarningCode::Other, kNoOffset,
                    std::string("chip type ") + std::to_string(static_cast<unsigned>(ctype)) +
                    " has no emulation core in this build; it will be silent"});
                continue;
            }

            DevRef ref;
            ref.devId = devId;
            ref.instance = inst;

            DeviceDesc dd;
            dd.id = static_cast<DeviceId>(info.layout.devices.size());
            dd.type = ctype;
            dd.instance = inst;

            // Nazwa: "YM2612" lub "YM2612 #2" przy dwoch ukladach tego samego typu.
            const char* sname = SndEmu_GetDevName(devId, 0x00, d.devCfg);
            dd.name = sname ? sname : "?";
            std::size_t sameType = 0;
            for (const PLR_DEV_INFO& o : list)
                if (o.parentIdx == static_cast<UINT32>(-1) && o.type == d.type) ++sameType;
            if (sameType > 1) dd.name += " #" + std::to_string(inst + 1);

            // Kanaly ukladu glownego.
            appendChannels(dd, d.devDecl, d.devCfg, "");
            ref.mainChannels = static_cast<std::uint16_t>(dd.channelNames.size());

            // Kanaly ukladow polaczonych (maks. jeden liczy sie w maskach libvgm).
            PLR_DEV_OPTS opts{};
            bool haveLinked = false;
            std::uint8_t linkedDevId = 0;
            for (std::size_t j = 0; j < list.size(); ++j) {
                if (list[j].parentIdx != i || haveLinked) continue;
                const PLR_DEV_INFO& l = list[j];
                if (!l.devDecl) continue;
                haveLinked = true;
                linkedDevId = static_cast<std::uint8_t>(l.type);
                const DEV_GEN_CFG* lcfg = l.devCfg ? l.devCfg : reinterpret_cast<const DEV_GEN_CFG*>(dummyCfg);
                const std::size_t before = dd.channelNames.size();
                appendChannels(dd, l.devDecl, lcfg, std::string(SndEmu_GetDevName(linkedDevId, 0, lcfg)) + " ");
                ref.linkedChannels = static_cast<std::uint16_t>(dd.channelNames.size() - before);
            }
            dd.hasLinked = haveLinked;
            dd.mainChannelCount = ref.mainChannels;
            dd.channelCount = static_cast<std::uint16_t>(dd.channelNames.size());

            // Wybor rdzenia: wybor uzytkownika (jesli skompilowany) > polityka vgmplay > domyslny libvgm.
            std::uint32_t mainCore = 0;
            for (const CoreChoice& c : cfg_.cores) {
                if (c.type != ctype || c.instance != inst || c.coreId == 0) continue;
                if (detail::coreAvailable(devId, c.coreId)) {
                    mainCore = c.coreId;
                } else {
                    info.warnings.push_back(Warning{WarningCode::Other, kNoOffset,
                        "requested core " + detail::fccToString(c.coreId) + " is not available for " +
                        dd.name + "; using default"});
                }
                break;
            }
            const detail::CorePreference pref = detail::defaultCorePreference(devId);
            if (mainCore == 0 && !pref.main.empty()) mainCore = detail::firstAvailable(devId, pref.main);
            std::uint32_t linkedCore = 0;
            if (haveLinked && !pref.linked.empty()) {
                linkedCore = detail::firstAvailable(linkedDevId, pref.linked);
                if (linkedCore == 0)
                    info.warnings.push_back(Warning{WarningCode::Other, kNoOffset,
                        "no allowed core for the linked device of " + dd.name});
            }
            dd.coreId = mainCore ? mainCore : detail::libDefaultCore(devId);

            if (mainCore != 0 || linkedCore != 0) {
                const UINT32 id = PLR_DEV_ID(devId, inst);
                if (tap_->GetDeviceOptions(id, opts) == 0) {
                    if (mainCore) opts.emuCore[0] = mainCore;
                    if (linkedCore) opts.emuCore[1] = linkedCore;
                    (void)tap_->SetDeviceOptions(id, opts);
                }
            }

            info.layout.devices.push_back(std::move(dd));
            devs_.push_back(ref);
        }

        for (std::size_t di = 0; di < info.layout.devices.size(); ++di) {
            for (std::uint16_t c = 0; c < info.layout.devices[di].channelCount; ++c)
                info.layout.channels.push_back(ChannelRef{static_cast<DeviceId>(di), c});
        }
        return {};
    }

    static void appendChannels(DeviceDesc& dd, const DEV_DECL* decl, const DEV_GEN_CFG* cfg,
                               const std::string& prefix)
    {
        const std::uint16_t n = decl->channelCount ? decl->channelCount(cfg) : 0;
        const char** names = decl->channelNames ? decl->channelNames(cfg) : nullptr;
        for (std::uint16_t c = 0; c < n; ++c) {
            std::string nm = (names && names[c]) ? std::string(names[c]) : ("Ch " + std::to_string(c + 1));
            dd.channelNames.push_back(prefix + nm);
        }
    }

    DecoderConfig cfg_;
    std::unique_ptr<PlayerA> pa_;
    TapVgmPlayer* tap_ = nullptr;          // wlasnosc PlayerA
    DATA_LOADER* dl_ = nullptr;
    std::vector<DevRef> devs_;
    std::vector<std::int32_t> scratch_;
    LogRing log_;
    std::string lastError_;
    SampleTime pos_ = 0;
    bool started_ = false;
    bool ended_ = false;
    bool nonLoopFade_ = false;
    bool fadeRequested_ = false;
    std::uint64_t fadeStartNonLoop_ = 0;
};

} // namespace

std::unique_ptr<IDecoder> makeVgmDecoder()
{
    return std::make_unique<VgmDecoder>();
}

} // namespace vgmplay::engine
