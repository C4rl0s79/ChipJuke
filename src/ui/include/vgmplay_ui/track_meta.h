// Metadane utworow do wyswietlania na liscie (tytul GD3 EN/JP, dlugosc). Tylko watek UI.
// Dane napelnia AppController wynikami workera (library::MetaCache); UI tylko czyta.
#pragma once

#include "vgmplay/format.hpp"
#include "vgmplay/playlist.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace vgmplay::ui {

struct TrackDisplay {
    std::string trackEn, trackJp;   // tytuly GD3 (UTF-8, puste = brak)
    std::uint64_t durationMs = 0;   // 0 = nieznana
    bool failed = false;            // odczyt metadanych sie nie udal (nie ponawiamy)
};

// Klucz sciezki bez rozrozniania wielkosci liter (sciezki Windows).
std::wstring pathKey(const std::filesystem::path& p);

class TrackMetaStore {
public:
    void put(const std::filesystem::path& p, TrackDisplay d) { items_[pathKey(p)] = std::move(d); }
    [[nodiscard]] const TrackDisplay* find(const std::filesystem::path& p) const
    {
        const auto it = items_.find(pathKey(p));
        return it == items_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::size_t size() const { return items_.size(); }
    void clear() { items_.clear(); }

private:
    std::unordered_map<std::wstring, TrackDisplay> items_;
};

// Tytul do wyswietlenia: GD3 w wybranym jezyku (z zapasowym drugim), potem Entry::title (z M3U/JSON),
// potem nazwa pliku. Wynik przechodzi przez sanitizeForDisplay (znaki kierunkowe/sterujace).
std::string entryTitle(const playlist::Entry& e, const TrackMetaStore* meta, format::Gd3Language lang);
// Dlugosc w ms: z metadanych, potem z wpisu; 0 = nieznana.
std::uint64_t entryDurationMs(const playlist::Entry& e, const TrackMetaStore* meta);

} // namespace vgmplay::ui
