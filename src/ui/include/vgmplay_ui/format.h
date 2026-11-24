// Czyste funkcje formatowania i arytmetyki paska pozycji (bez ImGui; testowalne).
#pragma once

#include <cstdint>
#include <string>

namespace vgmplay::ui {

// "m:ss" (do 59:59) albo "h:mm:ss"; ujemne i NaN -> "0:00".
std::string formatTime(double seconds);
// Zegar ukladu: "7.670 MHz", "985 kHz", "440 Hz".
std::string formatClock(std::uint32_t hz);
inline double framesToSeconds(std::uint64_t frames, std::uint32_t sampleRate)
{
    return sampleRate ? static_cast<double>(frames) / static_cast<double>(sampleRate) : 0.0;
}

// Pasek pozycji. Gdy dlugosc calkowita jest nieznana (nieskonczone petle: totalFrames == 0),
// pasek obejmuje jedno przejscie (intro + petla), a pozycja jest zawijana w petli.
struct SeekBar {
    std::uint64_t length = 0;    // 0 = brak (nic nie zaladowano)
    std::uint64_t position = 0;  // [0, length]
    bool infinite = false;
};
SeekBar makeSeekBar(std::uint64_t audibleFrame, std::uint64_t totalFrames,
                    std::uint64_t introFrames, std::uint64_t loopFrames,
                    std::uint64_t onePassFrames);

// Ulamek [0,1] -> klatka w [0, length) (ostatnia dopuszczalna klatka = length - 1).
std::uint64_t fractionToFrame(double fraction, std::uint64_t length);
// Pozycja +- przesuniecie w sekundach, obcieta do [0, length - 1] (length == 0 -> 0).
std::uint64_t offsetFrame(std::uint64_t position, double seconds, std::uint32_t sampleRate,
                          std::uint64_t length);

// Tekst z niezaufanego pliku do wyswietlenia: usuwa znaki kierunkowe/niewidoczne Unicode
// (U+200B-U+200F, U+202A-U+202E, U+2066-U+2069), ktore moglyby odwracac kolejnosc wyswietlania.
std::string sanitizeForDisplay(const std::string& utf8);

} // namespace vgmplay::ui
