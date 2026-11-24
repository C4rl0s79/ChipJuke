// Mapa fokusu panelu (E3, E4): wiersze elementow i kursor. Elementy rejestruja sie podczas rysowania
// (reg), wiec kursor moze stanac WYLACZNIE na elemencie, ktory w ostatniej klatce byl narysowany
// (np. przycisk paska narzedzi, ktory sie nie zmiescil, nie istnieje w mapie). Czysta logika, testowana.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <vector>

namespace vgmplay::ui {

struct FocusGrid {
    int row = 0, col = 0;

    FocusGrid() = default;
    FocusGrid(int r, int c) : row(r), col(c) {}

    void begin() { building_.clear(); }
    void reg(int r, int c)
    {
        if (r < 0 || c < 0) return;
        if (static_cast<int>(building_.size()) <= r) building_.resize(static_cast<std::size_t>(r) + 1);
        building_[static_cast<std::size_t>(r)].push_back(c);
    }
    void end()
    {
        cols_ = building_;
        for (std::vector<int>& v : cols_) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }
        if (rowCount() == 0) return;
        row = nearestRow(row);
        col = nearestCol(row, col);
    }

    [[nodiscard]] bool at(int r, int c) const { return row == r && col == c; }
    [[nodiscard]] int rowCount() const { return static_cast<int>(cols_.size()); }
    [[nodiscard]] bool has(int r, int c) const
    {
        if (r < 0 || r >= rowCount()) return false;
        const std::vector<int>& v = cols_[static_cast<std::size_t>(r)];
        return std::binary_search(v.begin(), v.end(), c);
    }
    // Liczba elementow w wierszu (zarejestrowanych, nie "najwiekszy indeks").
    [[nodiscard]] int rowSize(int r) const
    {
        return r >= 0 && r < rowCount() ? static_cast<int>(cols_[static_cast<std::size_t>(r)].size()) : 0;
    }

    // Gora/Dol: kolejny niepusty wiersz w danym kierunku (puste wiersze sa pomijane).
    void moveRow(int d)
    {
        if (d == 0 || rowCount() == 0) return;
        const int step = d < 0 ? -1 : 1;
        int r = row;
        for (int left = std::abs(d); left > 0; --left) {
            int n = r + step;
            while (n >= 0 && n < rowCount() && rowSize(n) == 0) n += step;
            if (n < 0 || n >= rowCount()) break;
            r = n;
        }
        row = r;
        col = nearestCol(row, col);
    }
    // Lewo/Prawo: kolejny ZAREJESTROWANY element w wierszu (dziury w numeracji sa pomijane).
    void moveCol(int d)
    {
        if (d == 0 || rowSize(row) == 0) return;
        const std::vector<int>& v = cols_[static_cast<std::size_t>(row)];
        auto it = std::lower_bound(v.begin(), v.end(), col);
        int idx = static_cast<int>(it - v.begin());
        if (it == v.end() || *it != col) idx = d < 0 ? idx : idx - 1;   // kursor poza mapa: najblizszy
        idx = std::clamp(idx + d, 0, static_cast<int>(v.size()) - 1);
        col = v[static_cast<std::size_t>(idx)];
    }

private:
    [[nodiscard]] int nearestRow(int r) const
    {
        r = std::clamp(r, 0, rowCount() - 1);
        if (rowSize(r) > 0) return r;
        for (int k = 1; k < rowCount(); ++k) {
            if (r - k >= 0 && rowSize(r - k) > 0) return r - k;
            if (r + k < rowCount() && rowSize(r + k) > 0) return r + k;
        }
        return r;
    }
    [[nodiscard]] int nearestCol(int r, int c) const
    {
        if (rowSize(r) == 0) return c;
        const std::vector<int>& v = cols_[static_cast<std::size_t>(r)];
        int best = v.front();
        for (int x : v)
            if (std::abs(x - c) < std::abs(best - c)) best = x;
        return best;
    }

    std::vector<std::vector<int>> cols_, building_;
};

} // namespace vgmplay::ui
