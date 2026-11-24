// Male widgety rysowane przez ImDrawList dla widoku kanalow (E1.5): przelacznik M/S, wskaznik VU
// w stylu ProTrackera, przebieg w stylu FastTrackera 2. Kolory wylacznie z palette().
// Wolac w kontekscie okna ImGui (korzystaja z myszy i dpi z ImGuiStyle::FontScaleDpi).
#pragma once

#include "vgmplay_ui/channel_scopes.h"

#include <imgui.h>

#include <vector>

namespace vgmplay::ui {

// Kwadratowy przelacznik z litera (np. M / S). Aktywny: wypelniony kolorem `active`, litera ciemna.
// Zwraca true w klatce klikniecia lewym przyciskiem. `tooltip` pokazywany po najechaniu.
bool drawToggleBox(ImDrawList* dl, ImVec2 topLeft, float size, const char* letter, bool on,
                   const ImVec4& activeColor, const char* tooltip);

// Pionowy wskaznik poziomu z segmentami (zielony/zolty/czerwony), wskaznik szczytu = jeden segment.
// `dim`: kanal wyciszony - przygaszone.
void drawVuBar(ImDrawList* dl, ImVec2 a, ImVec2 b, const VuMeter& meter, bool dim);

// Przebieg w prostokacie [a, b]: linia srodkowa, probki (-1..1, mnozone przez `gain`) sprowadzone do
// pikseli (najwieksza co do modulu probka w kolumnie), pikselowa linia bez antyaliasingu.
void drawWave(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::vector<float>& samples, float gain,
              const ImVec4& color, std::vector<ImVec2>& scratch);

// Tekst wysrodkowany w prostokacie, z polprzezroczystym tlem.
void drawCenteredTag(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* text, const ImVec4& color, bool backdrop);

} // namespace vgmplay::ui
