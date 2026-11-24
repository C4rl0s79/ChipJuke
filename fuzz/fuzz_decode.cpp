// Cel 5: pelny loadTrackFromMemory + render N probek przez dekoder (libvgm w ASan).
// Dekoder waliduje wejscie sam; render ograniczony do ~2 s audio na wejscie.
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "fuzz_common.hpp"
#include "vgmplay/loader.hpp"
#include "vgmplay/vgm_decoder.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace vgmplay;
    auto t = loader::loadTrackFromMemory({data, size}, {}, fuzz::smallLimits());
    if (!t) return 0;
    auto dec = engine::makeVgmDecoder();
    DecoderConfig cfg;
    cfg.sampleRate = 44100;
    cfg.loopCount = 1;
    cfg.fadeMs = 100;
    if (!dec->open(t.value().raw(), cfg).ok()) return 0;
    if (!dec->start().ok()) return 0;
    std::vector<float> buf(2u * kMaxBlockFrames);
    std::uint64_t frames = 0;
    while (frames < 2u * 44100) {
        const auto r = dec->render(buf.data(), kMaxBlockFrames, nullptr);
        frames += r.frames;
        if (r.ended || r.frames == 0) break;
    }
    return 0;
}
