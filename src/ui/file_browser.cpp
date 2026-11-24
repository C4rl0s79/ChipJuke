#include "vgmplay_ui/file_browser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include "fs_util.hpp"   // wewnetrzny naglowek modulu settings: isReservedDeviceName, finalPathOf
#endif

namespace vgmplay::ui {
namespace fs = std::filesystem;
namespace {

// Atrybuty plikow "w chmurze" (te same stale co library::isCloudPlaceholder; ui nie zalezy od modulu library).
constexpr std::uint32_t kAttrOffline = 0x00001000;
constexpr std::uint32_t kAttrRecallOnOpen = 0x00040000;
constexpr std::uint32_t kAttrRecallOnDataAccess = 0x00400000;

std::string toUtf8(const fs::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path fromUtf8(const std::string& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

char lowerAscii(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string lowerAscii(std::string s)
{
    for (char& c : s) c = lowerAscii(c);
    return s;
}

// Znormalizowany klucz do porownan sciezek: male litery ASCII, '\\', bez koncowego separatora (poza "C:\").
std::string pathKey(std::string s)
{
    for (char& c : s) c = c == '/' ? '\\' : lowerAscii(c);
    while (s.size() > 3 && s.back() == '\\') s.pop_back();
    return s;
}

bool samePath(const fs::path& a, const fs::path& b) { return pathKey(toUtf8(a)) == pathKey(toUtf8(b)); }

bool endsWithNoCase(const std::wstring& s, const wchar_t* suffix)
{
    const std::wstring suf = suffix;
    if (s.size() < suf.size()) return false;
    for (std::size_t i = 0; i < suf.size(); ++i)
        if (std::towlower(s[s.size() - suf.size() + i]) != std::towlower(suf[i])) return false;
    return true;
}

bool skippedSystemName(const std::string& name)
{
    const std::string l = lowerAscii(name);
    return l == "$recycle.bin" || l == "system volume information";
}

std::uint32_t fileAttributes(const fs::path& p)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(p.c_str());
    return a == INVALID_FILE_ATTRIBUTES ? 0u : static_cast<std::uint32_t>(a);
#else
    (void)p;
    return 0;
#endif
}

bool isCloud(std::uint32_t attrs) { return (attrs & (kAttrOffline | kAttrRecallOnOpen | kAttrRecallOnDataAccess)) != 0; }

} // namespace

// ---------------------------------------------------------------- czyste funkcje

PathClass classifyPath(const fs::path& p)
{
    std::string n = toUtf8(p);
    if (n.find('\0') != std::string::npos) return PathClass::Rejected;
    for (char& c : n)
        if (c == '/') c = '\\';
    if (n.size() >= 2 && n[0] == '\\' && n[1] == '\\') {
        if (n.size() >= 3 && (n[2] == '?' || n[2] == '.')) return PathClass::Rejected;   // przestrzen urzadzen
        const std::string low = lowerAscii(n);
        const std::size_t hostEnd = low.find('\\', 2);
        if (hostEnd != std::string::npos && (low.compare(hostEnd + 1, 5, "pipe\\") == 0 || low.substr(hostEnd + 1) == "pipe"))
            return PathClass::Rejected;
        return PathClass::Network;
    }
    if (!n.empty() && std::isalpha(static_cast<unsigned char>(n[0]))) {   // schemat URL: "xxx://"
        std::size_t i = 1;
        while (i < n.size() && (std::isalnum(static_cast<unsigned char>(n[i])) || n[i] == '+' || n[i] == '-' || n[i] == '.')) ++i;
        if (i + 2 < n.size() && n[i] == ':' && n[i + 1] == '\\' && n[i + 2] == '\\') return PathClass::Rejected;
    }
    return PathClass::Local;
}

bool isPathAllowed(const fs::path& p, bool allowNetwork)
{
    switch (classifyPath(p)) {
    case PathClass::Local: return true;
    case PathClass::Network: return allowNetwork;
    case PathClass::Rejected: return false;
    }
    return false;
}

bool listsFiles(BrowserMode mode) { return mode == BrowserMode::AddFiles || mode == BrowserMode::ImportM3u; }
bool canMark(BrowserMode mode) { return mode == BrowserMode::AddFiles || mode == BrowserMode::AddFolder; }

bool matchesMode(const fs::path& filename, BrowserMode mode)
{
    const std::wstring n = filename.filename().wstring();
    switch (mode) {
    case BrowserMode::AddFiles:
        return endsWithNoCase(n, L".vgm") || endsWithNoCase(n, L".vgz") || endsWithNoCase(n, L".vgm.gz");
    case BrowserMode::ImportM3u: return endsWithNoCase(n, L".m3u") || endsWithNoCase(n, L".m3u8");
    default: return false;
    }
}

int naturalCompare(const std::string& a, const std::string& b)
{
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            std::size_t is = i, js = j;   // bez wiodacych zer
            while (is + 1 < ie && a[is] == '0') ++is;
            while (js + 1 < je && b[js] == '0') ++js;
            const std::size_t la = ie - is, lb = je - js;
            if (la != lb) return la < lb ? -1 : 1;
            const int c = a.compare(is, la, b, js, lb);
            if (c != 0) return c < 0 ? -1 : 1;
            i = ie;
            j = je;
            continue;
        }
        const char la = lowerAscii(static_cast<char>(ca)), lb = lowerAscii(static_cast<char>(cb));
        if (la != lb) return static_cast<unsigned char>(la) < static_cast<unsigned char>(lb) ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return a == b ? 0 : (a < b ? -1 : 1);   // stabilny porzadek dla nazw rozniacych sie tylko wielkoscia liter
}

bool isRootLevel(const fs::path& dir)
{
    if (dir.empty()) return true;
    std::size_t comps = 0;
    for (const fs::path& c : dir.relative_path())
        if (!c.empty() && c != L"." ) ++comps;
    return classifyPath(dir) == PathClass::Network ? comps <= 1 : comps == 0;
}

fs::path parentOf(const fs::path& dir)
{
    if (dir.empty() || isRootLevel(dir)) return {};
    fs::path q = dir;
    if (!q.has_filename()) q = q.parent_path();   // "C:\a\b\" -> "C:\a\b"
    return q.parent_path();
}

std::vector<std::string> pushRecentFolder(std::vector<std::string> list, const std::string& folderUtf8, std::size_t maxCount)
{
    if (folderUtf8.empty() || maxCount == 0) return list;
    const std::string key = pathKey(folderUtf8);
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& s) { return pathKey(s) == key; }), list.end());
    list.insert(list.begin(), folderUtf8);
    if (list.size() > maxCount) list.resize(maxCount);
    return list;
}

std::string sanitizeFileName(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7F || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos) {
            out += '_';
        } else if (c == 0xE2 && i + 2 < name.size() &&
                   ((static_cast<unsigned char>(name[i + 1]) == 0x80 &&
                     (static_cast<unsigned char>(name[i + 2]) == 0x8E || static_cast<unsigned char>(name[i + 2]) == 0x8F ||
                      (static_cast<unsigned char>(name[i + 2]) >= 0xAA && static_cast<unsigned char>(name[i + 2]) <= 0xAE))) ||
                    (static_cast<unsigned char>(name[i + 1]) == 0x81 &&
                     static_cast<unsigned char>(name[i + 2]) >= 0xA6 && static_cast<unsigned char>(name[i + 2]) <= 0xA9))) {
            out += '_';   // znaki kierunkowe (U+200E/F, U+202A-E, U+2066-9)
            i += 2;
        } else {
            out += static_cast<char>(c);
        }
    }
    if (out.size() > 100) {
        std::size_t n = 100;
        while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
        out.resize(n);
    }
    auto trim = [&] {
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
        std::size_t b = 0;
        while (b < out.size() && out[b] == ' ') ++b;
        out.erase(0, b);
    };
    trim();
    if (out.empty()) return "playlist";
    // Nazwy urzadzen (CON, NUL, COM0-9, LPT0-9, COM/LPT z indeksem gornym, CONIN$, CONOUT$; takze z rozszerzeniem
    // i spacjami przed kropka) - jedna wspolna definicja z fsutil.
    const auto wide = fsutil::utf8ToWide(out, false);
    if (wide && fsutil::isReservedDeviceName(*wide)) out.insert(0, "_");
    return out;
}

fs::path uniqueFilePath(const fs::path& dir, const std::string& stemUtf8, const std::string& extUtf8)
{
    for (int n = 1; n <= 999; ++n) {
        const std::string name = n == 1 ? stemUtf8 + extUtf8 : stemUtf8 + " (" + std::to_string(n) + ")" + extUtf8;
        const fs::path p = dir / fromUtf8(name);
        std::error_code ec;
        const bool exists = fs::exists(fs::symlink_status(p, ec));
        if (ec && ec != std::errc::no_such_file_or_directory) return {};
        if (!exists) return p;
    }
    return {};
}

std::uint32_t logicalDriveMask()
{
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetLogicalDrives());
#else
    return 0;
#endif
}

namespace {

DriveKind systemDriveKind(wchar_t letter)
{
#ifdef _WIN32
    const wchar_t root[4] = {letter, L':', L'\\', L'\0'};
    switch (GetDriveTypeW(root)) {
    case DRIVE_FIXED: return DriveKind::Fixed;
    case DRIVE_REMOVABLE: return DriveKind::Removable;
    case DRIVE_REMOTE: return DriveKind::Network;
    case DRIVE_CDROM: return DriveKind::Optical;
    default: break;
    }
#else
    (void)letter;
#endif
    return DriveKind::Unknown;
}

bool isRemoteDriveLetter(wchar_t letter) { return systemDriveKind(letter) == DriveKind::Network; }

bool hasDriveLetter(const std::wstring& w) { return w.size() >= 2 && w[1] == L':' && std::iswalpha(w[0]); }

bool startsWithNoCase(const std::wstring& s, const wchar_t* prefix)
{
    const std::wstring pre = prefix;
    if (s.size() < pre.size()) return false;
    for (std::size_t i = 0; i < pre.size(); ++i)
        if (std::towlower(s[i]) != std::towlower(pre[i])) return false;
    return true;
}

} // namespace

bool isNetworkLinkTarget(const std::wstring& target)
{
    std::wstring t = target;
    for (wchar_t& c : t)
        if (c == L'/') c = L'\\';
    if (startsWithNoCase(t, LR"(\??\UNC\)") || startsWithNoCase(t, LR"(\\?\UNC\)")) return true;
    if (startsWithNoCase(t, LR"(\\.\)")) return true;   // przestrzen urzadzen: traktujemy jak zablokowana
    if (startsWithNoCase(t, LR"(\??\)") || startsWithNoCase(t, LR"(\\?\)")) t.erase(0, 4);
    else if (t.size() >= 2 && t[0] == L'\\' && t[1] == L'\\') return true;   // \\serwer\udzial
    return hasDriveLetter(t) && isRemoteDriveLetter(static_cast<wchar_t>(std::towupper(t[0])));
}

#ifdef _WIN32
namespace {

// Czyta SUROWY cel dowiazania symbolicznego / junction (nazwa zastepcza z reparse point), BEZ podazania za nim
// (FILE_FLAG_OPEN_REPARSE_POINT otwiera sam znacznik, nie cel; nie ma kontaktu z siecia). std::filesystem::read_symlink
// obcina "\??\" i dla UNC zostawialoby "UNC\srv\sh", dlatego parsujemy bufor sami.
bool readReparseTarget(const fs::path& link, std::wstring& target)
{
    HANDLE h = CreateFileW(link.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    alignas(8) unsigned char buf[16 * 1024];
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf, sizeof buf, &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 8) return false;
    std::uint32_t tag = 0;
    std::memcpy(&tag, buf, 4);
    std::uint16_t dataLen = 0;
    std::memcpy(&dataLen, buf + 4, 2);
    if (8u + dataLen > got) return false;
    const unsigned char* d = buf + 8;   // za naglowkiem (tag, dlugosc, zarezerwowane)
    std::size_t pathOff = 0;
    if (tag == IO_REPARSE_TAG_SYMLINK) pathOff = 12;           // 4 x USHORT + ULONG Flags
    else if (tag == IO_REPARSE_TAG_MOUNT_POINT) pathOff = 8;   // 4 x USHORT
    else return false;
    if (dataLen < pathOff) return false;
    std::uint16_t subOff = 0, subLen = 0;
    std::memcpy(&subOff, d, 2);
    std::memcpy(&subLen, d + 2, 2);
    if (pathOff + static_cast<std::size_t>(subOff) + subLen > dataLen || (subLen & 1u) != 0) return false;
    target.assign(reinterpret_cast<const wchar_t*>(d + pathOff + subOff), subLen / 2u);
    return true;
}

} // namespace
#endif

bool linkPointsToNetwork(const fs::path& p)
{
#ifdef _WIN32
    fs::path cur = p;
    for (int hop = 0; hop < 8; ++hop) {
        const DWORD a = GetFileAttributesW(cur.c_str());   // bez podazania za ostatnim skladnikiem
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return false;
        std::wstring w;
        if (!readReparseTarget(cur, w) || w.empty()) return false;
        if (isNetworkLinkTarget(w)) return true;
        if (startsWithNoCase(w, LR"(\??\)") || startsWithNoCase(w, LR"(\\?\)")) w.erase(0, 4);
        fs::path next(w);
        if (next.is_relative()) next = cur.parent_path() / next;
        cur = next.lexically_normal();
    }
#else
    (void)p;
#endif
    return false;
}

bool isNetworkRedirected(const fs::path& dir)
{
#ifdef _WIN32
    const std::wstring w = dir.wstring();
    if (hasDriveLetter(w) && isRemoteDriveLetter(static_cast<wchar_t>(std::towupper(w[0])))) return true;
    if (linkPointsToNetwork(dir)) return true;
    // Dowiazanie w srodku sciezki (C:\a\link\b): rozwiazanie przez uchwyt lokalnego katalogu.
    fs::path fin;
    DWORD err = 0;
    if (fsutil::finalPathOf(dir, fin, err) && isNetworkLinkTarget(fin.wstring())) return true;
#else
    (void)dir;
#endif
    return false;
}

std::vector<BrowserEntry> driveEntries(std::uint32_t mask, bool allowNetwork, DriveKind (*kindOf)(wchar_t))
{
    std::vector<BrowserEntry> out;
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        BrowserEntry e;
        e.kind = EntryKind::Drive;
        const char letter = static_cast<char>('A' + i);
        e.name = std::string(1, letter) + ":\\";
        e.path = fs::path(std::wstring(1, static_cast<wchar_t>(letter)) + L":\\");
        e.drive = (kindOf ? kindOf : systemDriveKind)(static_cast<wchar_t>(letter));
        // Dysk zmapowany na udzial sieciowy (net use) omija klasyfikacje UNC po tekscie sciezki: blokujemy go jak UNC.
        e.blocked = !allowNetwork && e.drive == DriveKind::Network;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<BrowserEntry> rootEntries(const std::vector<std::string>& recent, bool allowNetwork, std::uint32_t driveMask)
{
    std::vector<BrowserEntry> out;
    for (const std::string& r : recent) {
        const fs::path p = fromUtf8(r);
        if (r.empty() || !isPathAllowed(p, allowNetwork)) continue;
        BrowserEntry e;
        e.kind = EntryKind::Recent;
        e.name = r;
        e.path = p;
        out.push_back(std::move(e));
        if (out.size() >= kMaxRecentFolders) break;
    }
    std::vector<BrowserEntry> drives = driveEntries(driveMask, allowNetwork);
    out.insert(out.end(), std::make_move_iterator(drives.begin()), std::make_move_iterator(drives.end()));
    return out;
}

Listing listDirectory(const fs::path& dir, const ListOptions& opt, const std::atomic<bool>* cancel)
{
    using Clock = std::chrono::steady_clock;
    Listing out;
    out.dir = dir;
    if (!isPathAllowed(dir, opt.allowNetwork)) {
        out.blocked = true;
        out.error = "path not allowed";
        return out;
    }
    // Dysk zmapowany na siec albo dowiazanie do UNC: bez zgody nie wchodzimy (nie dotykamy serwera).
    if (!opt.allowNetwork && isNetworkRedirected(dir)) {
        out.blocked = true;
        out.error = "path not allowed";
        return out;
    }
    BrowserEntry parent;
    parent.kind = EntryKind::Parent;
    parent.name = "..";
    parent.path = parentOf(dir);
    out.entries.push_back(std::move(parent));

    // Zapowiedz nazwy eksportu: wolna nazwa wybierana tu (watek roboczy), nie w UI.
    const auto previewExport = [&] {
        if (!opt.exportStem.empty()) out.exportTarget = uniqueFilePath(dir, opt.exportStem, ".m3u8");
    };
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        out.error = ec.message();
        previewExport();
        return out;
    }
    const auto t0 = Clock::now();
    std::vector<BrowserEntry> dirs, files;
    std::size_t visited = 0;
    const fs::directory_iterator end;
    while (it != end) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            out.cancelled = true;
            break;
        }
        if (++visited > opt.maxVisited) {
            out.truncated = true;
            break;
        }
        if ((visited & 63u) == 0 && Clock::now() - t0 > opt.timeLimit) {
            out.timedOut = true;
            break;
        }
        const fs::directory_entry& de = *it;
        std::error_code e2;
        const fs::path name = de.path().filename();
        const fs::file_type lt = de.symlink_status(e2).type();
        const bool isLink = !e2 && (lt == fs::file_type::symlink || lt == fs::file_type::junction);
        e2.clear();
        bool isDir = false, dirOk = true, netLink = false;
        if (isLink && !opt.allowNetwork) {
            // Bez zgody na siec NIE podazamy za dowiazaniem (is_directory by otworzylo cel, czyli polaczylo z serwerem):
            // typ z atrybutow samego dowiazania, a cel UNC/mapowany oznaczamy jako zablokowany.
            netLink = linkPointsToNetwork(de.path());
            const std::uint32_t a = fileAttributes(de.path());
            isDir = (a & 0x10u) != 0;   // FILE_ATTRIBUTE_DIRECTORY
            dirOk = a != 0;
        } else {
            isDir = de.is_directory(e2);
            dirOk = !e2;
            e2.clear();
        }
        if (dirOk && isDir) {
            BrowserEntry e;
            e.name = toUtf8(name);
            if (!skippedSystemName(e.name)) {
                e.link = isLink;
                e.blocked = netLink;
                e.kind = EntryKind::Dir;
                e.path = de.path();
                if (dirs.size() + files.size() >= opt.maxEntries) {
                    out.truncated = true;
                    break;
                }
                dirs.push_back(std::move(e));
            }
        } else if (netLink) {
            // plik-dowiazanie do zasobu sieciowego bez zgody: pomijamy (nie otwieramy celu)
        } else if (listsFiles(opt.mode) && matchesMode(name, opt.mode)) {
            const bool regular = de.is_regular_file(e2);
            if (!e2 && regular) {
                BrowserEntry e;
                e.kind = EntryKind::File;
                e.name = toUtf8(name);
                e.path = de.path();
                e.cloud = isCloud(fileAttributes(de.path()));
                if (!e.cloud) {
                    e2.clear();
                    const std::uintmax_t sz = de.file_size(e2);
                    e.size = e2 ? 0 : static_cast<std::uint64_t>(sz);
                }
                if (dirs.size() + files.size() >= opt.maxEntries) {
                    out.truncated = true;
                    break;
                }
                files.push_back(std::move(e));
            }
        }
        it.increment(ec);
        if (ec) {
            out.error = ec.message();
            break;
        }
    }
    auto byName = [](const BrowserEntry& a, const BrowserEntry& b) { return naturalCompare(a.name, b.name) < 0; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    out.entries.insert(out.entries.end(), std::make_move_iterator(dirs.begin()), std::make_move_iterator(dirs.end()));
    out.entries.insert(out.entries.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
    previewExport();
    return out;
}

// ---------------------------------------------------------------- DirectoryLister

namespace {

std::atomic<int> g_activeListers{0};
std::atomic<int> g_maxListers{DirectoryLister::kMaxListerThreads};

bool tryAcquireListerSlot()
{
    int cur = g_activeListers.load();
    while (cur < g_maxListers.load()) {
        if (g_activeListers.compare_exchange_weak(cur, cur + 1)) return true;
    }
    return false;
}

void releaseListerSlot() { g_activeListers.fetch_sub(1); }

} // namespace

DirectoryLister::Shared::~Shared()
{
#ifdef _WIN32
    if (thread) CloseHandle(static_cast<HANDLE>(thread));
#endif
}

void DirectoryLister::setMaxThreadsForTest(int n) { g_maxListers.store(n < 0 ? kMaxListerThreads : n); }
int DirectoryLister::activeThreads() { return g_activeListers.load(); }

void DirectoryLister::launch(const std::shared_ptr<Shared>& s, const fs::path& dir, const ListOptions& options)
{
    // Wywolanie ma juz zajete miejsce (tryAcquireListerSlot); watek je zwalnia po zakonczeniu listowania.
    try {
        std::thread([s, dir, options] {
#ifdef _WIN32
            {   // uchwyt tego watku dla CancelSynchronousIo z cancel()
                HANDLE h = nullptr;
                if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS)) {
                    std::lock_guard lock(s->mutex);
                    s->thread = h;
                }
            }
#endif
            Listing l;
            try {
                l = listDirectory(dir, options, &s->cancel);
            } catch (...) {
                l = Listing{};
                l.dir = dir;
                l.error = "listing failed";
            }
            releaseListerSlot();
            std::lock_guard lock(s->mutex);
            s->result = std::move(l);
            s->done = true;
        }).detach();
    } catch (const std::system_error&) {
        releaseListerSlot();
        Listing l;
        l.dir = dir;
        l.error = "cannot start listing thread";
        std::lock_guard lock(s->mutex);
        s->result = std::move(l);
        s->done = true;
    }
}

void DirectoryLister::request(const fs::path& dir, const ListOptions& options)
{
    cancel();
    auto s = std::make_shared<Shared>();
    cur_ = s;
    if (tryAcquireListerSlot()) launch(s, dir, options);
    else pending_ = Pending{dir, options};   // "zajety": ruszy w poll(), gdy zwolni sie miejsce
}

void DirectoryLister::cancel()
{
    if (cur_) {
        cur_->cancel.store(true);
#ifdef _WIN32
        // Zawieszone I/O (martwy dysk, wolny udzial) konczy sie bledem zamiast blokowac watek i jego miejsce w limicie.
        std::lock_guard lock(cur_->mutex);
        if (cur_->thread && !cur_->done) CancelSynchronousIo(static_cast<HANDLE>(cur_->thread));
#endif
    }
    cur_.reset();
    pending_.reset();
}

bool DirectoryLister::busy() const
{
    if (!cur_) return false;
    std::lock_guard lock(cur_->mutex);
    return !cur_->done;
}

std::optional<Listing> DirectoryLister::poll()
{
    if (!cur_) return std::nullopt;
    if (pending_) {
        if (!tryAcquireListerSlot()) return std::nullopt;
        const Pending p = std::move(*pending_);
        pending_.reset();
        launch(cur_, p.dir, p.options);
    }
    std::lock_guard lock(cur_->mutex);
    if (!cur_->done || !cur_->result) return std::nullopt;
    std::optional<Listing> r = std::move(cur_->result);
    cur_->result.reset();
    return r;
}

// ---------------------------------------------------------------- FileBrowser

void FileBrowser::open(BrowserMode mode, const std::vector<std::string>& recent, bool allowNetwork,
                       const std::string& exportName)
{
    mode_ = mode;
    allowNetwork_ = allowNetwork;
    exportName_ = exportName;
    recent_.clear();
    for (const std::string& r : recent)
        if (!r.empty() && isPathAllowed(fromUtf8(r), allowNetwork)) recent_.push_back(r);
    open_ = true;
    initial_ = true;
    reselect_.clear();
    netBlocked_ = false;
    if (recent_.empty()) {
        initial_ = false;
        showRoot();
    } else {
        navigate(fromUtf8(recent_.front()));
    }
}

void FileBrowser::close()
{
    open_ = false;
    lister_.cancel();
    loading_ = false;
    entries_.clear();
    marks_.clear();
}

void FileBrowser::showRoot()
{
    lister_.cancel();
    loading_ = false;
    dir_.clear();
    status_ = {};
    netBlocked_ = false;
    exportTarget_.clear();
    entries_ = rootEntries(recent_, allowNetwork_, driveMaskSet_ ? driveMask_ : logicalDriveMask());
    marks_.assign(entries_.size(), false);
    sel_ = 0;
    top_ = 0;
    if (!reselect_.empty()) {
        for (std::size_t i = 0; i < entries_.size(); ++i)
            if (samePath(entries_[i].path, reselect_)) { sel_ = static_cast<int>(i); break; }
        reselect_.clear();
    }
    ensureVisible();
}

void FileBrowser::navigate(const fs::path& dir)
{
    if (dir.empty()) {
        initial_ = false;
        showRoot();
        return;
    }
    if (!isPathAllowed(dir, allowNetwork_)) {
        netBlocked_ = true;
        return;
    }
    netBlocked_ = false;
    dir_ = dir;
    entries_.clear();
    marks_.clear();
    status_ = {};
    exportTarget_.clear();
    sel_ = 0;
    top_ = 0;
    loading_ = true;
    ListOptions o;
    o.mode = mode_;
    o.allowNetwork = allowNetwork_;
    o.maxEntries = maxEntries_;
    o.timeLimit = timeLimit_;
    if (mode_ == BrowserMode::ExportM3u) o.exportStem = sanitizeFileName(exportName_);   // zapowiedz liczy worker
    lister_.request(dir, o);
}

void FileBrowser::reload()
{
    if (dir_.empty()) showRoot();
    else navigate(dir_);
}

void FileBrowser::tick()
{
    if (!open_ || !loading_) return;
    std::optional<Listing> r = lister_.poll();
    if (!r) return;
    loading_ = false;
    if (initial_) {
        initial_ = false;
        if (!r->error.empty() || r->blocked) {   // zapamietany folder zniknal: zaczynamy od listy dyskow
            showRoot();
            return;
        }
    }
    netBlocked_ = r->blocked;   // dysk zmapowany / dowiazanie do UNC bez zgody: ten sam komunikat co dla UNC
    entries_ = std::move(r->entries);
    status_ = std::move(*r);
    status_.entries.clear();
    marks_.assign(entries_.size(), false);
    sel_ = entries_.size() > 1 ? 1 : 0;
    top_ = 0;
    if (!reselect_.empty()) {
        for (std::size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].kind != EntryKind::Parent && samePath(entries_[i].path, reselect_)) { sel_ = static_cast<int>(i); break; }
        reselect_.clear();
    }
    if (mode_ == BrowserMode::ExportM3u) exportTarget_ = status_.exportTarget;   // z watku roboczego; bez I/O w UI
    ensureVisible();
}

bool FileBrowser::isMarked(int index) const
{
    return index >= 0 && static_cast<std::size_t>(index) < marks_.size() && marks_[static_cast<std::size_t>(index)];
}

std::size_t FileBrowser::markedCount() const
{
    return static_cast<std::size_t>(std::count(marks_.begin(), marks_.end(), true));
}

void FileBrowser::setVisibleRows(int n)
{
    visibleRows_ = std::max(1, n);
    ensureVisible();
}

void FileBrowser::clampSelection()
{
    const int n = static_cast<int>(entries_.size());
    sel_ = n == 0 ? 0 : std::clamp(sel_, 0, n - 1);
}

void FileBrowser::ensureVisible()
{
    clampSelection();
    const int n = static_cast<int>(entries_.size());
    if (sel_ < top_) top_ = sel_;
    if (sel_ >= top_ + visibleRows_) top_ = sel_ - visibleRows_ + 1;
    top_ = std::clamp(top_, 0, std::max(0, n - visibleRows_));
}

void FileBrowser::select(int index)
{
    sel_ = index;
    ensureVisible();
}

void FileBrowser::move(int delta)
{
    sel_ += delta;
    ensureVisible();
}

void FileBrowser::page(int direction)
{
    move((direction < 0 ? -1 : 1) * std::max(1, visibleRows_ - 1));
}

void FileBrowser::scrollBy(int rows)
{
    const int n = static_cast<int>(entries_.size());
    top_ = std::clamp(top_ + rows, 0, std::max(0, n - visibleRows_));
}

BrowserOutcome FileBrowser::enterSelected()
{
    if (entries_.empty()) return {};
    clampSelection();
    const BrowserEntry& e = entries_[static_cast<std::size_t>(sel_)];
    if (e.kind == EntryKind::Recent || e.kind == EntryKind::Drive || e.kind == EntryKind::Dir) {
        if (e.blocked) { netBlocked_ = true; return {}; }   // dysk/dowiazanie sieciowe bez zgody
        reselect_.clear();
        navigate(e.path);
    }
    return {};
}

BrowserOutcome FileBrowser::activate()
{
    if (entries_.empty()) return {};
    clampSelection();
    const BrowserEntry e = entries_[static_cast<std::size_t>(sel_)];   // kopia: navigate() czysci liste
    switch (e.kind) {
    case EntryKind::Recent:
    case EntryKind::Drive:
    case EntryKind::Dir:
        if (e.blocked) { netBlocked_ = true; return {}; }   // dysk/dowiazanie sieciowe bez zgody
        reselect_.clear();
        navigate(e.path);
        return {};
    case EntryKind::Parent:
        reselect_ = dir_;
        navigate(e.path);
        return {};
    case EntryKind::File: {
        BrowserOutcome o;
        o.folder = dir_;
        if (mode_ == BrowserMode::ImportM3u) {
            o.kind = BrowserOutcome::Kind::Import;
            o.paths.push_back(e.path);
            return o;
        }
        if (mode_ != BrowserMode::AddFiles) return {};
        o.kind = BrowserOutcome::Kind::AddPaths;
        for (std::size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].kind == EntryKind::File && (marks_[i] || static_cast<int>(i) == sel_)) o.paths.push_back(entries_[i].path);
        return o;
    }
    }
    return {};
}

BrowserOutcome FileBrowser::back()
{
    if (dir_.empty()) {
        BrowserOutcome o;
        o.kind = BrowserOutcome::Kind::Close;
        return o;
    }
    reselect_ = dir_;
    navigate(parentOf(dir_));
    return {};
}

BrowserOutcome FileBrowser::useHere()
{
    BrowserOutcome o;
    o.folder = dir_;
    switch (mode_) {
    case BrowserMode::AddFiles:
        if (dir_.empty() || loading_) return {};
        o.kind = BrowserOutcome::Kind::AddPaths;
        o.paths.push_back(dir_);
        return o;
    case BrowserMode::AddFolder:
        if (markedCount() > 0) {
            o.kind = BrowserOutcome::Kind::AddPaths;
            for (std::size_t i = 0; i < entries_.size(); ++i)
                if (marks_[i] && entries_[i].kind == EntryKind::Dir) o.paths.push_back(entries_[i].path);
            return o;
        }
        if (dir_.empty() || loading_) return {};
        o.kind = BrowserOutcome::Kind::AddPaths;
        o.paths.push_back(dir_);
        return o;
    case BrowserMode::ExportM3u:
        if (dir_.empty() || loading_ || exportTarget_.empty()) return {};
        o.kind = BrowserOutcome::Kind::Export;
        o.paths.push_back(exportTarget_);
        return o;
    case BrowserMode::ImportM3u: return {};
    }
    return {};
}

void FileBrowser::toggleMark()
{
    if (!canMark(mode_) || entries_.empty()) return;
    clampSelection();
    const BrowserEntry& e = entries_[static_cast<std::size_t>(sel_)];
    const bool ok = mode_ == BrowserMode::AddFiles ? e.kind == EntryKind::File : e.kind == EntryKind::Dir;
    if (!ok) return;
    marks_[static_cast<std::size_t>(sel_)] = !marks_[static_cast<std::size_t>(sel_)];
    move(1);
}

} // namespace vgmplay::ui
