// vgmplay - kontrakt: playlist.hpp
// Wiele nazwanych playlist, model edycji, JSON oraz import/eksport M3U/M3U8.
//
// WATEK: PlaylistModel tylko z watku UI (bez synchronizacji). Funkcje JSON/M3U dzialaja na
// lancuchach (czyste, bez I/O) - bezpieczne z workera. Funkcje plikowe (loadPlaylistsFile/
// savePlaylistsFile) sa BLOKUJACE: tylko worker. Zapis atomowy (plik tymczasowy + rename).
// Sciezki w pamieci: std::filesystem::path; w JSON/M3U: UTF-8 z '/' jako separatorem.
// Walidacja (odrzucanie URL, wyjscia poza dozwolony katalog) - w IMPLEMENTACJI; tu kontrakt.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vgmplay/core.hpp"

namespace vgmplay::playlist {

inline constexpr std::uint32_t kPlaylistSchemaVersion = 1;

struct Entry {
    std::filesystem::path path;
    std::string title;             // pamiec podreczna tytulu (UTF-8), pusty = nieznany
    std::uint64_t durationMs = 0;  // 0 = nieznana
};

struct Playlist {
    std::string name;              // unikalna (porownanie bez rozrozniania wielkosci liter)
    std::vector<Entry> entries;
    // Indeks biezacego wpisu; -1 = brak. Utrzymywany przez operacje modelu.
    std::int64_t cursor = -1;
};

enum class RepeatMode : std::uint8_t { Off = 0, One, All };

class PlaylistModel {
public:
    // --- zbior playlist ---
    [[nodiscard]] std::size_t playlistCount() const noexcept;
    [[nodiscard]] const Playlist& playlist(std::size_t index) const;   // UB poza zakresem
    [[nodiscard]] Result<std::size_t> addPlaylist(std::string name);   // InvalidArgument: pusta/duplikat
    [[nodiscard]] Result<void> removePlaylist(std::size_t index);
    [[nodiscard]] Result<void> renamePlaylist(std::size_t index, std::string name);
    [[nodiscard]] Result<void> movePlaylist(std::size_t from, std::size_t to);
    [[nodiscard]] std::size_t activeIndex() const noexcept;            // zawsze < count (gdy count > 0)
    [[nodiscard]] Result<void> setActive(std::size_t index);

    // --- edycja wpisow (InvalidArgument przy zlych indeksach; kursor sledzi wpis) ---
    [[nodiscard]] Result<void> insert(std::size_t list, std::size_t pos, std::span<const Entry> entries);
    [[nodiscard]] Result<void> remove(std::size_t list, std::size_t first, std::size_t count);
    // Przenosi [first, first+count) tak, by zaczynal sie od `to` (indeks PO usunieciu zakresu).
    [[nodiscard]] Result<void> move(std::size_t list, std::size_t first, std::size_t count, std::size_t to);
    // Deterministyczne tasowanie (std::mt19937_64 z seed); kursor podaza za biezacym wpisem.
    [[nodiscard]] Result<void> shuffle(std::size_t list, std::uint64_t seed);

    // --- kursor ---
    [[nodiscard]] Result<void> setCursor(std::size_t list, std::int64_t index);   // -1 dozwolone
    // Przesuwa kursor aktywnej playlisty; zwraca nowy indeks lub NotFound (koniec przy Off).
    [[nodiscard]] Result<std::size_t> next(RepeatMode repeat);
    [[nodiscard]] Result<std::size_t> previous(RepeatMode repeat);

private:
    std::vector<Playlist> lists_;
    std::size_t active_ = 0;
};

// ---- JSON (schemaVersion: kPlaylistSchemaVersion) ----
// saveJson: UTF-8, deterministyczny. loadJson: ParseError przy zlej skladni,
// UnsupportedSchema przy nowszej wersji; limity: <= 1000 playlist, <= 100000 wpisow na liste.
[[nodiscard]] std::string saveJson(const PlaylistModel& model);
[[nodiscard]] Result<PlaylistModel> loadJson(std::string_view json);

[[nodiscard]] Result<PlaylistModel> loadPlaylistsFile(const std::filesystem::path& file);
[[nodiscard]] Result<void> savePlaylistsFile(const std::filesystem::path& file, const PlaylistModel& model);

// ---- M3U / M3U8 ----
struct M3uImportOptions {
    // Katalog pliku M3U; sciezki wzgledne sa rozwiazywane wzgledem niego.
    std::filesystem::path baseDir;
    // Katalog dozwolony: wynik (po normalizacji, bez '..', bez dowiazan wychodzacych)
    // musi sie w nim miescic. Pusty = baseDir. Poza nim -> pozycja odrzucona + Warning.
    std::filesystem::path allowedRoot;
    bool allowAbsolutePaths = false;   // true: dozwolone sciezki absolutne (nadal w allowedRoot)
    bool assumeUtf8 = true;            // brak BOM i brak M3U8: true = UTF-8, false = strona kodowa systemu
    std::uint32_t maxEntries = 100000;
};

struct M3uImportResult {
    std::vector<Entry> entries;
    std::vector<Warning> warnings;     // m.in. odrzucone URL (http://, file://, UNC), wyjscie poza root
    std::uint32_t rejected = 0;
};

// Odrzuca: URL (dowolny schemat://), sciezki UNC/urzadzen (\\?\, \\.\), '..' wychodzace poza
// allowedRoot, nieistniejace pliki NIE (sprawdzanie istnienia nie jest zadaniem importu).
// Linie #EXTM3U/#EXTINF: tytul/czas trafiaja do Entry; inne komentarze ignorowane.
[[nodiscard]] Result<M3uImportResult> importM3u(std::string_view text, const M3uImportOptions& options);

struct M3uExportOptions {
    std::filesystem::path baseDir;     // sciezki relatywizowane do baseDir, gdy sie da
    bool utf8 = true;                  // true = M3U8 (UTF-8, bez BOM), false = M3U w stronie kodowej systemu
    bool extInf = true;                // dolacz #EXTM3U / #EXTINF
};
[[nodiscard]] std::string exportM3u(const Playlist& list, const M3uExportOptions& options);

} // namespace vgmplay::playlist
