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
//
// Walidacje konfiguracji ukladow (odpowiedniki VGMPlayer::GenerateDeviceConfig; ADR 0002 pkt 8.15-8.16):
//  * C352 (0xD6 == 0)  -> BadHeader: libvgm liczy clock*72/hdr[0xD6] bez ochrony (#DE, vgmplayer.cpp:1374);
//  * SN76496 0x2A > 32, SegaPCM 0x3C > 31 -> BadHeader (przesuniecia o >= 32 bity to UB w rdzeniach);
//  * zegar PO przeliczeniach libvgm (UINT32 z zawinieciem, jak w libvgm) == 0 -> BadHeader;
//  * zegar / natywny rate: Limits::maxChipClockHz, maxChipSampleRate, maxTotalChipSampleRate -> LimitExceeded
//    (Resmpl_Init alokuje 1 s probek ukladu, emu/Resampler.c:103, abort() przy OOM).
#include <algorithm>
#include <cstring>
#include <new>
#include <optional>
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

// Dzielnik czestotliwosci probek rdzenia wzgledem zegara PO przeliczeniach libvgm (rate = clock / div).
// Wartosc to NAJMNIEJSZY dzielnik sposrod rdzeni wlaczonych w libvgm (konserwatywnie: najwyzszy mozliwy rate),
// odczytany z emu/cores/*.c (INIT_DEVINF / device_start_*). 0 = rate niezalezny od zegara albo brak rdzenia
// (32X PWM: stale 22020 Hz; ES5506: rdzen jest szkieletem bez emulacji - SndEmu_Start2 nie wystartuje).
constexpr std::uint32_t kRateDivisor[detail::kVgmChipCount] = {
    /*00 SN76496*/ 2,    /*01 YM2413*/ 72,   /*02 YM2612*/ 144,  /*03 YM2151*/ 64,
    /*04 SegaPCM*/ 128,  /*05 RF5C68*/ 384,  /*06 YM2203*/ 72,   /*07 YM2608*/ 144,
    /*08 YM2610*/ 144,   /*09 YM3812*/ 72,   /*0A YM3526*/ 72,   /*0B Y8950*/ 72,
    /*0C YMF262*/ 288,   /*0D YMF278B*/ 768, /*0E YMF271*/ 384,  /*0F YMZ280B*/ 192,
    /*10 RF5C164*/ 384,  /*11 32X PWM*/ 0,   /*12 AY8910*/ 8,    /*13 GB DMG*/ 32,
    /*14 NES APU*/ 4,    /*15 MultiPCM*/ 224, /*16 uPD7759*/ 4,  /*17 OKIM6258*/ 512,
    /*18 OKIM6295*/ 132, /*19 K051649*/ 16,  /*1A K054539*/ 384, /*1B HuC6280*/ 16,
    /*1C C140/C219*/ 288, /*1D K053260*/ 64, /*1E Pokey*/ 1,     /*1F QSound*/ 2496,
    /*20 SCSP*/ 512,     /*21 WSwan*/ 128,   /*22 VSU*/ 120,     /*23 SAA1099*/ 128,
    /*24 ES5503*/ 16,    /*25 ES5506*/ 0,    /*26 X1-010*/ 512,  /*27 C352*/ 288,
    /*28 GA20*/ 4,       /*29 Mikey*/ 16,    /*2A K007232*/ 128, /*2B K005289*/ 1,
    /*2C MSM5205*/ 1,    /*2D MSM5232*/ 16,  /*2E BSMT2000*/ 772, /*2F ICS2115*/ 32,
};

// Zegar przekazywany przez libvgm do rdzenia (VGMPlayer::GenerateDeviceConfig, c5c03a2). Arytmetyka UINT32
// Z ZAWINIECIEM - dokladnie jak w libvgm (np. clock * 224 przy MultiPCM). Dla C352 wymaga hdr[0xD6] != 0.
[[nodiscard]] std::uint32_t libvgmEffectiveClock(std::size_t idx, std::uint32_t clock, const detail::PaddedHeader& h)
{
    switch (idx) {
    case 0x15: return static_cast<std::uint32_t>(clock * 224u) / 180u;          // MultiPCM (YMW258)
    case 0x1A: return clock < 1000000u ? clock * 384u : clock;                 // K054539
    case 0x1C:                                                                 // C140 / C219
        if (h.u8(0x96) == 2) {
            if (clock == 44100u) return 25056500u;
            return clock < 1000000u ? clock * 576u : clock;
        }
        if (clock == 21390u) return 12288000u;
        return clock < 1000000u ? clock * 576u : clock;
    case 0x1F: return clock < 5000000u ? clock * 15u : clock;                  // QSound
    case 0x20: return clock < 1000000u ? clock * 512u : clock;                 // SCSP
    case 0x27: return static_cast<std::uint32_t>(clock * 72u) / h.u8(0xD6);    // C352 (0xD6 != 0 sprawdzone wczesniej)
    default: return clock;
    }
}

struct ChipBudget {
    std::uint64_t totalRate = 0;
};

// Sprawdza konfiguracje jednej instancji ukladu; zwraca blad albo nic.
[[nodiscard]] std::optional<Error> checkChipConfig(std::size_t idx, std::uint32_t clock, const detail::PaddedHeader& h,
                                                   const Limits& lim, ChipBudget& budget)
{
    const std::uint64_t ofs = detail::kChipClockOfs[idx];
    if (idx == 0x00 && h.u8(0x2A) > 32)
        return makeError(ErrorCode::BadHeader, "SN76496 shift register width above 32 bits", 0x2A);
    if (idx == 0x04 && h.u8(0x3C) > 31)
        return makeError(ErrorCode::BadHeader, "SegaPCM bank shift above 31 bits", 0x3C);
    if (idx == 0x27 && h.u8(0xD6) == 0)
        return makeError(ErrorCode::BadHeader, "C352 clock divider (0xD6) is zero; libvgm divides by it", 0xD6);

    if (clock > lim.maxChipClockHz)
        return makeError(ErrorCode::LimitExceeded, "chip clock above limit", ofs);
    const std::uint32_t eff = libvgmEffectiveClock(idx, clock, h);
    if (eff == 0)
        return makeError(ErrorCode::BadHeader, "chip clock converts to zero in libvgm", ofs);
    if (eff > lim.maxChipClockHz)
        return makeError(ErrorCode::LimitExceeded, "effective chip clock above limit", ofs);
    const std::uint32_t div = kRateDivisor[idx];
    if (div != 0) {
        const std::uint32_t rate = eff / div;
        // Rdzenie libvgm (RC_SET_RATIO: GB DMG, Mikey, WonderSwan, ...) licza rate = clock / div i dziela przez nie.
        if (rate == 0)
            return makeError(ErrorCode::BadHeader, "chip clock too low: native sample rate would be zero", ofs);
        if (rate > lim.maxChipSampleRate)
            return makeError(ErrorCode::LimitExceeded, "chip native sample rate above limit", ofs);
        budget.totalRate += rate;
        if (budget.totalRate > lim.maxTotalChipSampleRate)
            return makeError(ErrorCode::LimitExceeded, "sum of chip native sample rates above limit", ofs);
    }
    return std::nullopt;
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
        hi.eofOffset = eofAbs;
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
                // libvgm ignoruje petle o zerowej dlugosci po cichu (vgmplayer.cpp:347) - bez ostrzezenia.
            } else {
                hi.loopOffset = abs;
            }
        }

        // --- uklady (pkt 11) ---
        std::uint32_t fm[3] = {0, 0, 0};
        const bool v101fix = hi.versionBcd < 0x110 && h.u32(0x10) != 0;
        if (v101fix) fmClockFix(file, dataOfs, dataEnd, h.u32(0x10), fm);

        ChipBudget budget;
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
                // libvgm tworzy urzadzenie takze przy zegarze 0 po zamaskowaniu bitow 30/31 (GetChipCount liczy
                // po surowym polu), wiec "pomijanie" takiego ukladu rozjezdzaloby liste ukladow z libvgm
                // (i stan strumieni DAC w pre-skanie). Plik z takim polem jest niepoprawny.
                if (e.clockHz == 0)
                    return makeError(ErrorCode::BadHeader, "chip instance with zero clock (flag bits only)",
                                     detail::kChipClockOfs[idx]);
                if (auto err = checkChipConfig(idx, e.clockHz, h, limits, budget)) return *err;
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

void applyLibvgmPatches(std::span<std::uint8_t> raw, const HeaderInfo& header, const StreamReport& stream) noexcept
{
    if (raw.size() < detail::kMinVgmHeader || raw.size() != header.fileSize) return;
    // EOF: libvgm porownuje eofOfs z rozmiarem bufora Z paddingiem (DataLoader_GetSize), a parser z rozmiarem bez
    // paddingu. Pole 0 / wskazujace w padding / za padding dalyby libvgm dataEnd w paddingu (zera = komendy 0x00).
    // Zapisujemy dokladnie ten eof, ktory uzyl parser.
    if (header.eofOffset >= 4 && header.eofOffset <= raw.size()) {
        const std::uint64_t rel = header.eofOffset - 4;
        for (unsigned i = 0; i < 4; ++i) raw[0x04 + i] = static_cast<std::uint8_t>(rel >> (8 * i));
    }
    // Blok PCM w/za punktem petli: libvgm w 2. przejsciu wykonalby jego dane jako komendy - zerujemy loopOfs
    // (0x1C) i loopSamples (0x20), plik gra raz.
    if (stream.loopDisabled)
        for (unsigned i = 0; i < 8; ++i) raw[0x1C + i] = 0;
}

} // namespace vgmplay::format
