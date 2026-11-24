// Wspolne dla celow libFuzzer: male limity (1 MiB), zeby fuzzer nie zjadal pamieci.
#pragma once

#include "vgmplay/format.hpp"

namespace vgmplay::fuzz {

inline format::Limits smallLimits()
{
    format::Limits l;
    l.maxInputBytes = 1u << 20;
    l.maxDecompressedBytes = 1u << 20;
    l.maxPcmBudgetBytes = 1u << 20;
    l.maxSingleBlockBytes = 1u << 20;
    l.maxCommands = 1'000'000;
    l.maxTotalSamples = 44100ull * 60 * 10;
    return l;
}

} // namespace vgmplay::fuzz
