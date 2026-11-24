#include "vgmplay_ui/channel_widgets.h"

#include "vgmplay_ui/theme.h"

#include <algorithm>
#include <cmath>

namespace vgmplay::ui {
namespace {

float dpiScale() { return ImGui::GetStyle().FontScaleDpi; }

} // namespace

bool drawToggleBox(ImDrawList* dl, ImVec2 a, float size, const char* letter, bool on,
                   const ImVec4& activeColor, const char* tooltip)
{
    const Palette& p = palette();
    const ImVec2 b(a.x + size, a.y + size);
    const bool hover = ImGui::IsMouseHoveringRect(a, b);
    dl->AddRectFilled(a, b, col32(on ? activeColor : p.panelHeader));
    dl->AddRect(a, b, col32(on ? activeColor : (hover ? p.focusBorder : p.textDim)));
    const ImVec2 ts = ImGui::CalcTextSize(letter);
    dl->AddText(ImVec2(a.x + (size - ts.x) * 0.5f, a.y + (size - ts.y) * 0.5f),
                col32(on ? p.cursorText : (hover ? p.text : p.textDim)), letter);
    if (!hover) return false;
    if (tooltip) ImGui::SetTooltip("%s", tooltip);
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

void drawVuBar(ImDrawList* dl, ImVec2 a, ImVec2 b, const VuMeter& m, bool dim)
{
    const Palette& p = palette();
    const float dpi = dpiScale();
    const float h = b.y - a.y;
    if (h < 4.0f || b.x - a.x < 2.0f) return;
    const int segs = std::clamp(static_cast<int>(h / (3.0f * dpi)), 4, 24);
    const float segH = h / static_cast<float>(segs);
    const float gap = std::max(1.0f, std::floor(dpi * 0.5f));
    const int lit = VuMeter::litSegments(m.level, segs);
    const int peakSeg = VuMeter::litSegments(m.peak, segs) - 1;
    const float alpha = dim ? 0.30f : 1.0f;
    for (int s = 0; s < segs; ++s) {
        const float frac = (static_cast<float>(s) + 0.5f) / static_cast<float>(segs);
        const ImVec4& c = frac < 0.60f ? p.vuLow : (frac < 0.85f ? p.vuMid : p.vuHigh);
        const float y1 = std::floor(b.y - static_cast<float>(s) * segH);
        const float y0 = std::max(a.y, std::floor(b.y - static_cast<float>(s + 1) * segH) + gap);
        const bool on = s < lit || s == peakSeg;
        dl->AddRectFilled(ImVec2(a.x, y0), ImVec2(b.x, std::max(y0 + 1.0f, y1)),
                          col32(on ? withAlpha(c, alpha) : p.vuOff));
    }
}

void drawWave(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::vector<float>& samples, float gain,
              const ImVec4& color, std::vector<ImVec2>& pts)
{
    const Palette& p = palette();
    const float dpi = dpiScale();
    const float px = std::max(1.0f, std::floor(dpi + 0.5f));
    const float w = b.x - a.x, h = b.y - a.y;
    if (w < 4.0f || h < 4.0f) return;
    const float cy = std::floor(a.y + h * 0.5f);
    dl->AddLine(ImVec2(a.x, cy), ImVec2(b.x, cy), col32(p.scopeGrid), px);
    if (samples.size() < 2) return;

    const int cols = std::max(2, static_cast<int>(w / px));
    const std::size_t n = samples.size();
    pts.clear();
    pts.reserve(static_cast<std::size_t>(cols));
    const float amp = (h * 0.5f - px) ;
    for (int c = 0; c < cols; ++c) {
        const std::size_t i0 = static_cast<std::size_t>(c) * n / static_cast<std::size_t>(cols);
        const std::size_t i1 = std::max(i0 + 1, static_cast<std::size_t>(c + 1) * n / static_cast<std::size_t>(cols));
        float v = samples[i0];
        for (std::size_t i = i0; i < std::min(i1, n); ++i)
            if (std::fabs(samples[i]) > std::fabs(v)) v = samples[i];
        v = std::clamp(v * gain, -1.0f, 1.0f);
        pts.emplace_back(std::floor(a.x + static_cast<float>(c) * px), std::floor(cy - v * amp));
    }
    // Bez antyaliasingu: ostre piksele jak w FastTrackerze.
    const ImDrawListFlags saved = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedLines;
    dl->PushClipRect(a, b, true);
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col32(color), ImDrawFlags_None, px);
    dl->PopClipRect();
    dl->Flags = saved;
}

void drawCenteredTag(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* text, const ImVec4& color, bool backdrop)
{
    const Palette& p = palette();
    const float dpi = dpiScale();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    if (ts.x > b.x - a.x || ts.y > b.y - a.y) return;
    const ImVec2 pos(std::floor(a.x + (b.x - a.x - ts.x) * 0.5f), std::floor(a.y + (b.y - a.y - ts.y) * 0.5f));
    if (backdrop)
        dl->AddRectFilled(ImVec2(pos.x - 3.0f * dpi, pos.y - dpi), ImVec2(pos.x + ts.x + 3.0f * dpi, pos.y + ts.y + dpi),
                          col32(withAlpha(p.scopeCell, 0.75f)));
    dl->AddText(pos, col32(color), text);
}

} // namespace vgmplay::ui
