// IoWorker: watek roboczy dla wszystkiego, co blokuje na dysku, a nie jest ladowaniem utworu:
// zapis ustawien i playlist (najnowszy stan wygrywa), wczytanie playlists.json, import/eksport M3U,
// metadane utworow (MetaCache, library-cache.json). UI tylko zleca i odbiera wyniki w take().
//
// WATKI: metody zlecajace i take() - watek UI. Wlasciciel MetaCache i wszystkich operacji plikowych -
// ten jeden watek. Wyniki leza w kolejce pod mutexem; wake() (dowolny watek) budzi petle UI.
// Zamkniecie (destruktor): dopisuje zaleglosci zapisu (ustawienia, playlisty, cache), porzuca reszte.
#pragma once

#include "vgmplay/library.hpp"
#include "vgmplay/playlist.hpp"
#include "vgmplay/settings.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vgmplay::app {

inline constexpr std::uintmax_t kMaxM3uBytes = 16u * 1024u * 1024u;   // limit rozmiaru pliku M3U

struct MetaResult {
    std::filesystem::path path;
    std::optional<library::TrackMeta> meta;   // brak = odczyt sie nie udal
};

struct ImportDone {
    std::filesystem::path file;
    bool tooBig = false;
    std::optional<Result<playlist::M3uImportResult>> result;   // brak przy tooBig
};

struct ExportDone {
    std::filesystem::path file;
    std::optional<Error> error;
};

struct SaveFailure {
    std::string what;   // "settings" | "playlists"
    Error error;
};

struct IoResults {
    std::optional<Result<playlist::PlaylistModel>> playlists;
    std::vector<MetaResult> meta;
    std::vector<ImportDone> imports;
    std::vector<ExportDone> exports;
    std::vector<SaveFailure> saveFailures;
    [[nodiscard]] bool empty() const
    {
        return !playlists && meta.empty() && imports.empty() && exports.empty() && saveFailures.empty();
    }
};

class IoWorker {
public:
    struct Paths {
        std::filesystem::path settings;    // pusty = brak zapisu ustawien
        std::filesystem::path playlists;   // pusty = brak wczytywania/zapisu playlist
        std::filesystem::path cache;       // pusty = cache metadanych tylko w pamieci
    };
    IoWorker(Paths paths, std::function<void()> wake);
    ~IoWorker();
    IoWorker(const IoWorker&) = delete;
    IoWorker& operator=(const IoWorker&) = delete;

    void loadPlaylists();
    void saveSettings(settings::Settings s);
    void savePlaylists(playlist::PlaylistModel m);
    void queueMeta(std::vector<std::filesystem::path> paths);
    void importM3u(std::filesystem::path file);
    void exportM3u(std::filesystem::path file, std::string text);
    [[nodiscard]] IoResults take();

private:
    void loop();
    void post(IoResults& add);

    Paths paths_;
    std::function<void()> wake_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false;
    bool loadPlaylists_ = false;
    std::optional<settings::Settings> pendingSettings_;
    std::optional<playlist::PlaylistModel> pendingPlaylists_;
    std::deque<std::filesystem::path> importQueue_;
    std::deque<std::pair<std::filesystem::path, std::string>> exportQueue_;
    std::deque<std::filesystem::path> metaQueue_;
    std::unordered_set<std::wstring> metaQueued_;
    IoResults results_;
    std::atomic<bool> wakePending_{false};
    CancelToken cancel_;
    std::thread thread_;   // ostatni: startuje po zainicjowaniu reszty
};

} // namespace vgmplay::app
