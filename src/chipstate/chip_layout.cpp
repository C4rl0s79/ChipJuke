// vgmplay - modul chipstate: kolejnosc i nazwy kanalow ukladow.
// MUSI byc identyczna z ChannelLayout silnika (src/engine/vgm_decoder.cpp: appendChannels z DEV_DECL libvgm:
// channelCount + channelNames; kanaly ukladu polaczonego dopisane po glownym z prefiksem nazwy ukladu).
// Tabela odpowiada deklaracjom emu/cores/*intf.c (DeviceChannels / DeviceChannelNames); zgodnosc z
// silnikiem pilnuje test "kolejnosc kanalow == ChannelLayout" (tests/chipstate_test.cpp), ktory otwiera
// syntetyczne naglowki dla kazdego z 48 ukladow przez makeVgmDecoder()->open().
#include "chip_layout.hpp"

#include <string>

namespace vgmplay::chipstate::detail {
namespace {

// Segment nazw: count > 0 => "<prefix>1".."<prefix>N"; count == 0 => jedna nazwa dosłownie = prefix.
struct Seg {
    const char* prefix;
    std::uint16_t count;
};

void append(std::vector<PatternChannel>& out, ChipType type, std::uint8_t instance, bool decoded,
            std::initializer_list<Seg> segs)
{
    for (const Seg& s : segs) {
        if (s.count == 0) {
            PatternChannel c;
            c.type = type; c.instance = instance; c.decoded = decoded;
            c.channelIdx = static_cast<std::uint16_t>(out.size());
            c.name = s.prefix;
            out.push_back(std::move(c));
            continue;
        }
        for (std::uint16_t i = 1; i <= s.count; ++i) {
            PatternChannel c;
            c.type = type; c.instance = instance; c.decoded = decoded;
            c.channelIdx = static_cast<std::uint16_t>(out.size());
            c.name = std::string(s.prefix) + std::to_string(i);
            out.push_back(std::move(c));
        }
    }
}

// Indeksy kanalow sa liczone od poczatku TEGO ukladu (kanaly main, potem linked), wiec buduje je osobno.
std::vector<PatternChannel> one(ChipType type, std::uint8_t instance, std::uint32_t flags)
{
    std::vector<PatternChannel> v;
    auto ch = [&](std::uint16_t n) { append(v, type, instance, false, {{"Ch ", n}}); };
    auto chd = [&](std::uint16_t n) { append(v, type, instance, true, {{"Ch ", n}}); };   // uklad z dekoderem
    switch (type) {
    case ChipType::SN76496:   append(v, type, instance, true, {{"", 3}, {"Noise", 0}}); break;
    case ChipType::YM2413:
    case ChipType::YM3812:
    case ChipType::YM3526:
        append(v, type, instance, true,
               {{"", 9}, {"Bass Drum", 0}, {"Snare Drum", 0}, {"Tom Tom", 0}, {"Cymbal", 0}, {"Hi-Hat", 0}});
        break;
    case ChipType::YM2612:    append(v, type, instance, true, {{"FM ", 6}, {"DAC", 0}}); break;
    case ChipType::YM2151:    append(v, type, instance, true, {{"Ch ", 8}}); break;
    case ChipType::SegaPcm:   chd(16); break;
    case ChipType::RF5C68:
    case ChipType::RF5C164:   ch(8); break;
    case ChipType::YM2203:
        append(v, type, instance, true, {{"Ch ", 3}, {"AY-3-8910 Ch ", 3}});
        break;
    case ChipType::YM2608:
    case ChipType::YM2610:
        append(v, type, instance, false,
               {{"FM ", 6}, {"ADPCM-A ", 6}, {"ADPCM-B", 0}, {"AY-3-8910 Ch ", 3}});
        break;
    case ChipType::Y8950:
        append(v, type, instance, false,
               {{"", 9}, {"Bass Drum", 0}, {"Snare Drum", 0}, {"Tom Tom", 0}, {"Cymbal", 0}, {"Hi-Hat", 0},
                {"ADPCM", 0}});
        break;
    case ChipType::YMF262:
        append(v, type, instance, true,
               {{"", 18}, {"Bass Drum", 0}, {"Snare Drum", 0}, {"Tom Tom", 0}, {"Cymbal", 0}, {"Hi-Hat", 0}});
        break;
    case ChipType::YMF278B:
        append(v, type, instance, false,
               {{"Ch ", 24}, {"YMF262 ", 18}, {"YMF262 Bass Drum", 0}, {"YMF262 Snare Drum", 0},
                {"YMF262 Tom Tom", 0}, {"YMF262 Cymbal", 0}, {"YMF262 Hi-Hat", 0}});
        break;
    case ChipType::YMF271:    ch(12); break;
    case ChipType::YMZ280B:   ch(8); break;
    case ChipType::PWM32X:    ch(1); break;
    case ChipType::AY8910:    append(v, type, instance, true, {{"Ch ", 3}}); break;
    case ChipType::GameBoyDMG:
        append(v, type, instance, false, {{"Square 1", 0}, {"Square 2", 0}, {"Wave", 0}, {"Noise", 0}});
        break;
    case ChipType::NesApu:
        if (flags & 0x80000000u)   // bit 31 zegara = FDS
            append(v, type, instance, true,
                   {{"Square 1", 0}, {"Square 2", 0}, {"Triangle", 0}, {"Noise", 0}, {"DPCM", 0}, {"FDS", 0}});
        else
            append(v, type, instance, true,
                   {{"Square 1", 0}, {"Square 2", 0}, {"Triangle", 0}, {"Noise", 0}, {"DPCM", 0}});
        break;
    case ChipType::MultiPCM:  ch(28); break;
    case ChipType::UPD7759:
    case ChipType::OKIM6258:  chd(1); break;
    case ChipType::OKIM6295:  append(v, type, instance, true, {{"Ch ", 4}}); break;
    case ChipType::K051649:   ch(5); break;
    case ChipType::K054539:   chd(8); break;
    case ChipType::HuC6280:   ch(6); break;
    case ChipType::C140:      chd(24); break;
    case ChipType::C219:      ch(16); break;
    case ChipType::K053260:   chd(4); break;
    case ChipType::Pokey:     ch(4); break;
    case ChipType::QSound:    append(v, type, instance, false, {{"PCM ", 16}, {"ADPCM ", 3}}); break;
    case ChipType::SCSP:      ch(32); break;
    case ChipType::WonderSwan: ch(4); break;
    case ChipType::VirtualBoyVSU:
    case ChipType::SAA1099:   ch(6); break;
    case ChipType::ES5503:    ch(32); break;
    case ChipType::ES5506:    break;   // szkielet bez rdzenia w libvgm: silnik pomija uklad (brak kanalow)
    case ChipType::X1010:     ch(16); break;
    case ChipType::C352:      ch(32); break;
    case ChipType::GA20:      chd(4); break;
    case ChipType::Mikey:     ch(4); break;
    case ChipType::K007232:
    case ChipType::K005289:   ch(2); break;
    case ChipType::MSM5205:   ch(1); break;
    case ChipType::MSM5232:   ch(11); break;
    case ChipType::BSMT2000:  append(v, type, instance, false, {{"PCM ", 12}, {"ADPCM", 0}}); break;
    case ChipType::ICS2115:   ch(32); break;
    default: break;
    }
    return v;
}

} // namespace

std::vector<PatternChannel> channelsFor(ChipType type, std::uint8_t instance, std::uint32_t flags)
{
    return one(type, instance, flags);
}

} // namespace vgmplay::chipstate::detail
