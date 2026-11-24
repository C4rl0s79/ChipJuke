// Czysta logika widoku wzorca w stylu FastTrackera 2 (bez ImGui; testowalna):
//  * pozycja slyszana (klatki silnika) -> wiersz wzorca (rowTicks, petla),
//  * mapowanie kolumn wzorca (chipstate::PatternChannel) na kanaly ChannelLayout,
//  * notacja komorek: "C-4 01 40 ...", "===" (key-off), "---" (pusto), ".." (brak),
//  * ustawienie dlugosci wiersza (presety rowTicks),
//  * sztuczny wzorzec do diagnostyki (--fake-pattern).
//
// Kontrakt danych: include/vgmplay/chipstate.hpp (Pattern liczony raz po zaladowaniu na watku
// workera; UI go tylko czyta). rowTicks sa w tickach VGM (44100 Hz), pozycja slyszana w klatkach
// silnika (sampleRate silnika).
#pragma once

#include "vgmplay/chipstate.hpp"
#include "vgmplay/layout.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vgmplay::ui {

inline constexpr std::uint32_t kVgmTickRate = 44100;
inline constexpr std::uint32_t kDefaultRowTicks = 735;   // 1/60 s

// Presety dlugosci wiersza (rosnaco = coraz wolniejszy przewijany wzorzec): 1/120, 1/60, 1/50,
// 1/30, 1/20, 1/10 s.
inline constexpr std::uint32_t kRowTickPresets[] = {368, 735, 882, 1470, 2205, 4410};
inline constexpr int kRowTickPresetCount = static_cast<int>(sizeof(kRowTickPresets) / sizeof(kRowTickPresets[0]));

// Najblizszy preset (dla dowolnej wartosci; 0 -> domyslny).
std::uint32_t nearestRowTicks(std::uint32_t ticks);
// dir > 0: wiersz krotszy (wiecej wierszy na sekunde), dir < 0: dluzszy; obciete do zakresu presetow.
std::uint32_t stepRowTicks(std::uint32_t current, int dir);
// "1/60 s" (liczba wierszy na sekunde zaokraglona).
std::string rowTicksLabel(std::uint32_t ticks);

// Klatki silnika -> ticki VGM (44100 Hz), bez przepelnien dla dlugich utworow.
std::uint64_t framesToTicks(std::uint64_t frames, std::uint32_t sampleRate);

struct PatternTiming {
    std::uint32_t rowTicks = kDefaultRowTicks;
    std::uint64_t rowCount = 0;
    std::uint64_t loopStartRow = 0;
    bool hasLoop = false;
};
PatternTiming timingOf(const chipstate::Pattern& p, bool hasLoop);

// Wiersz odpowiadajacy pozycji slyszanej. Po koncu danych: z petla -> loopStartRow + (nadmiar mod
// dlugosc petli), bez petli -> ostatni wiersz. rowCount == 0 -> 0.
std::uint64_t rowAtFrame(std::uint64_t audibleFrame, std::uint32_t sampleRate, const PatternTiming& t);
// Wiersz do narysowania dla "wirtualnego" wiersza (moze wykraczac poza koniec: wzorzec przed
// koncem pokazuje juz poczatek petli). nullopt = brak (przed poczatkiem / za koncem bez petli).
std::optional<std::uint64_t> displayRow(std::int64_t virtualRow, const PatternTiming& t);

// Dla kazdego kanalu globalnego (indeks w layout.channels): indeks kolumny wzorca albo -1, gdy
// wzorzec nie ma takiego kanalu lub kanal nie jest zdekodowany (decoded == false) - "n/a".
// Dopasowanie po (typ ukladu, instancja, indeks kanalu).
std::vector<int> mapPatternColumns(const ChannelLayout& layout, const chipstate::Pattern* pattern);

// Tekst komorki, kazde pole o stalej szerokosci (nuta 3, instrument 2, glosnosc 2, efekt 3).
struct CellText {
    char note[4];
    char inst[3];
    char vol[3];
    char fx[4];
};
inline constexpr int kCellCharsNoFx = 9;    // "C-4 01 40"
inline constexpr int kCellCharsFx = 13;     // "C-4 01 40 ..."
// MIDI 60 = "C-4"; kNoteNone "---"; kNoteOff "==="; poza zakresem "???"; nuty < 12 (oktawa ujemna)
// maja oktawe "-" (np. "C--").
void formatNote(std::int16_t note, char out[4]);
CellText formatCell(const chipstate::Cell& c);
// Czy komorka niesie jakiekolwiek zdarzenie (nuta, key-off, instrument, glosnosc, efekt).
bool cellHasEvent(const chipstate::Cell& c);
// Czy wzorzec zawiera jakikolwiek efekt (decyduje, czy rysowac kolumne efektow).
bool patternHasEffects(const chipstate::Pattern& p);

// DIAGNOSTYKA (--fake-pattern): wzorzec ze sztucznymi nutami dla kanalow ukladu; zdarzenia zaleza
// od czasu (nie od numeru wiersza), wiec wygladaja podobnie dla kazdego rowTicks. Kanal 5 (przy
// >= 8 kanalach) jest niezdekodowany, by pokazac "n/a".
std::shared_ptr<chipstate::Pattern> makeFakePattern(const ChannelLayout& layout, std::uint64_t totalTicks,
                                                    std::uint64_t loopStartTicks, bool hasLoop,
                                                    std::uint32_t rowTicks);

} // namespace vgmplay::ui
