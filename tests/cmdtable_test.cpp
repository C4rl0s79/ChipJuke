// Zgodnosc tabeli komend modulu format (src/format/cmdtable.hpp) z VGMPlayer::_CMD_INFO libvgm.
//
// _CMD_INFO jest `protected static` w VGMPlayer, wiec test korzysta z podklasy (jak src/engine/tap_vgm_player.hpp) -
// bez zmian w external/libvgm. Tabela formatu jest pisana recznie (len, chip, kind); ten test wylapuje
// rozjazd po aktualizacji submodulu libvgm (np. nowa komenda, zmieniona dlugosc) - rozjazd oznaczalby, ze pre-skan
// wyznacza inne granice komend niz libvgm (H2/H3 w docs/research/libvgm-parser-hazards.md).
#include <doctest/doctest.h>

#include <ostream>   // wymagane przez doctest dla stringifikacji std::string_view

#include <string>

// Te same definicje, ktore dostaje vgmplay_engine (stdtype.h libvgm; odczyt LE w vgmplayer.hpp).
#ifndef HAVE_STDINT_H
#define HAVE_STDINT_H
#endif
#ifndef VGM_LITTLE_ENDIAN
#define VGM_LITTLE_ENDIAN
#endif

#include "../src/engine/libvgm_headers.hpp"
#include "../src/format/cmdtable.hpp"

namespace {

// Dostep do skladowych protected VGMPlayer. Nie tworzymy instancji - tylko statyczna tabela i adresy handlerow.
class CmdTableProbe : public VGMPlayer {
public:
    struct Row {
        unsigned chip;
        unsigned len;
        char kind;
    };

    static Row row(unsigned cmd)
    {
        const COMMAND_INFO& ci = _CMD_INFO[cmd];
        char kind = 'N';
        if (ci.func == &CmdTableProbe::Cmd_unknown) kind = 'U';
        else if (ci.func == &CmdTableProbe::Cmd_invalid) kind = 'I';
        else if (ci.func == &CmdTableProbe::Cmd_EndOfData) kind = 'E';
        else if (ci.func == &CmdTableProbe::Cmd_DataBlock) kind = 'B';
        return Row{ci.chipType, ci.cmdLen, kind};
    }
};

} // namespace

TEST_CASE("kCmdTable == VGMPlayer::_CMD_INFO (dlugosc, uklad, rodzaj) dla wszystkich 256 komend")
{
    using vgmplay::format::detail::kCmdTable;
    int mismatches = 0;
    for (unsigned cmd = 0; cmd < 256; ++cmd) {
        const CmdTableProbe::Row libvgm = CmdTableProbe::row(cmd);
        const auto& ours = kCmdTable[cmd];
        const bool same = libvgm.len == ours.len && libvgm.chip == ours.chip && libvgm.kind == ours.kind;
        if (!same) ++mismatches;
        CHECK_MESSAGE(same, "komenda 0x", std::to_string(cmd), " (dziesietnie): libvgm {len=", libvgm.len,
                      ", chip=", libvgm.chip, ", kind=", libvgm.kind, "} format {len=", unsigned{ours.len},
                      ", chip=", unsigned{ours.chip}, ", kind=", ours.kind, "}");
    }
    CHECK(mismatches == 0);
}
