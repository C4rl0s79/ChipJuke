// Wbudowana przegladarka plikow (E4): lista dyskow / katalogow / plikow .vgm .vgz .vgm.gz .m3u obslugiwana
// padem i klawiatura (okno systemowe nie jest dostepne z pada). Czysta logika bez ImGui - rysuje ja
// PlayerScreen (file_browser_screen.cpp).
//
// Bezpieczenstwo:
//  * listowanie wylacznie przez std::filesystem z std::error_code (bez wyjatkow),
//  * sciezki UNC (\\serwer\udzial) tylko gdy efektywne allowNetworkPaths; przestrzen urzadzen (\\?\, \\.\)
//    i URL zawsze odrzucane,
//  * nie podaza rekurencyjnie za dowiazaniami (listuje jeden katalog); skan folderu robi ScanJob (bez reparse),
//  * limit wpisow na katalog (domyslnie 20000) i limit czasu; listowanie na wlasnym watku, UI tylko odpytuje,
//  * pliki-placeholdery chmury sa oznaczane (bez czytania ich tresci).
//
// WATEK: FileBrowser i DirectoryLister naleza do watku UI; praca listujaca idzie na watku roboczym,
// ktory trzyma tylko wspolny stan (shared_ptr) - bezpieczny nawet po zniszczeniu obiektu.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vgmplay::ui {

enum class BrowserMode : std::uint8_t {
    AddFiles,    // pliki .vgm/.vgz/.vgm.gz (+ foldery) -> aktywna playlista
    AddFolder,   // tylko foldery
    ImportM3u,   // plik .m3u/.m3u8
    ExportM3u,   // tylko foldery; zapis do biezacego z automatyczna nazwa
};

enum class EntryKind : std::uint8_t { Recent, Drive, Parent, Dir, File };
enum class DriveKind : std::uint8_t { Unknown, Fixed, Removable, Network, Optical };

struct BrowserEntry {
    EntryKind kind = EntryKind::File;
    std::string name;                // UTF-8 (przed sanitizeForDisplay)
    std::filesystem::path path;      // pelna sciezka; Parent: katalog nadrzedny (pusta = lista dyskow)
    bool cloud = false;              // placeholder chmury: nie czytamy tresci
    bool link = false;               // dowiazanie / junction (nie podazamy za nim rekurencyjnie)
    DriveKind drive = DriveKind::Unknown;
    std::uint64_t size = 0;
};

inline constexpr std::size_t kBrowserMaxEntries = 20000;
inline constexpr std::size_t kMaxRecentFolders = 8;

struct ListOptions {
    BrowserMode mode = BrowserMode::AddFiles;
    bool allowNetwork = false;
    std::size_t maxEntries = kBrowserMaxEntries;
    std::size_t maxVisited = 400000;                       // ile pozycji katalogu przegladamy (takze pominiete)
    std::chrono::milliseconds timeLimit{4000};
};

struct Listing {
    std::filesystem::path dir;
    std::vector<BrowserEntry> entries;   // Parent, katalogi, pliki (posortowane naturalnie, bez wielkosci liter)
    bool truncated = false;              // osiagnieto limit wpisow lub przegladanych pozycji
    bool timedOut = false;               // przekroczono limit czasu (wynik czesciowy)
    bool cancelled = false;
    bool blocked = false;                // sciezka sieciowa/urzadzenia bez zgody
    std::string error;                   // pusty = ok (komunikat systemowy, po angielsku)
};

// ---- czyste funkcje (testowane) ----

enum class PathClass : std::uint8_t { Local, Network, Rejected };
[[nodiscard]] PathClass classifyPath(const std::filesystem::path& p);
[[nodiscard]] bool isPathAllowed(const std::filesystem::path& p, bool allowNetwork);

// Czy plik o tej nazwie pojawia sie na liscie w danym trybie (rozszerzenia bez rozrozniania wielkosci).
[[nodiscard]] bool matchesMode(const std::filesystem::path& filename, BrowserMode mode);
[[nodiscard]] bool listsFiles(BrowserMode mode);   // AddFiles, ImportM3u
[[nodiscard]] bool canMark(BrowserMode mode);      // AddFiles (pliki), AddFolder (foldery)

// Porownanie naturalne bez wielkosci liter ("a2" < "a10"). <0, 0, >0.
[[nodiscard]] int naturalCompare(const std::string& a, const std::string& b);

// Katalog nadrzedny; pusta sciezka = lista dyskow (korzen dysku, korzen udzialu).
[[nodiscard]] std::filesystem::path parentOf(const std::filesystem::path& dir);
[[nodiscard]] bool isRootLevel(const std::filesystem::path& dir);   // dysk lub udzial UNC

// Lista "ostatnich": najnowszy na poczatku, bez duplikatow (bez rozrozniania wielkosci), max `maxCount`.
[[nodiscard]] std::vector<std::string> pushRecentFolder(std::vector<std::string> list, const std::string& folderUtf8,
                                                        std::size_t maxCount = kMaxRecentFolders);

// Nazwa pliku z nazwy playlisty: bez znakow niedozwolonych, zarezerwowanych nazw Windows, konczacych
// kropek/spacji; pusta -> "playlist". Maks. 100 bajtow UTF-8 (nie ucina w srodku znaku).
[[nodiscard]] std::string sanitizeFileName(const std::string& name);
// dir/stem+ext, a gdy istnieje: "stem (2)ext", "(3)"... Pusta sciezka = brak wolnej nazwy (limit 999).
[[nodiscard]] std::filesystem::path uniqueFilePath(const std::filesystem::path& dir, const std::string& stemUtf8,
                                                   const std::string& extUtf8);

[[nodiscard]] std::uint32_t logicalDriveMask();   // GetLogicalDrives (0 poza Windows)
[[nodiscard]] std::vector<BrowserEntry> driveEntries(std::uint32_t mask);
// Widok "Ten komputer": ostatnie foldery (bez UNC bez zgody) + dyski.
[[nodiscard]] std::vector<BrowserEntry> rootEntries(const std::vector<std::string>& recent, bool allowNetwork,
                                                    std::uint32_t driveMask);

// Blokujace listowanie jednego katalogu (wolane z watku roboczego lub testow).
[[nodiscard]] Listing listDirectory(const std::filesystem::path& dir, const ListOptions& options,
                                    const std::atomic<bool>* cancel = nullptr);

// Asynchroniczne listowanie: "ostatnie zadanie wygrywa" - wynik poprzedniego jest porzucany.
class DirectoryLister {
public:
    DirectoryLister() = default;
    ~DirectoryLister() { cancel(); }
    DirectoryLister(const DirectoryLister&) = delete;
    DirectoryLister& operator=(const DirectoryLister&) = delete;

    void request(const std::filesystem::path& dir, const ListOptions& options);
    void cancel();
    [[nodiscard]] bool busy() const;
    [[nodiscard]] std::optional<Listing> poll();   // gotowy wynik ostatniego zadania (raz)

private:
    struct Shared {
        std::atomic<bool> cancel{false};
        std::mutex mutex;
        std::optional<Listing> result;
        bool done = false;
    };
    std::shared_ptr<Shared> cur_;
};

// ---- maszyna stanow przegladarki ----

struct BrowserOutcome {
    enum class Kind : std::uint8_t { None, Close, AddPaths, Import, Export };
    Kind kind = Kind::None;
    std::vector<std::filesystem::path> paths;   // AddPaths: pliki i/lub foldery; Import/Export: jeden plik
    std::filesystem::path folder;               // biezacy katalog przy zatwierdzeniu (do listy ostatnich)
};

class FileBrowser {
public:
    // `recent`: ostatnie foldery (UTF-8). Start w pierwszym istniejacym, inaczej lista dyskow.
    // `exportName`: nazwa playlisty (tryb ExportM3u).
    void open(BrowserMode mode, const std::vector<std::string>& recent, bool allowNetwork,
              const std::string& exportName = {});
    void close();
    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] BrowserMode mode() const { return mode_; }

    // Raz na klatke: odbiera wynik listowania.
    void tick();
    [[nodiscard]] bool loading() const { return loading_; }

    [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }
    [[nodiscard]] bool atRoot() const { return dir_.empty(); }
    [[nodiscard]] const std::vector<BrowserEntry>& entries() const { return entries_; }
    [[nodiscard]] int selection() const { return sel_; }
    [[nodiscard]] int scrollTop() const { return top_; }
    [[nodiscard]] const Listing& status() const { return status_; }
    [[nodiscard]] bool networkBlocked() const { return netBlocked_; }   // ostatnia nawigacja odrzucona (UNC bez zgody)
    [[nodiscard]] bool isMarked(int index) const;
    [[nodiscard]] std::size_t markedCount() const;
    [[nodiscard]] const std::string& exportName() const { return exportName_; }
    // Nazwa pliku, pod jaka zapisze ExportM3u (z uwzglednieniem istniejacych); pusta = brak / nie dotyczy.
    [[nodiscard]] const std::filesystem::path& exportTarget() const { return exportTarget_; }

    void setVisibleRows(int n);
    void select(int index);
    void move(int delta);
    void page(int direction);          // +1 w dol, -1 w gore
    void scrollBy(int rows);           // kolko myszy: przesuwa okno bez zmiany zaznaczenia

    BrowserOutcome activate();         // A / Enter
    BrowserOutcome back();             // B / Esc: poziom wyzej, na liscie dyskow zamyka
    BrowserOutcome enterSelected();    // Prawo: wejscie do katalogu (bez wyboru plikow)
    BrowserOutcome useHere();          // X: dodaj biezacy folder / zapisz tutaj
    void toggleMark();                 // Y
    void navigate(const std::filesystem::path& dir);   // pusta = lista dyskow
    void reload();

    // Dyski podstawiane w testach (domyslnie: logicalDriveMask()).
    void setDriveMaskForTest(std::uint32_t mask) { driveMask_ = mask; driveMaskSet_ = true; }
    void setLimitsForTest(std::size_t maxEntries, std::chrono::milliseconds limit)
    {
        maxEntries_ = maxEntries;
        timeLimit_ = limit;
    }

private:
    void showRoot();
    void ensureVisible();
    void clampSelection();
    [[nodiscard]] bool selectable(const BrowserEntry& e) const;

    bool open_ = false;
    BrowserMode mode_ = BrowserMode::AddFiles;
    bool allowNetwork_ = false;
    std::string exportName_;
    std::vector<std::string> recent_;
    std::filesystem::path dir_;
    std::vector<BrowserEntry> entries_;
    std::vector<bool> marks_;
    Listing status_;
    int sel_ = 0;
    int top_ = 0;
    int visibleRows_ = 10;
    bool loading_ = false;
    bool initial_ = false;               // pierwsze listowanie po open(): porazka -> lista dyskow
    bool netBlocked_ = false;
    std::filesystem::path exportTarget_;
    std::filesystem::path reselect_;     // po wyjsciu poziom wyzej: zaznacz katalog, z ktorego wyszlismy
    std::uint32_t driveMask_ = 0;
    bool driveMaskSet_ = false;
    std::size_t maxEntries_ = kBrowserMaxEntries;
    std::chrono::milliseconds timeLimit_{4000};
    DirectoryLister lister_;
};

} // namespace vgmplay::ui
