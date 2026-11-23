// vgmplay - engine: os czasu odtwarzania (petle, wyciszenie, ogon ciszy) w klatkach wyjscia.
// Czysta arytmetyka, bez libvgm; wspolna dla Engine i testow. Zrodlem danych jest wlasny skan
// strumienia (format::StreamReport), nie naglowek pliku.
//
// Model libvgm (PlayerA, zweryfikowany testem): odtwarzanie ma os liniowa (pozycja nie wraca przy
// petli). Wyciszenie startuje w chwili `fadeStartFrame` = intro + loopCount * dlugoscPetli,
// trwa fadeFrames; po jego zakonczeniu utwor konczy sie (bez dodatkowej ciszy).
#pragma once

#include <cstdint>

#include "vgmplay/core.hpp"

namespace vgmplay::engine {

// Liczba przejsc petli po uwzglednieniu modyfikatorow z naglowka VGM (0x7E loopBase, 0x7F
// loopModifier); odpowiada VGMPlayer::GetModifiedLoopCount. 0 = bez konca.
[[nodiscard]] constexpr std::uint32_t modifiedLoopCount(std::uint32_t defaultLoops, std::int8_t loopBase,
                                                        std::uint8_t loopModifier) noexcept
{
    if (defaultLoops == 0) return 0;
    const std::uint32_t modified =
        loopModifier ? (defaultLoops * loopModifier + 0x08u) / 0x10u : defaultLoops;
    if (static_cast<std::int32_t>(modified) <= static_cast<std::int32_t>(loopBase)) return 1;
    return modified - static_cast<std::uint32_t>(static_cast<std::int32_t>(loopBase));
}

// Ticki VGM (44100/s) -> klatki wyjscia; zaokraglenie w dol jak VGMPlayer::Tick2Sample.
[[nodiscard]] constexpr std::uint64_t ticksToFrames(std::uint64_t ticks, std::uint32_t sampleRate) noexcept
{
    return ticks * sampleRate / 44100u;
}

struct TimelineInput {
    std::uint64_t totalTicks = 0;       // dlugosc jednego przejscia (StreamReport::totalSamples)
    std::uint64_t loopStartTick = 0;    // poczatek petli (StreamReport::loopStartSample)
    bool hasLoop = false;
    std::uint32_t loopCount = 2;        // juz po modifiedLoopCount; 0 = bez konca
    std::uint32_t sampleRate = 48000;
    std::uint32_t fadeMs = 1000;
    NonLoopEnd nonLoopEnd = NonLoopEnd::Stop;
    std::uint32_t endSilenceMs = 500;
};

struct PlaybackTimeline {
    std::uint64_t introFrames = 0;      // do poczatku petli (bez petli: cala dlugosc)
    std::uint64_t loopFrames = 0;       // 0 = brak petli
    std::uint64_t onePassFrames = 0;    // intro + petla
    std::uint64_t fadeFrames = 0;       // 0 = brak wyciszenia
    // Poczatek wyciszenia (== koniec ostatniej petli); 0 = brak wyciszenia / nieskonczony.
    std::uint64_t fadeStartFrame = 0;
    // Pelna dlugosc odtwarzania (z wyciszeniem lub cisza koncowa); 0 = nieskonczony.
    std::uint64_t totalFrames = 0;
    // Gorna granica dla Seek: [0, seekLimit); UINT64_MAX = bez ograniczenia (nieskonczony).
    std::uint64_t seekLimit = 0;
    bool infinite = false;
};

[[nodiscard]] inline PlaybackTimeline computeTimeline(const TimelineInput& in) noexcept
{
    PlaybackTimeline t;
    const std::uint64_t total = ticksToFrames(in.totalTicks, in.sampleRate);
    const std::uint64_t fade = static_cast<std::uint64_t>(in.sampleRate) * in.fadeMs / 1000u;
    const std::uint64_t silence = static_cast<std::uint64_t>(in.sampleRate) * in.endSilenceMs / 1000u;
    t.onePassFrames = total;
    if (in.hasLoop && in.loopStartTick < in.totalTicks) {
        t.introFrames = ticksToFrames(in.loopStartTick, in.sampleRate);
        t.loopFrames = total - t.introFrames;
        if (in.loopCount == 0) {
            t.infinite = true;
            t.seekLimit = ~std::uint64_t{0};
            return t;
        }
        t.fadeFrames = fade;
        t.fadeStartFrame = t.introFrames + static_cast<std::uint64_t>(in.loopCount) * t.loopFrames;
        t.totalFrames = t.fadeStartFrame + fade;
        t.seekLimit = t.fadeStartFrame ? t.fadeStartFrame : 1;
    } else {
        t.introFrames = total;
        if (in.nonLoopEnd == NonLoopEnd::FadeOut) {
            t.fadeFrames = fade < total ? fade : total;
            t.fadeStartFrame = total - t.fadeFrames;
            t.totalFrames = total;
        } else {
            t.totalFrames = total + silence;
        }
        t.seekLimit = total ? total : 1;
    }
    return t;
}

} // namespace vgmplay::engine
