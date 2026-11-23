// vgmplay - engine (wewnetrzne): mapowanie ukladow i polityka rdzeni. Patrz chip_map.hpp.
#include "chip_map.hpp"

#include "libvgm_headers.hpp"

namespace vgmplay::engine::detail {

namespace {
struct Pair { ChipType type; std::uint8_t dev; };
// Kolejnosc dowolna; kazdy ChipType (poza Unknown/Count_) ma wpis.
constexpr Pair kMap[] = {
    {ChipType::SN76496, DEVID_SN76496}, {ChipType::YM2413, DEVID_YM2413},
    {ChipType::YM2612, DEVID_YM2612},   {ChipType::YM2151, DEVID_YM2151},
    {ChipType::SegaPcm, DEVID_SEGAPCM}, {ChipType::RF5C68, DEVID_RF5C68},
    {ChipType::YM2203, DEVID_YM2203},   {ChipType::YM2608, DEVID_YM2608},
    {ChipType::YM2610, DEVID_YM2610},   {ChipType::YM3812, DEVID_YM3812},
    {ChipType::YM3526, DEVID_YM3526},   {ChipType::Y8950, DEVID_Y8950},
    {ChipType::YMF262, DEVID_YMF262},   {ChipType::YMF278B, DEVID_YMF278B},
    {ChipType::YMF271, DEVID_YMF271},   {ChipType::YMZ280B, DEVID_YMZ280B},
    {ChipType::RF5C164, DEVID_RF5C68},  {ChipType::PWM32X, DEVID_32X_PWM},
    {ChipType::AY8910, DEVID_AY8910},   {ChipType::GameBoyDMG, DEVID_GB_DMG},
    {ChipType::NesApu, DEVID_NES_APU},  {ChipType::MultiPCM, DEVID_YMW258},
    {ChipType::UPD7759, DEVID_uPD7759}, {ChipType::OKIM6258, DEVID_MSM6258},
    {ChipType::OKIM6295, DEVID_MSM6295},{ChipType::K051649, DEVID_K051649},
    {ChipType::K054539, DEVID_K054539}, {ChipType::HuC6280, DEVID_C6280},
    {ChipType::C140, DEVID_C140},       {ChipType::K053260, DEVID_K053260},
    {ChipType::Pokey, DEVID_POKEY},     {ChipType::QSound, DEVID_QSOUND},
    {ChipType::SCSP, DEVID_SCSP},       {ChipType::WonderSwan, DEVID_WSWAN},
    {ChipType::VirtualBoyVSU, DEVID_VBOY_VSU}, {ChipType::SAA1099, DEVID_SAA1099},
    {ChipType::ES5503, DEVID_ES5503},   {ChipType::ES5506, DEVID_ES5506},
    {ChipType::X1010, DEVID_X1_010},    {ChipType::C352, DEVID_C352},
    {ChipType::GA20, DEVID_GA20},       {ChipType::Mikey, DEVID_MIKEY},
    {ChipType::K007232, DEVID_K007232}, {ChipType::K005289, DEVID_K005289},
    {ChipType::MSM5205, DEVID_MSM5205}, {ChipType::MSM5232, DEVID_MSM5232},
    {ChipType::BSMT2000, DEVID_BSMT2000}, {ChipType::ICS2115, DEVID_ICS2115},
    {ChipType::C219, DEVID_C219},
};
} // namespace

bool chipTypeToDevId(ChipType type, std::uint8_t& devId) noexcept
{
    for (const Pair& p : kMap) {
        if (p.type == type) { devId = p.dev; return true; }
    }
    return false;
}

ChipType devIdToChipType(std::uint8_t devId, std::uint8_t cfgFlags) noexcept
{
    if (devId == DEVID_RF5C68) return cfgFlags == 1 ? ChipType::RF5C164 : ChipType::RF5C68;
    for (const Pair& p : kMap) {
        // RF5C164 i YMW258 (drugi wpis tego samego DEV_ID) pomijamy: zwracamy pierwszy.
        if (p.dev == devId) return p.type;
    }
    return ChipType::Unknown;
}

// ---------------------------------------------------------------------------------------------
// Polityka rdzeni. Zasada: jawny rdzen o czytelnej licencji (docs/licensing/libvgm-audit.md),
// z lista zapasowa; wybor ZAWSZE sprawdzany wzgledem rdzeni skompilowanych (tryb scisly
// VGMPLAY_LIBVGM_STRICT_LICENSES wylacza m.in. GENS i CTR - lista zapasowa musi je pomijac).
// ---------------------------------------------------------------------------------------------
CorePreference defaultCorePreference(std::uint8_t devId)
{
    switch (devId) {
    case DEVID_YM2612:  return {{FCC_GPGX, FCC_NUKE, FCC_GENS}, {}};
    case DEVID_YM3812:  return {{FCC_MAME, FCC_NUKE}, {}};
    case DEVID_YMF262:  return {{FCC_MAME, FCC_NUKE}, {}};
    // OPL4: libvgm wymusza dla czesci FM (YMF262) FCC_ADLE (vgmplayer.cpp, DeviceLinkCallback),
    // a adlibemu jest wylaczony w buildzie - bez emuCore[1] OPL4 zagralby bez FM.
    case DEVID_YMF278B: return {{}, {FCC_MAME, FCC_NUKE}};
    case DEVID_C6280:   return {{FCC_MAME}, {}};            // Ootake wylaczony (licencja)
    // libvgm wpisuje na stale CTR (QSound) i GENS (RF5C164); oba maja niejasna licencje.
    case DEVID_QSOUND:  return {{FCC_MAME, FCC_CTR_}, {}};
    case DEVID_RF5C68:  return {{FCC_MAME, FCC_GENS}, {}};
    case DEVID_SN76496: return {{FCC_MAME, FCC_MAXM}, {}};
    case DEVID_SAA1099: return {{FCC_MAME, FCC_VBEL}, {}};
    // Jawnie to, co libvgm wybralby domyslnie (pierwszy z listy) - odporne na zmiane kolejnosci.
    // NES: NSFPlay ma niejasna licencje (audyt, poz. 3); w trybie scislym jest wylaczony i
    // lista zapasowa daje MAME.
    case DEVID_NES_APU: return {{FCC_NSFP, FCC_MAME}, {}};
    case DEVID_YM2413:  return {{FCC_EMU_, FCC_MAME}, {}};
    case DEVID_YM2151:  return {{FCC_MAME, FCC_NUKE}, {}};
    case DEVID_AY8910:  return {{FCC_EMU_, FCC_MAME}, {}};
    case DEVID_GB_DMG:  return {{FCC_SBOY, FCC_MAME}, {}};
    default:            return {};
    }
}

bool coreAvailable(std::uint8_t devId, std::uint32_t fcc)
{
    const DEV_DECL* decl = SndEmu_GetDevDecl(devId, nullptr, 0);
    if (!decl) return false;
    for (const DEV_DEF* const* c = decl->cores; *c; ++c) {
        if ((*c)->coreID == fcc) return true;
    }
    return false;
}

std::uint32_t firstAvailable(std::uint8_t devId, std::span<const std::uint32_t> list)
{
    for (std::uint32_t fcc : list) {
        if (coreAvailable(devId, fcc)) return fcc;
    }
    return 0;
}

std::uint32_t libDefaultCore(std::uint8_t devId)
{
    const DEV_DECL* decl = SndEmu_GetDevDecl(devId, nullptr, 0);
    if (!decl || !decl->cores[0]) return 0;
    return decl->cores[0]->coreID;
}

std::string fccToString(std::uint32_t fcc)
{
    std::string s;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = static_cast<char>((fcc >> shift) & 0xFF);
        if (c >= 0x20 && c < 0x7F) s.push_back(c);
    }
    return s;
}

} // namespace vgmplay::engine::detail
