// Wspolne pomocniki rysowania paneli (wewnetrzne dla src/ui; nie jest czescia API modulu).
#pragma once

#include "vgmplay_ui/theme.h"

#include <imgui.h>

#include "vgmplay_input/bindings_edit.hpp"

#include <span>
#include <string>

namespace vgmplay::ui::detail {

bool beginPanel(const char* id, float x, float y, float w, float h);
float drawFrame(ImDrawList* dl, const char* title, bool focused);   // zwraca wysokosc paska tytulu
bool clickedInside();
// Wyrazny pierscien fokusu (kolory z palety).
void drawFocusRing(ImDrawList* dl, ImVec2 a, ImVec2 b, float dpi);
// Nazwy do wyswietlenia (bindings_screen.cpp).
const char* actionTitle(input::Action a);
std::string sourceLabel(const input::BindingSource& s);
std::string classLabel(std::span<const input::Binding> b, input::Action a, input::SourceClass cls);
void clippedText(ImDrawList* dl, float x0, float x1, float y, ImU32 color, const char* text);

// Rzad przyciskow tekstowych z zawijaniem do nastepnej linii.
struct ButtonFlow {
    ImDrawList* dl = nullptr;
    float x0 = 0, right = 0, cx = 0, cy = 0, h = 0, dpi = 1;
    float lineStartX() const { return x0; }
    // Zwraca true, gdy kliknieto (lewy przycisk) aktywny przycisk.
    bool button(const std::string& label, bool enabled, bool active = false, const char* tip = nullptr,
                bool ring = false);
    void text(const std::string& label, float maxW, ImU32 color);
    void newline() { cx = x0; cy += h + 2.0f * dpi; }
    float bottom() const { return cy + h; }
};

} // namespace vgmplay::ui::detail
