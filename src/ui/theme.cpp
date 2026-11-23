#include "vgmplay_ui/theme.h"

#include <cstdio>

namespace vgmplay::ui {
namespace {

constexpr ImVec4 rgb(unsigned hex, float a = 1.0f)
{
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f,
                  (hex & 0xFF) / 255.0f, a);
}

constexpr float kBaseFontSize = 16.0f;

} // namespace

const Palette& palette()
{
    static const Palette p = {
        /*window*/ rgb(0x0B0D12),
        /*panel*/ rgb(0x12151C),
        /*panelHeader*/ rgb(0x1C212C),
        /*focusHeader*/ rgb(0x23506B),
        /*border*/ rgb(0x2A3040),
        /*focusBorder*/ rgb(0x4FC3F7),
        /*text*/ rgb(0xE3E8F0),
        /*textDim*/ rgb(0x8C96A8),
        /*accent*/ rgb(0xFFB74D),
        /*gridBeat*/ rgb(0x1A1F2A),
        /*gridBar*/ rgb(0x252C3B),
        /*cursorRow*/ rgb(0x4FC3F7, 0.18f),
        /*cursorCell*/ rgb(0xFFB74D),
        /*cursorText*/ rgb(0x10131A),
        /*scopeGrid*/ rgb(0x232A38),
        /*scopeLine*/ rgb(0x69F0AE),
        /*channel*/
        {rgb(0xFF6B6B), rgb(0xFFB74D), rgb(0xFFEB6B), rgb(0x69F0AE), rgb(0x4FC3F7),
         rgb(0xC792EA)},
    };
    return p;
}

void installFont(ImGuiIO& io, const void* ttfData, int ttfSize)
{
    if (!ttfData || ttfSize <= 0) {
        std::fprintf(stderr, "vgmplay: brak fontu osadzonego, uzywam domyslnego (bez JP)\n");
        io.Fonts->AddFontDefault();
        return;
    }
    // imgui >= 1.92: atlas dynamiczny - glify (w tym kanji) rasteryzowane na zadanie,
    // rozmiar 0 = rozmiar z ImGuiStyle::FontSizeBase; zakresy glifow zbedne.
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(ttfData), ttfSize, 0.0f, &cfg);
}

void applyTheme(float dpiScale)
{
    const Palette& p = palette();
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.WindowBorderSize = 0.0f;
    s.WindowRounding = 0.0f;
    s.FrameRounding = 0.0f;
    s.ItemSpacing = ImVec2(6, 4);
    s.ScaleAllSizes(dpiScale);
    s.FontSizeBase = kBaseFontSize;
    s.FontScaleDpi = dpiScale;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = p.panel;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textDim;
    c[ImGuiCol_Border] = p.border;
}

} // namespace vgmplay::ui
