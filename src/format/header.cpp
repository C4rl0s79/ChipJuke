// vgmplay - modul format: parseVgmHeader (ADR 0002 pkt 8.3-8.6, 8.11, 8.14).
//
// Reguly naglowka odwzorowuja VGMPlayer::ParseHeader z libvgm (c5c03a2), ale kazdy offset jest
// liczony w UINT64 i sprawdzany wzgledem rozmiaru pliku. Pola spoza "dlugosci naglowka"
// (= min(dataOfs, offset naglowka rozszerzonego, 0x100)) sa traktowane jak zera - tak samo jak
// _hdrBuffer w libvgm, dzieki czemu lista ukladow zgadza sie z tym, co libvgm naprawde zagra.
//
// Pakowanie ChipEntry::flags (bit 31 = bit 31 surowego zegara):
//   SN76496  : bity 0..15 = 0x28 (noise taps), 16..23 = 0x2A (shift reg width), 24..30 = 0x2B & 0x7F
//   SegaPcm  : 0x3C (interface register) & 0x7FFFFFFF
//   YM2203   : 0x7A (flagi AY8910)         YM2608: 0x7B
//   AY8910   : bity 0..7 = 0x78 (typ), 8..15 = 0x79 (flagi)
//   OKIM6258 : 0x94    K054539: 0x95    C140/C219: 0x96    ES5503: 0xD4    ES5506: 0xD5
//   C352     : 0xD6 (clock divider)     MSM5205: 0xD7
//   pozostale: 0
#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

#include "cmdtable.hpp"
#include "internal.hpp"
#include "vgmplay/format.hpp"

namespace vgmplay::format {
namespace {

using detail::rd16;
using detail::rd32;

struct XhClock {
    std::uint8_t type;
    std::uint32_t clock;
};
struct XhVolume {
    std::uint8_t type;
    std::uint8_t flags;
    std::uint16_t value;
};

void warn(std::vector<Warning>& w, WarningCode code, std::uint64_t ofs, std::string msg)
{
    w.push_back(Warning{code, ofs, std::move(msg)});
}

[[nodiscard]] bool bcdOk(std::uint32_t v) noexcept
{
    for (int shift = 0; shift < 16; shift += 4)
        if (((v >> shift) & 0xF) > 9) return false;
    return true;
}

// Odpowiednik VGMPlayer::ParseFileForFMClocks (wersje < 1.10: jedno pole "FM clock" w 0x10).
void fmClockFix(std::span<const std::uint8_t> file, std::uint64_t dataOfs, std::uint64_t dataEnd,
                std::uint32_t hdrClock, std::uint32_t out[3])
{
    out[0] = hdrClock;  // YM2413
    out[1] = 0;         // YM2612
    out[2] = 0;         // YM2151
    std::uint64_t pos = dataOfs;
    while (pos < dataEnd) {
        const std::uint8_t cmd = file[static_cast<std::size_t>(pos)];
        switch (cmd) {
        case 0x66: return;
        case 0x67:
            if (pos + 7 > dataEnd) return;
            pos += 7 + (rd32(&file[static_cast<std::size_t>(pos) + 3]) & 0x7FFFFFFFu);
            break;
        case 0x51: return;
        case 0x52:
        case 0x53: out[1] = out[0]; out[0] = 0; return;
        case 0x54: out[2] = out[0]; out[0] = 0; return;
        default: {
            const std::uint8_t len = detail::kCmdTable[cmd].len;
            if (len == 0) return;
            pos += len;
            break;
        }
        }
    }
}

[[nodiscard]] std::uint32_t chipFlags(std::size_t idx, const detail::PaddedHeader& h, std::uint32_t rawClock)
{
    std::uint32_t f = 0;
    switch (idx) {
    case 0x00: f = static_cast<std::uint32_t>(h.u16(0x28)) | (static_cast<std::uint32_t>(h.u8(0x2A)) << 16) |
                   (static_cast<std::uint32_t>(h.u8(0x2B) & 0x7F) << 24); break;
    case 0x04: f = h.u32(0x3C) & 0x7FFFFFFFu; break;
    case 0x06: f = h.u8(0x7A); break;
    case 0x07: f = h.u8(0x7B); break;
    case 0x12: f = static_cast<std::uint32_t>(h.u8(0x78)) | (static_cast<std::uint32_t>(h.u8(0x79)) << 8); break;
    case 0x17: f = h.u8(0x94); break;
    case 0x1A: f = h.u8(0x95); break;
    case 0x1C: f = h.u8(0x96); break;
    case 0x24: f = h.u8(0xD4); break;
    case 0x25: f = h.u8(0xD5); break;
    case 0x27: f = h.u8(0xD6); break;
    case 0x2C: f = h.u8(0xD7); break;
    default: break;
    }
    return (f & 0x7FFFFFFFu) | (rawClock & 0x80000000u);
}

} // namespace

Result<HeaderInfo> parseVgmHeader(std::span<const std::uint8_t> file, const Limits& limits)
{
    try {
        const std::uint64_t size = file.size();
        if (size > std::max(limits.maxInputBytes, limits.maxDecompressedBytes))
            return makeError(ErrorCode::TooLarge, "VGM data exceeds size limit");
        if (size < detail::kMinVgmHeader)
            return makeError(ErrorCode::Truncated, "file shorter than the minimal VGM header", size);
        if (std::memcmp(file.data(), "Vgm ", 4) != 0)
            return makeError(ErrorCode::BadSignature, "missing \"Vgm \" signature", 0);

        HeaderInfo hi;
        hi.fileSize = size;
        hi.versionBcd = rd32(&file[0x08]);
        if (hi.versionBcd < 0x100 || hi.versionBcd > 0x2FF || !bcdOk(hi.versionBcd))
            return makeError(ErrorCode::UnsupportedVersion, "unsupported VGM version", 0x08);
        if (hi.versionBcd > 0x172)
            warn(hi.warnings, WarningCode::UnknownVersion, 0x08, "VGM version newer than 1.72; extra fields ignored");

        // --- dataOfs (pkt 4): 0x34 + rel w UINT64; wersje < 1.50 i rel == 0 -> 0x40 ---
        std::uint64_t dataOfs = 0x40;
        if (hi.versionBcd >= 0x150) {
            const std::uint32_t rel = rd32(&file[0x34]);
            if (rel != 0) dataOfs = 0x34ull + rel;
        }
        if (dataOfs < detail::kMinVgmHeader) {
            warn(hi.warnings, WarningCode::HeaderLengthMismatch, 0x34, "data offset inside the fixed header; raised to 0x38");
            dataOfs = detail::kMinVgmHeader;
        }
        if (dataOfs >= size)
            return makeError(ErrorCode::BadHeader, "data offset at or beyond end of file", 0x34);
        hi.dataOffset = dataOfs;

        // --- dlugosc naglowka i naglowek rozszerzony (pkt 6) ---
        std::uint64_t hdrLen = dataOfs;
        std::uint64_t extraAbs = 0;
        if (hdrLen >= 0xC0) {
            const std::uint32_t rel = rd32(&file[0xBC]);
            if (rel != 0) {
                const std::uint64_t abs = 0xBCull + rel;
                if (abs < 0xC0 || abs + 4 > size) {
                    warn(hi.warnings, WarningCode::HeaderLengthMismatch, 0xBC, "extra header offset out of range; ignored");
                } else {
                    extraAbs = abs;
                    if (hdrLen > extraAbs) hdrLen = extraAbs;
                }
            }
        }
        hdrLen = std::min<std::uint64_t>(hdrLen, 0x100);
        const detail::PaddedHeader h{file.data(), static_cast<std::size_t>(hdrLen)};

        // --- eofOfs (pkt 5) ---
        std::uint64_t eofAbs = 0;
        if (const std::uint32_t rel = h.u32(0x04); rel != 0) eofAbs = 0x04ull + rel;
        if (eofAbs == 0 || eofAbs > size) {
            warn(hi.warnings, WarningCode::EofOffsetMismatch, 0x04, "EOF offset invalid; using file size");
            eofAbs = size;
        }
        std::uint64_t dataEnd = eofAbs;
        if (dataEnd <= dataOfs)
            return makeError(ErrorCode::BadHeader, "EOF offset does not leave room for command data", 0x04);

        // --- GD3 (pkt 6) ---
        if (const std::uint32_t rel = h.u32(0x14); rel != 0) {
            const std::uint64_t abs = 0x14ull + rel;
            if (abs < detail::kMinVgmHeader || abs + 12 > eofAbs) {
                warn(hi.warnings, WarningCode::Gd3Invalid, 0x14, "GD3 offset out of range; tags ignored");
            } else {
                hi.gd3Offset = abs;
            }
            // Dane komend koncza sie na poczatku GD3 (jak w libvgm), takze gdy sam GD3 jest uciety.
            if (abs >= dataOfs && abs < dataEnd) dataEnd = abs;
        }
        if (dataEnd <= dataOfs)
            return makeError(ErrorCode::BadHeader, "no command data between data offset and GD3/EOF", 0x14);
        hi.dataEnd = dataEnd;

        // --- naglowek rozszerzony: zegary (drugi uklad) i glosnosci ---
        std::vector<XhClock> xhClocks;
        std::vector<XhVolume> xhVols;
        if (extraAbs != 0 && extraAbs < eofAbs) {
            const std::uint32_t xhLen = rd32(&file[static_cast<std::size_t>(extraAbs)]);
            if (xhLen >= 0x08 && extraAbs + 8 <= size) {
                const std::uint32_t rel = rd32(&file[static_cast<std::size_t>(extraAbs) + 4]);
                const std::uint64_t abs = rel ? extraAbs + 4 + rel : 0;
                if (abs != 0 && abs < size) {
                    std::uint64_t p = abs;
                    const unsigned count = file[static_cast<std::size_t>(p)];
                    ++p;
                    for (unsigned i = 0; i < count && p + 5 <= size; ++i, p += 5)
                        xhClocks.push_back({file[static_cast<std::size_t>(p)], rd32(&file[static_cast<std::size_t>(p) + 1])});
                } else if (abs != 0) {
                    warn(hi.warnings, WarningCode::HeaderLengthMismatch, extraAbs + 4, "extra header clock list out of range; ignored");
                }
            }
            if (xhLen >= 0x0C && extraAbs + 12 <= size) {
                const std::uint32_t rel = rd32(&file[static_cast<std::size_t>(extraAbs) + 8]);
                const std::uint64_t abs = rel ? extraAbs + 8 + rel : 0;
                if (abs != 0 && abs < size) {
                    std::uint64_t p = abs;
                    const unsigned count = file[static_cast<std::size_t>(p)];
                    ++p;
                    for (unsigned i = 0; i < count && p + 4 <= size; ++i, p += 4)
                        xhVols.push_back({file[static_cast<std::size_t>(p)], file[static_cast<std::size_t>(p) + 1],
                                          rd16(&file[static_cast<std::size_t>(p) + 2])});
                } else if (abs != 0) {
                    warn(hi.warnings, WarningCode::HeaderLengthMismatch, extraAbs + 8, "extra header volume list out of range; ignored");
                }
            }
        }

        // --- loop (pkt 6, libvgm :340-353) ---
        hi.totalSamples = h.u32(0x18);
        hi.loopSamples = h.u32(0x20);
        hi.rate = h.u32(0x24);
        hi.volumeModifier = static_cast<std::int8_t>(h.u8(0x7C));
        hi.loopBase = static_cast<std::int8_t>(h.u8(0x7E));
        hi.loopModifier = h.u8(0x7F);
        if (const std::uint32_t rel = h.u32(0x1C); rel != 0) {
            const std::uint64_t abs = 0x1Cull + rel;
            if (abs < dataOfs || abs >= dataEnd) {
                warn(hi.warnings, WarningCode::Other, 0x1C, "loop offset outside [dataOfs, dataEnd); loop ignored");
            } else if (hi.loopSamples == 0) {
                warn(hi.warnings, WarningCode::Other, 0x20, "zero-length loop; loop ignored");
            } else {
                hi.loopOffset = abs;
            }
        }

        // --- uklady (pkt 11) ---
        std::uint32_t fm[3] = {0, 0, 0};
        const bool v101fix = hi.versionBcd < 0x110 && h.u32(0x10) != 0;
        if (v101fix) fmClockFix(file, dataOfs, dataEnd, h.u32(0x10), fm);

        for (std::size_t idx = 0; idx < detail::kVgmChipCount; ++idx) {
            std::uint32_t raw = h.u32(detail::kChipClockOfs[idx]);
            if (v101fix && idx >= 1 && idx <= 3) raw = fm[idx - 1];
            if (raw == 0) continue;

            const bool dual = (raw & 0x40000000u) != 0;
            const std::uint32_t clock = raw & 0x3FFFFFFFu;
            ChipType type = detail::chipTypeFromIndex(idx);
            if (idx == 0x1C && h.u8(0x96) == 2) type = ChipType::C219;
            // T6W28 (SN76496 z bitem 31) to dwie polowki jednego ukladu - jedna instancja.
            const unsigned instances = (dual && !(idx == 0 && (raw & 0x80000000u))) ? 2u : 1u;

            for (unsigned inst = 0; inst < instances; ++inst) {
                ChipEntry e;
                e.type = type;
                e.instance = static_cast<std::uint8_t>(inst);
                e.clockHz = clock;
                if (inst == 1) {
                    for (const XhClock& x : xhClocks)
                        if (x.type == idx) { e.clockHz = x.clock & 0x3FFFFFFFu; break; }
                }
                e.flags = chipFlags(idx, h, raw);
                for (const XhVolume& v : xhVols)
                    if (v.type == idx && (v.flags & 1u) == inst) { e.volume = v.value; break; }
                if (e.clockHz == 0) {
                    warn(hi.warnings, WarningCode::Other, detail::kChipClockOfs[idx], "chip with zero clock ignored");
                    continue;
                }
                hi.chips.push_back(e);
            }
        }
        if (hi.chips.empty())
            warn(hi.warnings, WarningCode::Other, kNoOffset, "header lists no sound chips");

        return hi;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory while parsing VGM header");
    } catch (...) {
        return makeError(ErrorCode::Internal, "unexpected exception in parseVgmHeader");
    }
}

} // namespace vgmplay::format
