// vgmplay - engine (wewnetrzne): mapowanie ChipType <-> DEVID_* libvgm i polityka rdzeni.
// Naglowek zalezy od libvgm - NIE dolaczac go poza src/engine.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "vgmplay/chips.hpp"
#include "vgmplay/core.hpp"

namespace vgmplay::engine::detail {

// ChipType -> DEV_ID libvgm; false = brak odpowiednika (Unknown).
[[nodiscard]] bool chipTypeToDevId(ChipType type, std::uint8_t& devId) noexcept;
// DEV_ID (+ flagi z DEV_GEN_CFG::flags) -> ChipType. RF5C164 rozpoznawany po flags == 1.
[[nodiscard]] ChipType devIdToChipType(std::uint8_t devId, std::uint8_t cfgFlags) noexcept;

// Kolejnosc preferencji rdzeni (FCC) vgmplay dla ukladu. Pusta lista = domyslny rdzen libvgm
// (pierwszy skompilowany z listy rdzeni ukladu). Wartosc 0 na koncu nie jest uzywana.
struct CorePreference {
    std::vector<std::uint32_t> main;
    std::vector<std::uint32_t> linked;   // rdzen ukladu polaczonego (np. YMF262 w OPL4)
};
[[nodiscard]] CorePreference defaultCorePreference(std::uint8_t devId);

// Czy rdzen (FCC) jest skompilowany dla ukladu (SndEmu_GetDevDecl()->cores).
[[nodiscard]] bool coreAvailable(std::uint8_t devId, std::uint32_t fcc);
// Pierwszy dostepny z listy; 0 gdy zadnego.
[[nodiscard]] std::uint32_t firstAvailable(std::uint8_t devId, std::span<const std::uint32_t> list);
// FCC pierwszego (domyslnego libvgm) rdzenia ukladu; 0 gdy brak deklaracji.
[[nodiscard]] std::uint32_t libDefaultCore(std::uint8_t devId);

// Kod FCC -> 4 znaki ("MAME") do komunikatow.
[[nodiscard]] std::string fccToString(std::uint32_t fcc);

} // namespace vgmplay::engine::detail
