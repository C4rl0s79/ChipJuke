// vgmplay - kontrakt: library.hpp
// Asynchroniczny skan katalogu w poszukiwaniu plikow VGM/VGZ. NIE parsuje zawartosci plikow.
//
// WATEK: ScanJob tworzy WLASNY watek roboczy (start()); UI tylko odpytuje (progress/done/
// takeResult/cancel) - zadna z tych metod nie blokuje. Obiekt nalezy do wolajacego; destruktor
// anuluje i DOLACZA watek (moze krotko zablokowac - najwyzej jeden katalog/plik).
//
// Bezpieczenstwo: nie podaza za punktami reparse (dowiazania/junctions) gdy
// followReparsePoints=false (domyslnie), nie wychodzi poza root, liczy glebokosc i pliki,
// bledy dostepu zbiera jako Warning i kontynuuje; brak wyjatkow.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vgmplay/core.hpp"

namespace vgmplay::library {

struct ScanOptions {
    bool recursive = true;
    bool followReparsePoints = false;
    std::uint32_t maxDepth = 32;          // 0 = tylko root
    std::uint32_t maxFiles = 100000;      // limit wynikow
    // Rozszerzenia (male litery, z kropka); porownanie bez rozrozniania wielkosci.
    std::vector<std::string> extensions{".vgm", ".vgz", ".vgm.gz"};
};

struct FoundFile {
    std::filesystem::path path;           // sciezka absolutna
    std::uint64_t sizeBytes = 0;
    // [dodatek E2-sec] dwFileAttributes z systemu plikow w chwili skanu (0 = nieznane).
    std::uint32_t attributes = 0;
};

// [dodatek E2-sec] Atrybuty plikow "w chmurze" (OneDrive / Cloud Files / HSM): odczyt tresci wymusilby
// pobranie z sieci. Wartosci = FILE_ATTRIBUTE_OFFLINE, ..._RECALL_ON_OPEN, ..._RECALL_ON_DATA_ACCESS.
inline constexpr std::uint32_t kAttrOffline = 0x00001000;
inline constexpr std::uint32_t kAttrRecallOnOpen = 0x00040000;
inline constexpr std::uint32_t kAttrRecallOnDataAccess = 0x00400000;
[[nodiscard]] constexpr bool isCloudPlaceholder(std::uint32_t attributes) noexcept
{
    return (attributes & (kAttrOffline | kAttrRecallOnOpen | kAttrRecallOnDataAccess)) != 0;
}

struct ScanResult {
    std::vector<FoundFile> files;         // posortowane leksykograficznie (stabilnie)
    std::vector<Warning> warnings;        // ograniczone do ~100 wpisow
    std::uint64_t directoriesVisited = 0;
    std::uint64_t reparsePointsSkipped = 0;
    bool truncatedByMaxFiles = false;
    bool truncatedByMaxDepth = false;
    bool cancelled = false;
};

struct ScanProgress {
    std::uint64_t filesFound = 0;
    std::uint64_t directoriesVisited = 0;
};

class ScanJob {
public:
    // Uruchamia skan w tle. Blad (np. root nie istnieje / nie jest katalogiem) zwracany od razu.
    [[nodiscard]] static Result<std::unique_ptr<ScanJob>> start(const std::filesystem::path& root,
                                                                 const ScanOptions& options = {});
    ~ScanJob();
    ScanJob(const ScanJob&) = delete;
    ScanJob& operator=(const ScanJob&) = delete;

    void cancel() noexcept;                         // = token.requestCancel()
    [[nodiscard]] bool done() const noexcept;       // true, gdy wynik gotowy (takze po anulowaniu)
    [[nodiscard]] ScanProgress progress() const noexcept;   // atomowe odczyty
    // Zwraca wynik dokladnie raz (potem nullopt); przed done() zawsze nullopt.
    [[nodiscard]] std::optional<ScanResult> takeResult();

private:
    ScanJob();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// [dodatek E2] Metadane utworow + cache. ScanJob powyzej NIE parsuje plikow; metadane czyta
// osobno readTrackMeta (pelna walidacja przez loader::loadTrackFromFile - tylko ona daje
// PRAWDZIWA dlugosc z pre-skanu strumienia; naglowek bywa falszywy). BLOKUJACE: tylko worker.
// ---------------------------------------------------------------------------

// Odcisk pliku do uniewazniania cache: rozmiar + czas modyfikacji (FILETIME, 100 ns od 1601).
struct FileStamp {
    std::uint64_t sizeBytes = 0;
    std::uint64_t mtime = 0;
    bool operator==(const FileStamp&) const = default;
};
[[nodiscard]] Result<FileStamp> statFile(const std::filesystem::path& path);

struct TrackMeta {
    std::filesystem::path path;
    FileStamp stamp;
    std::uint64_t durationMs = 0;    // jedno przejscie strumienia (od poczatku do 0x66), ms
    std::uint64_t loopMs = 0;        // dlugosc petli; 0 = brak (rowniez gdy loader wylaczyl petle)
    bool hasGd3 = false;
    // Tytuly GD3 (UTF-8; puste = brak). Wybor jezyka (Settings::ui.gd3Language) po stronie UI.
    std::string trackEn, trackJp, gameEn, gameJp, systemEn, systemJp, authorEn, authorJp;
};

// Czyta plik przez loader. Bledy loadera przekazywane bez zmian (Cancelled przy cancel).
// Pliki z isCloudPlaceholder(atrybuty) sa POMIJANE (brak pobierania z sieci): wynik to Error
// ErrorCode::CloudPlaceholder, rozpoznawany przez isCloudPlaceholderError(). `allowCloudDownload=true`
// ("pobierz mimo to") czyta plik normalnie (system pobierze tresc). Ostrzezenie skanu to nadal
// WarningCode::Other z komunikatem "cloud placeholder ..." (brak dedykowanego WarningCode).
[[nodiscard]] Result<TrackMeta> readTrackMeta(const std::filesystem::path& path, const CancelToken& cancel = {},
                                              bool allowCloudDownload = false);
[[nodiscard]] bool isCloudPlaceholderError(const Error& e) noexcept;

// Cache metadanych: klucz = sciezka (bez rozrozniania wielkosci liter) + FileStamp. Nie jest
// watkowo-bezpieczny (uzywaj z jednego workera). Limit 200000 wpisow (nadmiar nie jest dodawany).
// Plik JSON (schemaVersion 1) lezy obok ustawien, np. library-cache.json; uszkodzony plik =
// blad, a wywolujacy zaczyna od pustego cache (nic waznego tam nie ma, wiec bez kopii .bak).
class MetaCache {
public:
    [[nodiscard]] std::optional<TrackMeta> find(const std::filesystem::path& path, const FileStamp& stamp) const;
    void put(const TrackMeta& meta);
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    // Zwraca z cache albo czyta i dopisuje (stat + readTrackMeta).
    // Trafienie w cache nie czyta pliku (nie wymusza pobrania); przy braku wpisu obowiazuje regula chmury
    // z readTrackMeta.
    [[nodiscard]] Result<TrackMeta> getOrRead(const std::filesystem::path& path, const CancelToken& cancel = {},
                                              bool allowCloudDownload = false);

    [[nodiscard]] std::string toJson() const;
    // Limity parsera: zagniezdzenie <= 8, <= 4 mln wartosci; durationMs/loopMs <= 24 h; tytuly czyszczone
    // (max 512 B). Sciezki UNC odrzucane, chyba ze allowNetworkPaths (URL/urzadzenia zawsze).
    [[nodiscard]] static Result<MetaCache> fromJson(std::string_view json, bool allowNetworkPaths = false);

private:
    std::unordered_map<std::wstring, TrackMeta> items_;   // klucz = sciezka malymi literami
};
[[nodiscard]] Result<MetaCache> loadMetaCacheFile(const std::filesystem::path& file, bool allowNetworkPaths = false);   // brak pliku = pusty
[[nodiscard]] Result<void> saveMetaCacheFile(const std::filesystem::path& file, const MetaCache& cache);

} // namespace vgmplay::library
