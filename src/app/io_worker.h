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
    bool cloud = false;                       // plik w chmurze (placeholder) - pominiety
};

struct ImportDone {
    std::filesystem::path file;
    bool tooBig = false;
    std::optional<Result<playlist::M3uImportResult>> result;   // brak przy tooBig
};

struct ExportDone {
    std::filesystem::path file;      // zadana nazwa (propozycja z przegladarki)
    std::filesystem::path written;   // faktycznie utworzony plik ("nazwa (2).m3u8", gdy zajeta); pusty przy bledzie
    std::optional<Error> error;
};

// Wynik rozpoznania sciezek (plik / katalog) na watku roboczym - UI nie dotyka dysku.
struct ClassifiedPath {
    std::filesystem::path path;
    bool isDirectory = false;
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
    std::vector<std::vector<ClassifiedPath>> classified;   // jedna paczka na wywolanie classifyPaths()
    std::vector<std::string> classifiedTags;               // tag przekazany do classifyPaths(); ten sam indeks co `classified`
    std::vector<SaveFailure> saveFailures;
    [[nodiscard]] bool empty() const
    {
        return !playlists && meta.empty() && imports.empty() && exports.empty() && classified.empty() && saveFailures.empty();
    }
};

class IoWorker {
public:
    struct Paths {
        std::filesystem::path settings;    // pusty = brak zapisu ustawien
        std::filesystem::path playlists;   // pusty = brak wczytywania/zapisu playlist
        std::filesystem::path cache;       // pusty = cache metadanych tylko w pamieci
        bool allowNetworkPaths = false;    // sciezki UNC w playlistach i cache
    };
    IoWorker(Paths paths, std::function<void()> wake);
    ~IoWorker();
    IoWorker(const IoWorker&) = delete;
    IoWorker& operator=(const IoWorker&) = delete;

    void loadPlaylists();
    void saveSettings(settings::Settings s);
    void savePlaylists(playlist::PlaylistModel m);
    void queueMeta(std::vector<std::filesystem::path> paths);
    void queueMetaDownload(std::filesystem::path path);   // "pobierz mimo to" (plik w chmurze)
    void importM3u(std::filesystem::path file);
    // Zapis M3U pod nazwa `file`. Domyslnie (overwrite=false, wbudowana przegladarka): gdy zajeta - "nazwa (2).m3u8"...
    // (limit 999), nigdy nie nadpisuje istniejacego pliku. overwrite=true (systemowe okno zapisu, gdzie uzytkownik potwierdzil
    // zastapienie): atomowa zamiana pliku. Faktyczna nazwa wraca w ExportDone::written.
    void exportM3u(std::filesystem::path file, std::string text, bool overwrite = false);
    // Rozpoznanie plik/katalog dla listy sciezek (dysk tylko na watku workera); wynik: IoResults::classified.
    // `tag` wraca bez zmian w IoResults::classifiedTags (np. nazwa playlisty docelowej z chwili zlecenia).
    void classifyPaths(std::vector<std::filesystem::path> paths, std::string tag = {});
    [[nodiscard]] IoResults take();
    // [E3-sec] Efektywna zgoda na sciezki UNC (playlisty, cache); watkowo-bezpieczne (atomik), dziala od
    // nastepnej operacji workera, bez restartu. Wywolywane z watku UI po zmianie ustawienia.
    void setAllowNetworkPaths(bool allow) noexcept { allowNetwork_.store(allow, std::memory_order_relaxed); }
    [[nodiscard]] bool allowNetworkPaths() const noexcept { return allowNetwork_.load(std::memory_order_relaxed); }

private:
    void loop();
    void post(IoResults& add);

    Paths paths_;   // paths_.allowNetworkPaths = tylko wartosc poczatkowa; biezaca: allowNetwork_
    std::atomic<bool> allowNetwork_;
    std::function<void()> wake_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false;
    bool loadPlaylists_ = false;
    std::optional<settings::Settings> pendingSettings_;
    std::optional<playlist::PlaylistModel> pendingPlaylists_;
    std::deque<std::filesystem::path> importQueue_;
    struct ExportJob {
        std::filesystem::path file;
        std::string text;
        bool overwrite = false;
    };
    std::deque<ExportJob> exportQueue_;
    std::deque<std::pair<std::vector<std::filesystem::path>, std::string>> classifyQueue_;
    std::deque<std::filesystem::path> metaQueue_;
    std::deque<std::filesystem::path> downloadQueue_;
    std::unordered_set<std::wstring> metaQueued_;
    IoResults results_;
    std::atomic<bool> wakePending_{false};
    CancelToken cancel_;
    std::thread thread_;   // ostatni: startuje po zainicjowaniu reszty
};

} // namespace vgmplay::app
