// Historia ostatnich klatek master L/R z silnika (ScopeBlock) do rysowania oscyloskopu
// zsynchronizowanego z dzwiekiem. Czysta struktura danych (bez ImGui), tylko watek UI.
//
// Bloki niosa epoch i startFrame (pozycja w utworze). Nowsza epoka zeruje historie, starsza jest
// odrzucana; luka w pozycji (np. gdy pierscien scope byl pelny) rowniez zeruje historie.
// extract() zwraca okno zaczynajace sie od pozycji slyszanej (lub pierwszego narastajacego
// przejscia przez zero po niej), wiec obraz odpowiada temu, co wlasnie gra.
#pragma once

#include "vgmplay/layout.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vgmplay::ui {

class ScopeBuffer {
public:
    static constexpr std::size_t kCapacity = 1u << 15; // klatek (potega 2)

    void reset();
    void push(const ScopeBlock& block);

    std::uint64_t epoch() const { return epoch_; }
    bool empty() const { return end_ == begin_; }
    std::uint64_t beginFrame() const { return begin_; }
    std::uint64_t endFrame() const { return end_; }

    // Wypelnia l/r `count` klatkami. `audibleFrame` = pozycja slyszana. Trigger: pierwsze przejscie
    // sumy L+R z ujemnej na nieujemna w [audible, audible + count). Zwraca false, gdy brak danych.
    bool extract(std::uint64_t audibleFrame, std::size_t count, std::vector<float>& l,
                 std::vector<float>& r, std::uint64_t* startedAt = nullptr) const;

    // Szczyt |L| i |R| w oknie `count` klatek od pozycji slyszanej (okno cofane przy koncu danych).
    bool peak(std::uint64_t audibleFrame, std::size_t count, float& l, float& r) const;

private:
    float mono(std::uint64_t frame) const;

    std::vector<float> l_ = std::vector<float>(kCapacity, 0.0f);
    std::vector<float> r_ = std::vector<float>(kCapacity, 0.0f);
    std::uint64_t epoch_ = 0;
    std::uint64_t begin_ = 0; // pierwsza dostepna klatka
    std::uint64_t end_ = 0;   // nastepna oczekiwana klatka
};

} // namespace vgmplay::ui
