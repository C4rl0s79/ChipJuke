// vgmplay - modul format: validateCommandStream (ADR 0002 pkt 8.8-8.10, 8.11; hazards H1-H3).
//
// Pre-skan odtwarza to, co zrobi VGMPlayer::ParseFile/Cmd_DataBlock z libvgm (c5c03a2), ale bez
// wykonywania komend: wyznacza granice komend, sprawdza bloki danych i sume opoznien.
// Decyzje "odrzuc" (zamiast ostrzezenia) tam, gdzie libvgm NIE ma obrony i skutkiem bylby blad
// pamieci albo wykonanie danych jako komend:
//  * blok 0x00..0x7F lezacy w/za loopOfs (libvgm w 2. petli pomija go BEZ przeskoczenia payloadu),
//  * blok PCM dopisany do banku, na ktory wskazuje juz strumien DAC (0x91; wiszacy wskaznik),
//  * blok skompresowany z nieznanym typem / za krotki / bez zgodnej tabeli (niezainicjowane dane),
//  * zapis ROM/RAM krotszy niz naglowek (libvgm wraca bez przeskoczenia payloadu),
//  * komenda "invalid" (libvgm konczy na niej odtwarzanie), uciety operand, loopOfs nie na granicy komendy.
#include <algorithm>
#include <array>
#include <bitset>
#include <new>

#include "cmdtable.hpp"
#include "internal.hpp"
#include "vgmplay/format.hpp"

namespace vgmplay::format {
namespace {

using detail::kCmdTable;
using detail::rd16;
using detail::rd32;

constexpr std::size_t kMaxWarnings = 256;
constexpr std::size_t kBankCount = 0x40;   // VGMPlayer::_PCM_BANK_COUNT

struct ComprTable {
    bool valid = false;
    std::uint8_t bitsDec = 0;
    std::uint8_t bitsCmp = 0;
    std::uint16_t valueCount = 0;
};

[[nodiscard]] constexpr bool isSecondChipCmd(std::uint8_t cmd) noexcept
{
    return cmd == 0x30 || cmd == 0x3F || (cmd >= 0xA1 && cmd <= 0xAF);
}

class Scanner {
public:
    Scanner(std::span<const std::uint8_t> file, const HeaderInfo& h, const Limits& l) : f_(file), h_(h), lim_(l) {}
    Result<StreamReport> run();

private:
    void warn(WarningCode c, std::uint64_t ofs, std::string msg)
    {
        if (rep_.warnings.size() < kMaxWarnings) rep_.warnings.push_back(Warning{c, ofs, std::move(msg)});
    }
    [[nodiscard]] const std::uint8_t* at(std::uint64_t p) const noexcept
    {
        return f_.data() + static_cast<std::size_t>(p);
    }
    [[nodiscard]] Result<void> dataBlock(std::uint64_t pos);
    [[nodiscard]] Result<void> comprHeader(std::uint64_t pos, std::uint8_t type, const std::uint8_t* d,
                                           std::uint64_t dblkLen, std::uint64_t& decmpLen);
    void checkChip(std::uint8_t cmd, std::uint64_t pos);
    [[nodiscard]] Result<void> addPcm(std::uint64_t pos, std::size_t bank, std::uint64_t dataLen);

    std::span<const std::uint8_t> f_;
    const HeaderInfo& h_;
    const Limits& lim_;
    StreamReport rep_;
    std::uint64_t end_ = 0;

    std::array<std::uint8_t, 48> chipInstances_{};
    std::bitset<48 * 2> chipWarned_;
    std::bitset<256> cmdWarned_;
    std::array<std::uint64_t, kBankCount> bankBytes_{};
    std::bitset<kBankCount> bankPinned_;
    std::bitset<256> streamSetup_;
    ComprTable table_;
    std::array<std::array<std::uint64_t, 2>, kBankCount> romSize_{};
    std::uint64_t ticks_ = 0;
};

Result<void> Scanner::addPcm(std::uint64_t pos, std::size_t bank, std::uint64_t dataLen)
{
    if (dataLen > lim_.maxSingleBlockBytes)
        return makeError(ErrorCode::LimitExceeded, "data block larger than per-block limit", pos);
    if (bankPinned_.test(bank) && dataLen != 0)
        return makeError(ErrorCode::BadDataBlock,
                         "data block appended to a bank already referenced by a DAC stream (0x91)", pos);
    bankBytes_[bank] += dataLen;
    rep_.pcmBytes += dataLen;
    if (bankBytes_[bank] > 0xFFFFFFFFull || rep_.pcmBytes + rep_.romWriteBytes > lim_.maxPcmBudgetBytes)
        return makeError(ErrorCode::LimitExceeded, "PCM/ROM memory budget exceeded", pos);
    return {};
}

// Naglowek bloku skompresowanego (0x40..0x7E): [0]=typ [1..4]=decmpLen [5]=bitsDec [6]=bitsCmp
// [7]=subType [8..9]=baseVal; payload od 10. Odpowiada ReadComprDataBlkHdr + DecompressDataBlk.
Result<void> Scanner::comprHeader(std::uint64_t pos, std::uint8_t type, const std::uint8_t* d,
                                  std::uint64_t dblkLen, std::uint64_t& decmpLen)
{
    (void)type;
    if (dblkLen < 10)
        return makeError(ErrorCode::BadDataBlock, "compressed data block shorter than its header", pos);
    const std::uint8_t comprType = d[0];
    decmpLen = rd32(d + 1);
    const std::uint8_t bitsDec = d[5];
    const std::uint8_t bitsCmp = d[6];
    const std::uint8_t subType = d[7];
    if (comprType > 1)
        return makeError(ErrorCode::BadDataBlock, "unknown data block compression type", pos);
    if (decmpLen > lim_.maxSingleBlockBytes)
        return makeError(ErrorCode::LimitExceeded, "decompressed block larger than per-block limit", pos);
    if (bitsCmp < 1 || bitsCmp > 16 || bitsDec < 1 || bitsDec > 16 || bitsCmp > bitsDec)
        return makeError(ErrorCode::BadDataBlock, "invalid bit widths in compressed data block", pos);
    const unsigned valSize = (bitsDec + 7u) / 8u;
    // Dekompresor 16-bitowy zapisuje po 2 bajty - nieparzysta dlugosc = zapis o 1 bajt za bufor.
    if (valSize == 2 && (decmpLen & 1u))
        return makeError(ErrorCode::BadDataBlock, "odd decompressed length for 16-bit values", pos);
    if (decmpLen == 0 && dblkLen > 10)
        return makeError(ErrorCode::BadDataBlock,
                         "empty compressed block with payload (libvgm would execute the payload as commands)", pos);

    const bool needTable = comprType == 1 || subType == 2;
    if (needTable) {
        if (!table_.valid || table_.valueCount == 0)
            return makeError(ErrorCode::BadDataBlock, "table-compressed block without a preceding value table", pos);
        if (table_.bitsDec != bitsDec || table_.bitsCmp != bitsCmp)
            return makeError(ErrorCode::BadDataBlock, "value table incompatible with compressed block", pos);
        if (static_cast<std::uint64_t>(table_.valueCount) < (1ull << bitsCmp))
            return makeError(ErrorCode::BadDataBlock, "value table smaller than 2^bitsCmp entries", pos);
    }
    return {};
}

Result<void> Scanner::dataBlock(std::uint64_t pos)
{
    // 0x67 0x66 tt ss ss ss ss <dane>; libvgm nie sprawdza bajtu zgodnosci 0x66.
    if (pos + 7 > end_) return makeError(ErrorCode::Truncated, "data block header cut by end of data", pos);
    const std::uint8_t type = *at(pos + 2);
    const std::uint32_t rawLen = rd32(at(pos + 3));
    const unsigned chipId = (rawLen & 0x80000000u) ? 1u : 0u;
    const std::uint64_t dblkLen = rawLen & 0x7FFFFFFFu;
    if (dblkLen > end_ - (pos + 7))
        return makeError(ErrorCode::BadDataBlock, "data block extends beyond end of command data", pos);
    const std::uint8_t* d = at(pos + 7);

    switch (type & 0xC0) {
    case 0x00:
    case 0x40: {
        if (h_.hasLoop() && pos >= h_.loopOffset)
            return makeError(ErrorCode::BadDataBlock,
                             "PCM data block at/after the loop start (libvgm skips it on loops > 0 without skipping its payload)",
                             pos);
        if (type == 0x7F) {
            if (dblkLen < 6) return makeError(ErrorCode::BadDataBlock, "value table block shorter than 6 bytes", pos);
            const std::uint8_t bitsDec = d[2];
            const std::uint8_t bitsCmp = d[3];
            const std::uint16_t count = rd16(d + 4);
            if (bitsDec < 1 || bitsDec > 16 || bitsCmp < 1 || bitsCmp > 16)
                return makeError(ErrorCode::BadDataBlock, "invalid bit widths in value table", pos);
            const std::uint64_t valSize = (bitsDec + 7u) / 8u;
            if (6 + static_cast<std::uint64_t>(count) * valSize > dblkLen)
                return makeError(ErrorCode::BadDataBlock, "value table longer than its data block", pos);
            table_ = ComprTable{true, bitsDec, bitsCmp, count};
            break;
        }
        const std::size_t bank = type & 0x3F;
        std::uint64_t dataLen = dblkLen;
        if (type & 0x40) {
            if (auto r = comprHeader(pos, type, d, dblkLen, dataLen); !r) return r;
        } else if (dblkLen > lim_.maxSingleBlockBytes) {
            return makeError(ErrorCode::LimitExceeded, "data block larger than per-block limit", pos);
        }
        if (auto r = addPcm(pos, bank, dataLen); !r) return r;
        break;
    }
    case 0x80: {
        // Zapis ROM: [0..3]=memSize [4..7]=dataOfs, reszta = dane.
        if (dblkLen < 8) return makeError(ErrorCode::BadDataBlock, "ROM write block shorter than 8 bytes", pos);
        const std::uint64_t memSize = rd32(d);
        const std::uint64_t dataOfs = rd32(d + 4);
        const std::uint64_t dataLen = dblkLen - 8;
        if (memSize > lim_.maxSingleBlockBytes)
            return makeError(ErrorCode::LimitExceeded, "ROM size larger than per-block limit", pos);
        if (dataOfs + dataLen > memSize)
            return makeError(ErrorCode::BadDataBlock, "ROM write exceeds the declared ROM size", pos);
        std::uint64_t& slot = romSize_[type & 0x3F][chipId];
        if (memSize > slot) {
            rep_.romWriteBytes += memSize - slot;     // liczymy najwiekszy rozmiar na (typ, uklad)
            slot = memSize;
        }
        if (rep_.pcmBytes + rep_.romWriteBytes > lim_.maxPcmBudgetBytes)
            return makeError(ErrorCode::LimitExceeded, "PCM/ROM memory budget exceeded", pos);
        break;
    }
    default: {
        // Zapis RAM (0xC0..0xFF): 16-bitowy (C0..DF) albo 32-bitowy (E0..FF) adres.
        const std::uint64_t hdr = (type & 0x20) ? 4 : 2;
        if (dblkLen < hdr) return makeError(ErrorCode::BadDataBlock, "RAM write block shorter than its address field", pos);
        if (dblkLen - hdr > lim_.maxSingleBlockBytes)
            return makeError(ErrorCode::LimitExceeded, "RAM write larger than per-block limit", pos);
        break;
    }
    }
    return {};
}

void Scanner::checkChip(std::uint8_t cmd, std::uint64_t pos)
{
    const std::uint8_t chip = kCmdTable[cmd].chip;
    if (chip >= chipInstances_.size()) return;
    const unsigned inst = isSecondChipCmd(cmd) ? 1u : 0u;
    if (chipInstances_[chip] > inst) return;
    const std::size_t key = static_cast<std::size_t>(chip) * 2 + inst;
    if (chipWarned_.test(key)) return;
    chipWarned_.set(key);
    warn(WarningCode::ChipNotInHeader, pos, "command addresses a chip with no clock in the header (ignored by libvgm)");
}

Result<StreamReport> Scanner::run()
{
    if (f_.size() != h_.fileSize)
        return makeError(ErrorCode::InvalidArgument, "header.fileSize does not match the buffer size");
    if (h_.dataEnd > f_.size() || h_.dataOffset >= h_.dataEnd)
        return makeError(ErrorCode::InvalidArgument, "inconsistent header data range");
    end_ = h_.dataEnd;

    for (const ChipEntry& c : h_.chips) {
        const std::size_t idx = detail::chipIndexFromType(c.type);
        if (idx >= chipInstances_.size()) continue;
        chipInstances_[idx] = static_cast<std::uint8_t>(chipInstances_[idx] + 1);
        if (idx == 0 && (c.flags & 0x80000000u)) chipInstances_[idx] = 2;   // T6W28: druga polowka = komendy 0x30/0x3F
    }

    bool loopPending = h_.hasLoop();
    std::uint64_t pos = h_.dataOffset;
    while (pos < end_) {
        if (loopPending) {
            if (pos == h_.loopOffset) {
                rep_.loopStartSample = ticks_;
                loopPending = false;
            } else if (pos > h_.loopOffset) {
                return makeError(ErrorCode::BadHeader, "loop offset is not at a command boundary", h_.loopOffset);
            }
        }
        if (++rep_.commandCount > lim_.maxCommands)
            return makeError(ErrorCode::LimitExceeded, "too many commands", pos);

        const std::uint8_t cmd = *at(pos);
        const detail::CmdInfo& ci = kCmdTable[cmd];

        if (ci.kind == 'E') {
            rep_.hasEndCommand = true;
            break;
        }
        if (ci.kind == 'B') {
            if (auto r = dataBlock(pos); !r) return r.error();
            pos += 7 + (rd32(at(pos + 3)) & 0x7FFFFFFFu);
            continue;
        }
        if (ci.kind == 'I')
            return makeError(ErrorCode::BadCommandStream, "invalid command byte (libvgm stops playback here)", pos);
        if (ci.len > end_ - pos)
            return makeError(ErrorCode::Truncated, "command operands cut by end of data", pos);

        if (ci.kind == 'U') {
            if (!cmdWarned_.test(cmd)) {
                cmdWarned_.set(cmd);
                warn(WarningCode::UnknownCommand, pos, "unknown command skipped");
            }
        }
        checkChip(cmd, pos);

        std::uint64_t delay = 0;
        if (cmd == 0x61) delay = rd16(at(pos + 1));
        else if (cmd == 0x62) delay = 735;
        else if (cmd == 0x63) delay = 882;
        else if (cmd >= 0x70 && cmd <= 0x7F) delay = (cmd & 0x0Fu) + 1u;
        else if (cmd >= 0x80 && cmd <= 0x8F) delay = cmd & 0x0Fu;
        else if (cmd == 0x90) {
            if (*at(pos + 1) != 0xFF) streamSetup_.set(*at(pos + 1));
        } else if (cmd == 0x91) {
            const std::uint8_t stream = *at(pos + 1);
            const std::uint8_t bank = *at(pos + 2);
            // libvgm: tylko zainicjowany strumien i bank < 0x40; wskaznik NULL przy pustym banku jest bezpieczny.
            if (streamSetup_.test(stream) && bank < kBankCount && bankBytes_[bank] != 0) bankPinned_.set(bank);
        }
        ticks_ += delay;
        if (ticks_ > lim_.maxTotalSamples)
            return makeError(ErrorCode::LimitExceeded, "stream length exceeds sanity limit", pos);

        pos += ci.len;
    }

    if (loopPending)
        return makeError(ErrorCode::BadHeader, "loop offset lies beyond the end-of-data command", h_.loopOffset);

    rep_.totalSamples = ticks_;
    if (!rep_.hasEndCommand)
        warn(WarningCode::EndsEarly, end_, "no end-of-data command (0x66) before end of command data");
    return std::move(rep_);
}

} // namespace

Result<StreamReport> validateCommandStream(std::span<const std::uint8_t> file, const HeaderInfo& header,
                                           const Limits& limits)
{
    try {
        Scanner s(file, header, limits);
        return s.run();
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory while scanning command stream");
    } catch (...) {
        return makeError(ErrorCode::Internal, "unexpected exception in validateCommandStream");
    }
}

} // namespace vgmplay::format
