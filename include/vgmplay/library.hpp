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
#include <vector>

#include "vgmplay/core.hpp"

namespace vgmplay::library {

struct ScanOptions {
    bool recursive = true;
    bool followReparsePoints = false;
    std::uint32_t maxDepth = 32;          // 0 = tylko root
    std::uint32_t maxFiles = 100000;      // limit wynikow
    // Rozszerzenia (male litery, z kropka); porownanie bez rozrozniania wielkosci.
    std::vector<std::string> extensions{".vgm", ".vgz"};
};

struct FoundFile {
    std::filesystem::path path;           // sciezka absolutna
    std::uint64_t sizeBytes = 0;
};

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

} // namespace vgmplay::library
