// vgmplay - modul chipstate: buildPattern (kontrakt: include/vgmplay/chipstate.hpp).
//
// Jedno przejscie po strumieniu komend VGM od dataOffset: czas w tickach 44100 z czekan, wiersz = tick / rowTicks,
// komendy zapisu rejestrow trafiaja do dekoderow ukladow (STAN rejestrow, nie dzwiek), ktore zglaszaja nuty/
// glosnosci do Recordera. Zlozonosc O(rozmiar strumienia + liczba wierszy * liczba kanalow).
// Strumienie DAC (0x90..0x95) sa sledzone tylko dla YM2612 (kanal "DAC": nuta C-4 na czas odtwarzania probki).
#include <array>
#include <new>
#include <unordered_map>

#include "chip_layout.hpp"
#include "cmdtable.hpp"   // src/format (tabela dlugosci komend, ta sama co w pre-skanie)
#include "cs_internal.hpp"

namespace vgmplay::chipstate {
namespace {

using namespace detail;
using format::detail::kCmdTable;

[[nodiscard]] inline std::uint32_t rd32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
[[nodiscard]] inline std::uint32_t rd16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
}

std::unique_ptr<ChipDecoder> makeDecoderFor(Host& h, const format::ChipEntry& e, std::size_t first)
{
    switch (e.type) {
    case ChipType::SN76496: return makeSn76489(h, first, e.clockHz, e.flags);
    case ChipType::YM2413:  return makeYm2413(h, first, e.clockHz);
    case ChipType::YM2612:  return makeYm2612(h, first, e.clockHz);
    case ChipType::YM2151:  return makeYm2151(h, first, e.clockHz);
    case ChipType::YM2203:  return makeYm2203(h, first, e.clockHz);
    case ChipType::YM2608:  return makeYm2608(h, first, e.clockHz);
    case ChipType::YM2610:  return makeYm2610(h, first, e.clockHz, (e.flags & 0x80000000u) != 0);
    case ChipType::YM3812:
    case ChipType::YM3526:  return makeOpl(h, first, e.clockHz, false);
    case ChipType::YMF262:  return makeOpl(h, first, e.clockHz, true);
    case ChipType::AY8910:  return makeAy8910(h, first, e.clockHz);
    case ChipType::OKIM6295: return makeOkim6295(h, first, e.clockHz, e.flags);
    case ChipType::NesApu:  return makeNesApu(h, first, e.clockHz, e.flags);
    case ChipType::C140:    return makeC140(h, first, e.clockHz);
    case ChipType::K054539: return makeK054539(h, first, e.clockHz, e.flags);
    case ChipType::K053260: return makeK053260(h, first, e.clockHz);
    case ChipType::SegaPcm: return makeSegaPcm(h, first, e.clockHz);
    case ChipType::GA20:    return makeGa20(h, first, e.clockHz);
    case ChipType::OKIM6258: return makeOkim6258(h, first, e.clockHz, e.flags);
    default: return nullptr;
    }
}

struct DacStream {
    bool valid = false;          // 0x90 skierowane do istniejacego YM2612 / OKIM6258
    ChipType chip = ChipType::YM2612;
    std::uint8_t inst = 0;       // instancja ukladu
    std::uint8_t bank = 0xFF;
    std::uint8_t step = 1;
    std::uint32_t freq = 0;
};

class Walker {
public:
    Walker(std::span<const std::uint8_t> raw, const format::HeaderInfo& header, Host& host)
        : raw_(raw), header_(header), host_(host)
    {
    }

    void addDecoder(ChipType type, std::uint8_t inst, std::unique_ptr<ChipDecoder> d)
    {
        if (!d || inst > 1) return;
        const std::size_t t = static_cast<std::size_t>(type);
        if (t >= slot_.size()) return;
        all_.push_back(d.get());
        slot_[t][inst] = std::move(d);
    }

    // Zwraca false przy limicie wierszy.
    bool run()
    {
        std::uint64_t pos = header_.dataOffset;
        const std::uint64_t end = std::min<std::uint64_t>(header_.dataEnd, raw_.size());
        while (pos < end) {
            const std::uint8_t op = raw_[static_cast<std::size_t>(pos)];
            const auto& ci = kCmdTable[op];
            if (ci.kind == 'E' || ci.kind == 'I') break;
            if (ci.kind == 'B') {
                if (end - pos < 7) break;
                const std::uint64_t size = rd32(&raw_[static_cast<std::size_t>(pos) + 3]) & 0x7FFFFFFFu;
                if (size > end - (pos + 7)) break;
                dataBlock(pos, size);
                pos += 7 + size;
                continue;
            }
            if (ci.len == 0 || ci.len > end - pos) break;
            if (!command(op, &raw_[static_cast<std::size_t>(pos)])) return false;
            pos += ci.len;
        }
        return true;
    }

    [[nodiscard]] std::uint64_t now() const noexcept { return host_.now; }

private:
    ChipDecoder* find(ChipType type, unsigned inst) const noexcept
    {
        const std::size_t t = static_cast<std::size_t>(type);
        return (t < slot_.size() && inst < 2) ? slot_[t][inst].get() : nullptr;
    }
    void write(ChipType type, unsigned inst, std::uint8_t port, std::uint8_t reg, std::uint8_t val)
    {
        if (ChipDecoder* d = find(type, inst)) d->write(port, reg, val);
    }
    void writeMem(ChipType type, unsigned inst, std::uint32_t addr, std::uint8_t val)
    {
        if (ChipDecoder* d = find(type, inst)) d->writeMem(addr, val);
    }

    // Przesuwa czas o `ticks`, po drodze obslugujac terminy dekoderow (koniec probki, cisza DAC).
    bool advance(std::uint64_t ticks)
    {
        const std::uint64_t target = host_.now + ticks;
        for (int guard = 0; guard < 4096; ++guard) {
            std::uint64_t best = kNever;
            ChipDecoder* who = nullptr;
            for (ChipDecoder* d : all_) {
                const std::uint64_t dl = d->deadline();
                if (dl < best) { best = dl; who = d; }
            }
            if (!who || best > target) break;
            host_.now = std::max(best, host_.now);
            if (!host_.rec.advanceTo(host_.now)) return false;
            who->onDeadline(host_.now);
            if (who->deadline() <= host_.now) break;   // brak postepu (zabezpieczenie)
        }
        host_.now = target;
        return host_.rec.advanceTo(target);
    }

    bool command(std::uint8_t op, const std::uint8_t* p)
    {
        switch (op) {
        case 0x50: write(ChipType::SN76496, 0, 0, 0, p[1]); break;
        case 0x30: write(ChipType::SN76496, 1, 0, 0, p[1]); break;
        case 0x51: write(ChipType::YM2413, 0, 0, p[1], p[2]); break;
        case 0xA1: write(ChipType::YM2413, 1, 0, p[1], p[2]); break;
        case 0x52: case 0x53: write(ChipType::YM2612, 0, static_cast<std::uint8_t>(op - 0x52), p[1], p[2]); break;
        case 0xA2: case 0xA3: write(ChipType::YM2612, 1, static_cast<std::uint8_t>(op - 0xA2), p[1], p[2]); break;
        case 0x54: write(ChipType::YM2151, 0, 0, p[1], p[2]); break;
        case 0xA4: write(ChipType::YM2151, 1, 0, p[1], p[2]); break;
        case 0x55: write(ChipType::YM2203, 0, 0, p[1], p[2]); break;
        case 0xA5: write(ChipType::YM2203, 1, 0, p[1], p[2]); break;
        case 0x56: case 0x57: write(ChipType::YM2608, 0, static_cast<std::uint8_t>(op - 0x56), p[1], p[2]); break;
        case 0xA6: case 0xA7: write(ChipType::YM2608, 1, static_cast<std::uint8_t>(op - 0xA6), p[1], p[2]); break;
        case 0x58: case 0x59: write(ChipType::YM2610, 0, static_cast<std::uint8_t>(op - 0x58), p[1], p[2]); break;
        case 0xA8: case 0xA9: write(ChipType::YM2610, 1, static_cast<std::uint8_t>(op - 0xA8), p[1], p[2]); break;
        case 0x5A: write(ChipType::YM3812, 0, 0, p[1], p[2]); break;
        case 0xAA: write(ChipType::YM3812, 1, 0, p[1], p[2]); break;
        case 0x5B: write(ChipType::YM3526, 0, 0, p[1], p[2]); break;
        case 0xAB: write(ChipType::YM3526, 1, 0, p[1], p[2]); break;
        case 0x5E: case 0x5F: write(ChipType::YMF262, 0, static_cast<std::uint8_t>(op - 0x5E), p[1], p[2]); break;
        case 0xAE: case 0xAF: write(ChipType::YMF262, 1, static_cast<std::uint8_t>(op - 0xAE), p[1], p[2]); break;
        case 0xA0: write(ChipType::AY8910, p[1] >> 7, 0, p[1] & 0x7F, p[2]); break;
        case 0xB8: write(ChipType::OKIM6295, p[1] >> 7, 0, p[1] & 0x7F, p[2]); break;
        case 0xB4: {   // NES APU / FDS: mapowanie rejestrow jak w libvgm (Cmd_NES_Reg)
            std::uint8_t ofs = p[1] & 0x7F;
            if (ofs == 0x3F) ofs = 0x23;
            else if ((ofs & 0xE0) == 0x20) ofs = static_cast<std::uint8_t>(0x80 | (ofs & 0x1F));
            write(ChipType::NesApu, p[1] >> 7, 0, ofs, p[2]);
            break;
        }
        case 0xB7: write(ChipType::OKIM6258, p[1] >> 7, 0, p[1] & 0x7F, p[2]); break;
        case 0xBA: write(ChipType::K053260, p[1] >> 7, 0, p[1] & 0x7F, p[2]); break;
        case 0xBF: write(ChipType::GA20, p[1] >> 7, 0, p[1] & 0x7F, p[2]); break;
        case 0xC0: writeMem(ChipType::SegaPcm, p[2] >> 7, (p[1] | (p[2] << 8)) & 0x7FFFu, p[3]); break;
        case 0xD3: writeMem(ChipType::K054539, p[1] >> 7, ((p[1] << 8) | p[2]) & 0x7FFFu, p[3]); break;
        case 0xD4: writeMem(ChipType::C140, p[1] >> 7, ((p[1] << 8) | p[2]) & 0x7FFFu, p[3]); break;
        case 0x61: return advance(rd16(p + 1));
        case 0x62: return advance(735);
        case 0x63: return advance(882);
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: dacCommand(op, p); break;
        default:
            if (op >= 0x70 && op <= 0x7F) return advance((op & 0x0Fu) + 1u);
            if (op >= 0x80 && op <= 0x8F) {   // zapis probki z banku PCM do DAC YM2612 + czekanie
                if (ChipDecoder* d = find(ChipType::YM2612, 0)) d->dacData();
                return advance(op & 0x0Fu);
            }
            break;
        }
        return true;
    }

    void dataBlock(std::uint64_t pos, std::uint64_t size)
    {
        const std::uint8_t* b = &raw_[static_cast<std::size_t>(pos)];
        const std::uint8_t type = b[2];
        const unsigned inst = (rd32(b + 3) & 0x80000000u) ? 1u : 0u;
        const std::uint8_t* d = b + 7;
        if (type < 0x80) {
            if (type == 0x7F) return;   // tablica wartosci kompresji
            std::uint64_t len = size;
            if (type & 0x40) {          // skompresowany: rozmiar po dekompresji z naglowka bloku
                if (size < 5) return;
                len = rd32(d + 1);
            }
            const unsigned bank = type & 0x3F;
            if (blockSizes_[bank].size() < 65536) blockSizes_[bank].push_back(static_cast<std::uint32_t>(len));
            bankTotal_[bank] += len;
        } else if (size >= 8) {   // ROM ukladu: [memSize][startAddr][dane]
            ChipType chip = ChipType::Unknown;
            switch (type) {
            case 0x80: chip = ChipType::SegaPcm; break;
            case 0x8B: chip = ChipType::OKIM6295; break;
            case 0x8C: chip = ChipType::K054539; break;
            case 0x8D: chip = ChipType::C140; break;
            case 0x8E: chip = ChipType::K053260; break;
            case 0x93: chip = ChipType::GA20; break;
            default: break;
            }
            if (chip != ChipType::Unknown)
                if (ChipDecoder* o = find(chip, inst))
                    o->romBlock(type, rd32(d), rd32(d + 4),
                                std::span<const std::uint8_t>(d + 8, static_cast<std::size_t>(size - 8)));
        }
    }

    // Czas trwania w tickach 44100 dla `cmds` probek przy czestotliwosci freq [Hz]; kNever gdy nieznany.
    [[nodiscard]] std::uint64_t endAfter(std::uint64_t cmds, std::uint32_t freq) const
    {
        if (freq == 0) return kNever;
        return host_.now + cmds * 44100u / freq;
    }

    void dacCommand(std::uint8_t op, const std::uint8_t* p)
    {
        DacStream& s = streams_[p[1]];
        switch (op) {
        case 0x90: {
            if (p[1] == 0xFF) return;
            const unsigned type = p[2] & 0x7Fu, inst = p[2] >> 7;
            s = DacStream{};
            // Uklad docelowy strumienia: 0x02 = YM2612, 0x17 = OKIM6258 (indeksy ukladow jak w kCmdTable).
            const ChipType target =
                type == 0x02 ? ChipType::YM2612 : (type == 0x17 ? ChipType::OKIM6258 : ChipType::Unknown);
            if (target != ChipType::Unknown && find(target, inst)) {
                s.valid = true;
                s.chip = target;
                s.inst = static_cast<std::uint8_t>(inst);
            }
            break;
        }
        case 0x91:
            if (!s.valid) return;
            s.bank = p[2];
            s.step = p[3] ? p[3] : 1;
            break;
        case 0x92:
            if (s.valid) s.freq = rd32(p + 2);
            break;
        case 0x93: {
            if (!s.valid) return;
            const std::int32_t start = static_cast<std::int32_t>(rd32(p + 2));
            const std::uint8_t mode = p[6];
            const std::uint32_t len = rd32(p + 7);
            const std::uint8_t dataStep = s.step;   // cmdSize YM2612 = 1
            std::uint64_t end = kNever;
            switch (mode & 0x0F) {
            case 1: end = endAfter(len, s.freq); break;
            case 2: if (s.freq) end = endAfter(1000ull * len / s.freq, s.freq); break;
            case 3:
                if (dataStep && s.bank < 0x40 && start >= 0 && bankTotal_[s.bank] > static_cast<std::uint64_t>(start))
                    end = endAfter((bankTotal_[s.bank] - static_cast<std::uint64_t>(start)) / dataStep, s.freq);
                break;
            case 0x0F: if (dataStep) end = endAfter(len / dataStep, s.freq); break;
            default: break;
            }
            if (mode & 0x80) end = kNever;   // petla: do stopu
            std::uint8_t inst = 0;
            if (start >= 0) {
                auto it = startIds_.find(static_cast<std::uint32_t>(start));
                if (it == startIds_.end())
                    it = startIds_.emplace(static_cast<std::uint32_t>(start),
                                           static_cast<std::uint8_t>(std::min<std::size_t>(startIds_.size(), 254))).first;
                inst = it->second;
            }
            if (ChipDecoder* d = find(s.chip, s.inst)) d->dacStreamStart(inst, end);
            break;
        }
        case 0x94:
            if (p[1] == 0xFF) {
                for (DacStream& t : streams_)
                    if (t.valid)
                        if (ChipDecoder* d = find(t.chip, t.inst)) d->dacStreamStop();
            } else if (s.valid) {
                if (ChipDecoder* d = find(s.chip, s.inst)) d->dacStreamStop();
            }
            break;
        case 0x95: {
            if (!s.valid) return;
            const unsigned block = rd16(p + 2);
            std::uint64_t end = kNever;
            if (s.bank < 0x40 && block < blockSizes_[s.bank].size() && s.step)
                end = endAfter(blockSizes_[s.bank][block] / s.step, s.freq);
            if (p[4] & 1) end = kNever;
            if (ChipDecoder* d = find(s.chip, s.inst))
                d->dacStreamStart(static_cast<std::uint8_t>(std::min(block, 254u)), end);
            break;
        }
        default: break;
        }
    }

    std::span<const std::uint8_t> raw_;
    const format::HeaderInfo& header_;
    Host& host_;
    std::array<std::array<std::unique_ptr<ChipDecoder>, 2>, static_cast<std::size_t>(ChipType::Count_)> slot_;
    std::vector<ChipDecoder*> all_;
    std::array<DacStream, 256> streams_{};
    std::array<std::vector<std::uint32_t>, 0x40> blockSizes_{};
    std::array<std::uint64_t, 0x40> bankTotal_{};
    std::unordered_map<std::uint32_t, std::uint8_t> startIds_;
};

} // namespace

Result<Pattern> buildPattern(std::span<const std::uint8_t> raw, const format::HeaderInfo& header,
                             const format::StreamReport& stream, const PatternOptions& options)
{
    try {
        if (header.dataOffset >= header.dataEnd || header.dataEnd > raw.size())
            return makeError(ErrorCode::Internal, "buildPattern: inconsistent header data range");

        Pattern pat;
        pat.rowTicks = options.rowTicks == 0 ? 735u : options.rowTicks;

        // Kanaly w kolejnosci naglowka (== kolejnosc urzadzen silnika), potem dekodery.
        struct Pending { format::ChipEntry e; std::size_t first; };
        std::vector<Pending> chips;
        for (const format::ChipEntry& e : header.chips) {
            std::vector<PatternChannel> ch = detail::channelsFor(e.type, e.instance, e.flags);
            if (ch.empty()) continue;
            chips.push_back({e, pat.channels.size()});
            for (PatternChannel& c : ch) pat.channels.push_back(std::move(c));
        }

        const std::uint64_t maxRows = options.maxRows == 0 ? 1u : options.maxRows;
        Host host(pat.channels.size(), pat.rowTicks, maxRows);
        Walker walker(raw, header, host);
        for (const Pending& c : chips) walker.addDecoder(c.e.type, c.e.instance, makeDecoderFor(host, c.e, c.first));

        walker.run();

        const std::uint64_t wanted = std::max<std::uint64_t>(stream.totalSamples, walker.now()) / pat.rowTicks + 1;
        pat.cells = host.rec.finish(wanted, pat.rowCount);
        if (host.rec.truncated())
            pat.warnings.push_back(Warning{WarningCode::LimitReached, kNoOffset, "pattern truncated to maxRows"});
        if (header.hasLoop() && !stream.loopDisabled)
            pat.loopStartRow = std::min<std::uint64_t>(stream.loopStartSample / pat.rowTicks, pat.rowCount - 1);
        return pat;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "buildPattern: out of memory");
    } catch (...) {
        return makeError(ErrorCode::Internal, "buildPattern: unexpected exception");
    }
}

} // namespace vgmplay::chipstate
