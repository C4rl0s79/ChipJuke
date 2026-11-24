// Czyste obliczenia szerokosci kolumn kanalow i przewijania (E4); bez ImGui, testowane.
// Kolumny kanalow (oscyloskop + kolumna wzorca pod nim) sa 2x szersze niz w E3 i - gdy ich suma nie
// miesci sie w panelu - przewijane poziomo jednym wspolnym przesunieciem (pixele). Ta sama funkcja
// clampScroll obsluguje pionowe przewijanie panelu Info.
#pragma once

#include <algorithm>
#include <cmath>

namespace vgmplay::ui {

// Mnoznik szerokosci kolumny wzgledem E3 (uzytkownik: "oscyloskopy sa za male, szerzej x2").
inline constexpr float kChannelColumnScale = 2.0f;

// Szerokosc kolumny kanalu: komorka wzorca (cellChars znakow + 1,5 znaku marginesu, jak w E3) * skala.
[[nodiscard]] inline float channelColumnWidth(float charWidth, int cellChars)
{
    return std::floor(charWidth * (static_cast<float>(cellChars) + 1.5f)) * kChannelColumnScale;
}

[[nodiscard]] inline float contentWidth(int columns, float colWidth) { return static_cast<float>(std::max(0, columns)) * colWidth; }

// Przesuniecie w [0, max(0, content - view)].
[[nodiscard]] inline float clampScroll(float offset, float content, float view)
{
    const float maxOff = std::max(0.0f, content - view);
    return std::clamp(offset, 0.0f, maxOff);
}

struct ColumnRange {
    int first = 0;   // pierwsza kolumna choc czesciowo widoczna
    int last = -1;   // ostatnia (wlacznie); last < first = brak
    [[nodiscard]] int count() const { return last >= first ? last - first + 1 : 0; }
};

// Kolumny, ktorych jakikolwiek piksel lezy w oknie [offset, offset + view).
[[nodiscard]] inline ColumnRange columnsInView(float offset, int columns, float colWidth, float view)
{
    ColumnRange r;
    if (columns <= 0 || colWidth <= 0.0f || view <= 0.0f) return r;
    r.first = std::clamp(static_cast<int>(std::floor(offset / colWidth)), 0, columns - 1);
    r.last = std::clamp(static_cast<int>(std::ceil((offset + view) / colWidth)) - 1, r.first, columns - 1);
    return r;
}

// Kolumny widoczne W CALOSCI (kolumna szersza niz okno: brak). last < first = brak.
[[nodiscard]] inline ColumnRange fullyVisibleColumns(float offset, int columns, float colWidth, float view)
{
    ColumnRange r;
    if (columns <= 0 || colWidth <= 0.0f || colWidth > view) return r;
    r.first = std::clamp(static_cast<int>(std::ceil((offset - 0.5f) / colWidth)), 0, columns);
    r.last = std::min(columns - 1, static_cast<int>(std::floor((offset + view + 0.5f) / colWidth)) - 1);
    return r;
}

// Czy kolumna miesci sie w oknie w calosci (kolumna szersza niz okno: wystarczy, ze wypelnia okno).
[[nodiscard]] inline bool columnFullyVisible(int column, float offset, float colWidth, float view)
{
    const float a = static_cast<float>(column) * colWidth, b = a + colWidth;
    if (colWidth > view) return a <= offset + 0.5f && b >= offset + view - 0.5f;
    return a >= offset - 0.5f && b <= offset + view + 0.5f;
}

// Najmniejsze przesuniecie, po ktorym kolumna jest w calosci widoczna (wynik obciety do zakresu).
[[nodiscard]] inline float revealColumn(float offset, int column, int columns, float colWidth, float view)
{
    const float content = contentWidth(columns, colWidth);
    const float a = static_cast<float>(column) * colWidth, b = a + colWidth;
    float o = offset;
    if (a < o) o = a;
    else if (b > o + view) o = b - view;
    return clampScroll(o, content, view);
}

// Pasek przewijania: uchwyt w torze o dlugosci `track`.
struct Thumb {
    float pos = 0.0f;
    float len = 0.0f;
};

[[nodiscard]] inline Thumb scrollThumb(float offset, float content, float view, float track, float minLen)
{
    Thumb t;
    if (content <= view || content <= 0.0f || track <= 0.0f) {
        t.len = track;
        return t;
    }
    t.len = std::clamp(track * view / content, std::min(minLen, track), track);
    const float maxOff = content - view;
    t.pos = (track - t.len) * std::clamp(offset / maxOff, 0.0f, 1.0f);
    return t;
}

// Przesuniecie odpowiadajace polozeniu lewej krawedzi uchwytu (przeciaganie myszka).
[[nodiscard]] inline float offsetFromThumb(float thumbPos, float content, float view, float track, float thumbLen)
{
    const float room = track - thumbLen;
    if (room <= 0.0f || content <= view) return 0.0f;
    return clampScroll(std::clamp(thumbPos / room, 0.0f, 1.0f) * (content - view), content, view);
}

// Przesuniecie o `wheel` "klikniec" kolka (znak: dodatnie = w gore/lewo), krok = polowa kolumny.
[[nodiscard]] inline float wheelScroll(float offset, float wheel, float step, float content, float view)
{
    return clampScroll(offset - wheel * step, content, view);
}

} // namespace vgmplay::ui
