// Motyw "tracker dark": jedyne miejsce definiujace kolory interfejsu.
// Kod rysujacy pobiera kolory wylacznie z palette() - bez literalow kolorow.
#pragma once

#include <imgui.h>

namespace vgmplay::ui {

inline constexpr int kChannelCount = 6;

struct Palette {
    ImVec4 window;      // tlo okna aplikacji (czyszczenie ramki)
    ImVec4 panel;       // tlo paneli
    ImVec4 panelHeader; // pasek tytulu panelu (bez fokusu)
    ImVec4 focusHeader; // pasek tytulu panelu z fokusem
    ImVec4 border;      // obramowanie panelu
    ImVec4 focusBorder; // obramowanie panelu z fokusem
    ImVec4 text;
    ImVec4 textDim;
    ImVec4 accent;
    ImVec4 gridBeat;    // wiersz co 4
    ImVec4 gridBar;     // wiersz co 16
    ImVec4 cursorRow;   // wiersz kursora
    ImVec4 cursorCell;  // komorka kursora
    ImVec4 cursorText;  // tekst na komorce kursora
    ImVec4 scopeGrid;
    ImVec4 scopeLine;   // kanal L
    ImVec4 scopeLineR;  // kanal R
    ImVec4 error;       // komunikat bledu
    ImVec4 notice;      // dyskretna informacja (ostrzezenia)
    ImVec4 scopeCell;   // tlo ramki oscyloskopu kanalu
    ImVec4 vuLow;       // segmenty VU: dol (zielony)
    ImVec4 vuMid;       // segmenty VU: srodek (zolty)
    ImVec4 vuHigh;      // segmenty VU: gora (czerwony)
    ImVec4 vuOff;       // segment zgaszony
    ImVec4 muteOn;      // przelacznik MUTE wlaczony
    ImVec4 soloOn;      // przelacznik SOLO wlaczony
    ImVec4 playing;     // biezacy wpis playlisty (znacznik)
    ImVec4 overlay;     // przyciemnienie pod oknem dialogowym
    ImVec4 dialog;      // tlo okna dialogowego
    ImVec4 channel[kChannelCount];
};

const Palette& palette();

inline ImU32 col32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImVec4 withAlpha(ImVec4 c, float a) { c.w = a; return c; }

// Instaluje font (dane TTF w pamieci; wskaznik musi zyc do konca programu) i styl ImGui.
// dpiScale: skala wyswietlacza (SDL_GetWindowDisplayScale).
void installFont(ImGuiIO& io, const void* ttfData, int ttfSize);
void applyTheme(float dpiScale);

} // namespace vgmplay::ui
