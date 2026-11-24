// vgmplay - WEWNETRZNY naglowek pomocniczy modulow settings/playlist/library (nie jest kontraktem).
// Konwersje UTF-8 <-> UTF-16, ograniczony odczyt pliku oraz zapis atomowy (plik tymczasowy w tym
// samym katalogu + ReplaceFileW / MoveFileExW). Blokujace - tylko z watku workera.
// Trzeci modul (playlist, library) dolacza ten plik przez PRIVATE include_directories
// (../settings); to jedyne miejsce ze wspolnym kodem plikowym, zeby nie dublowac WinAPI.
#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <cstdint>
#include <filesystem>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "vgmplay/core.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace vgmplay::fsutil {

// ---- kodowanie ----

// strict: niepoprawny UTF-8 -> nullopt; !strict: znaki zastepcze U+FFFD.
[[nodiscard]] inline std::optional<std::wstring> utf8ToWide(std::string_view s, bool strict = true)
{
    if (s.empty()) return std::wstring();
    if (s.size() > 0x7FFFFFFFu) return std::nullopt;
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int n = ::MultiByteToWideChar(CP_UTF8, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::nullopt;
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, flags, s.data(), static_cast<int>(s.size()), w.data(), n) != n)
        return std::nullopt;
    return w;
}

[[nodiscard]] inline std::optional<std::wstring> acpToWide(std::string_view s)
{
    if (s.empty()) return std::wstring();
    if (s.size() > 0x7FFFFFFFu) return std::nullopt;
    const int n = ::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::nullopt;
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    if (::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), n) != n)
        return std::nullopt;
    return w;
}

// Niesparowane surogaty -> U+FFFD (bez WC_ERR_INVALID_CHARS), zawsze zwraca napis.
[[nodiscard]] inline std::string wideToUtf8(std::wstring_view w)
{
    if (w.empty() || w.size() > 0x7FFFFFFFu) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Strona kodowa systemu; znaki bez odpowiednika -> '?'.
[[nodiscard]] inline std::string wideToAcp(std::wstring_view w)
{
    if (w.empty() || w.size() > 0x7FFFFFFFu) return {};
    const int n = ::WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Sciezka -> UTF-8 z '/' jako separatorem (format JSON/M3U).
[[nodiscard]] inline std::string pathToUtf8(const std::filesystem::path& p)
{
    return wideToUtf8(p.generic_wstring());
}

[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view s)
{
    auto w = utf8ToWide(s, false);
    return w ? std::filesystem::path(*w) : std::filesystem::path();
}

// Porownanie bez rozrozniania wielkosci liter (Unicode, wg tabel systemu).
[[nodiscard]] inline bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

// ---- rozwiazywanie dowiazan ----

// Rozwiazuje istniejaca sciezke przez uchwyt (GetFinalPathNameByHandleW). false + err przy bledzie.
[[nodiscard]] inline bool finalPathOf(const std::filesystem::path& cur, std::filesystem::path& out, DWORD& err)
{
    HANDLE h = ::CreateFileW(cur.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = ::GetLastError(); return false; }
    std::wstring buf(1024, L'\0');
    DWORD n = ::GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), VOLUME_NAME_DOS);
    if (n >= buf.size()) {
        buf.resize(static_cast<std::size_t>(n) + 1);
        n = ::GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), VOLUME_NAME_DOS);
    }
    err = n == 0 ? ::GetLastError() : 0;
    ::CloseHandle(h);
    if (n == 0 || n >= buf.size()) return false;
    buf.resize(n);
    // "\\?\UNC\srv\sh" -> "\\srv\sh";  "\\?\C:\x" -> "C:\x"
    if (buf.rfind(LR"(\\?\UNC\)", 0) == 0) buf = L"\\\\" + buf.substr(8);
    else if (buf.rfind(LR"(\\?\)", 0) == 0) buf = buf.substr(4);
    out = std::filesystem::path(buf);
    return true;
}

// true, gdy ktorys istniejacy skladnik sciezki jest dowiazaniem symbolicznym / junction.
[[nodiscard]] inline bool hasLinkComponent(std::filesystem::path cur)
{
    while (!cur.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (::GetFileAttributesExW(cur.c_str(), GetFileExInfoStandard, &d) && (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return true;
        const std::filesystem::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return false;
}

// "Slabo kanoniczna" sciezka: najglebszy ISTNIEJACY przodek jest rozwiazywany przez system
// (junctions/symlinki -> rzeczywisty cel), reszta (nieistniejaca) jest doklejana leksykalnie.
// `p` musi byc bezwzgledna i znormalizowana leksykalnie.
// Zamiennik std::filesystem::weakly_canonical, ktore w MSVC STL zawodzi ("Incorrect function")
// m.in. na dyskach `subst`. Obejscie: dysk subst jest tlumaczony na sciezke docelowa
// (QueryDosDeviceW) i rozwiazywany ponownie. Gdy to sie nie uda, a w sciezce sa dowiazania,
// zwracamy false (nie mozna zweryfikowac => odrzucamy); bez dowiazan wynik leksykalny jest poprawny.
[[nodiscard]] inline bool weaklyResolve(const std::filesystem::path& p, std::filesystem::path& out)
{
    std::filesystem::path cur = p;
    std::vector<std::filesystem::path> tail;
    while (::GetFileAttributesW(cur.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::filesystem::path parent = cur.parent_path();
        if (parent == cur || parent.empty()) { out = p; return true; }   // korzen nie istnieje (np. brak dysku)
        tail.push_back(cur.filename());
        cur = parent;
    }
    std::filesystem::path resolved;
    DWORD err = 0;
    bool ok = finalPathOf(cur, resolved, err);
    if (!ok && (err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED)) {
        const std::wstring& n = cur.native();
        if (n.size() >= 3 && n[1] == L':' && n[2] == L'\\') {
            wchar_t target[1024] = {};
            const wchar_t drive[3] = {n[0], L':', L'\0'};
            if (::QueryDosDeviceW(drive, target, 1024) != 0) {
                std::wstring t = target;   // dla subst: "\??\D:\katalog"
                if (t.rfind(LR"(\??\)", 0) == 0 && t.size() >= 6 && t[5] == L':') {
                    t = t.substr(4);
                    while (t.size() > 3 && t.back() == L'\\') t.pop_back();
                    ok = finalPathOf(std::filesystem::path(t + n.substr(2)), resolved, err);
                }
            }
        }
    }
    if (!ok) {
        if (err != ERROR_INVALID_FUNCTION && err != ERROR_NOT_SUPPORTED) return false;
        if (hasLinkComponent(cur)) return false;
        out = p;
        return true;
    }
    for (auto it = tail.rbegin(); it != tail.rend(); ++it) resolved /= *it;
    out = resolved.lexically_normal();
    return true;
}

// ---- bledy WinAPI ----

[[nodiscard]] inline ErrorCode errorCodeFromWin(DWORD e) noexcept
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME: return ErrorCode::NotFound;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
    case ERROR_SHARING_VIOLATION: return ErrorCode::AccessDenied;
    default: return ErrorCode::Io;
    }
}

[[nodiscard]] inline Error winError(const char* what, DWORD e)
{
    return makeError(errorCodeFromWin(e), std::string(what) + " (win32 error " + std::to_string(e) + ")");
}

// ---- odczyt ----

// Czyta caly zwykly plik; rozmiar sprawdzany na otwartym uchwycie przed alokacja.
[[nodiscard]] inline Result<std::string> readFileLimited(const std::filesystem::path& file, std::uint64_t maxBytes)
{
    HANDLE h = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return winError("cannot open file", ::GetLastError());
    struct Closer { HANDLE h; ~Closer() { ::CloseHandle(h); } } closer{h};
    if (::GetFileType(h) != FILE_TYPE_DISK) return makeError(ErrorCode::Io, "not a regular file");
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz)) return winError("cannot get file size", ::GetLastError());
    if (sz.QuadPart < 0 || static_cast<std::uint64_t>(sz.QuadPart) > maxBytes)
        return makeError(ErrorCode::TooLarge, "file exceeds size limit");
    std::string data;
    try {
        data.resize(static_cast<std::size_t>(sz.QuadPart));
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory reading file");
    }
    std::size_t got = 0;
    while (got < data.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min<std::size_t>)(data.size() - got, 1u << 24));
        DWORD n = 0;
        if (!::ReadFile(h, data.data() + got, chunk, &n, nullptr)) return winError("read failed", ::GetLastError());
        if (n == 0) break;   // plik skrocony w trakcie czytania
        got += n;
    }
    data.resize(got);
    return data;
}

// ---- losowosc ----

// 128 bitow z systemowego CSPRNG (BCryptGenRandom), jako 32 znaki hex (male litery).
[[nodiscard]] inline std::wstring randomHex32()
{
    unsigned char b[16] = {};
    if (::BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // Awaria CSPRNG jest praktycznie niemozliwa; awaryjnie mieszamy czas i PID (CREATE_NEW i tak chroni).
        LARGE_INTEGER t{};
        ::QueryPerformanceCounter(&t);
        std::uint64_t x = static_cast<std::uint64_t>(t.QuadPart) ^ (static_cast<std::uint64_t>(::GetCurrentProcessId()) << 32);
        for (int i = 0; i < 16; ++i) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            b[i] = static_cast<unsigned char>(x >> 56);
        }
    }
    static const wchar_t kHex[] = L"0123456789abcdef";
    std::wstring s;
    for (unsigned char c : b) {
        s.push_back(kHex[c >> 4]);
        s.push_back(kHex[c & 15]);
    }
    return s;
}

// ---- zapis atomowy ----

// Czy `suffix` (to, co zostaje po "<nazwa>.tmp") ma NASZ wzorzec: 32 znaki hex albo stary "<pid>_<n>".
[[nodiscard]] inline bool isOwnTempSuffix(std::wstring_view suffix)
{
    if (suffix.size() == 32)
        return std::all_of(suffix.begin(), suffix.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); });
    const std::size_t us = suffix.find(L'_');
    if (us == std::wstring_view::npos || us == 0 || us + 1 >= suffix.size()) return false;
    return std::all_of(suffix.begin(), suffix.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'_'; }) &&
           suffix.find(L'_', us + 1) == std::wstring_view::npos;
}

// Usuwa osierocone pliki "<nazwa>.tmp<wzorzec>" starsze niz `minAgeSeconds` (pozostale po awarii w trakcie
// zapisu). Dotyka WYLACZNIE nazw o naszym wzorcu, tylko zwyklych plikow. Zwraca liczbe usunietych.
inline int cleanupOrphanTemps(const std::filesystem::path& file, std::uint64_t minAgeSeconds = 3600) noexcept
{
    int removed = 0;
    try {
        const std::filesystem::path dir = file.parent_path();
        const std::wstring prefix = file.filename().wstring() + L".tmp";
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileExW((dir / (prefix + L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return 0;
        FILETIME nowFt{};
        ::GetSystemTimeAsFileTime(&nowFt);
        const auto toU64 = [](const FILETIME& f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring name = fd.cFileName;
            if (name.size() <= prefix.size() || !isOwnTempSuffix(std::wstring_view(name).substr(prefix.size()))) continue;
            const std::uint64_t now = toU64(nowFt), w = toU64(fd.ftLastWriteTime);
            if (now < w || (now - w) / 10000000ull < minAgeSeconds) continue;
            if (::DeleteFileW((dir / name).c_str())) ++removed;   // dowiazanie zostaje usuniete jako dowiazanie
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    } catch (...) {
    }
    return removed;
}

// Zapisuje `data` do `file` tak, by odczytujacy widzial stara albo nowa tresc (nigdy polowe):
// plik tymczasowy w TYM SAMYM katalogu (nazwa z 128 bitami losowosci, CREATE_NEW, bez podazania za
// dowiazaniem, po otwarciu weryfikowany jako zwykly plik) -> FlushFileBuffers -> ReplaceFileW (gdy cel
// istnieje) albo MoveFileExW. Tworzy brakujace katalogi. Przy bledzie usuwa plik tymczasowy.
// Na poczatku sprząta osierocone, stare pliki tymczasowe tego samego pliku.
[[nodiscard]] inline Result<void> atomicWriteFile(const std::filesystem::path& file, std::string_view data)
{
    std::error_code ec;
    const std::filesystem::path dir = file.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec) return makeError(ErrorCode::Io, "cannot create directory: " + ec.message());
    }
    cleanupOrphanTemps(file);

    HANDLE h = INVALID_HANDLE_VALUE;
    std::filesystem::path tmp;
    for (int attempt = 0; attempt < 4 && h == INVALID_HANDLE_VALUE; ++attempt) {
        tmp = file;
        tmp += L".tmp" + randomHex32();
        h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE && ::GetLastError() != ERROR_FILE_EXISTS && ::GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (h == INVALID_HANDLE_VALUE) return winError("cannot create temporary file", ::GetLastError());
    DWORD werr = 0;
    {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(h, &info)) werr = ::GetLastError();
        else if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || ::GetFileType(h) != FILE_TYPE_DISK)
            werr = ERROR_INVALID_NAME;
    }
    std::size_t put = 0;
    while (werr == 0 && put < data.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min<std::size_t>)(data.size() - put, 1u << 24));
        DWORD n = 0;
        if (!::WriteFile(h, data.data() + put, chunk, &n, nullptr)) { werr = ::GetLastError(); break; }
        put += n;
    }
    if (werr == 0 && !::FlushFileBuffers(h)) werr = ::GetLastError();
    ::CloseHandle(h);
    if (werr != 0) {
        ::DeleteFileW(tmp.c_str());
        return winError("write failed", werr);
    }

    DWORD err = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {   // antywirus / indeksator moga chwilowo trzymac plik
        const bool exists = ::GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
        BOOL ok = FALSE;
        if (exists) {
            ok = ::ReplaceFileW(file.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr);
        } else {
            ok = ::MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        if (ok) return Result<void>{};
        err = ::GetLastError();
        if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) break;
        ::Sleep(20u << attempt);
    }
    ::DeleteFileW(tmp.c_str());
    return winError("cannot replace target file", err);
}

// Kopiuje uszkodzony plik obok oryginalu (file + suffix, np. ".bak"). Poprzednia kopia (takze
// dowiazanie podstawione w jej miejsce) jest NAJPIERW usuwana, potem CopyFileW(bFailIfExists=TRUE):
// kopia nigdy nie zapisuje przez dowiazanie. Oryginal zostaje nietkniety. true = kopia powstala.
inline bool preserveCorruptCopy(const std::filesystem::path& file, const wchar_t* suffix) noexcept
{
    std::filesystem::path dst = file;
    dst += suffix;
    const DWORD a = ::GetFileAttributesW(dst.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) {
        if (a & FILE_ATTRIBUTE_DIRECTORY) return false;   // nie ruszamy katalogow
        if (!::DeleteFileW(dst.c_str())) return false;
    }
    return ::CopyFileW(file.c_str(), dst.c_str(), TRUE) != 0;
}

// ---- walidacja nazw i sciezek (wspolna dla M3U, JSON, skanu) ----

[[nodiscard]] inline bool isReservedDeviceName(std::wstring comp)
{
    const std::size_t dot = comp.find(L'.');
    if (dot != std::wstring::npos) comp.resize(dot);
    while (!comp.empty() && comp.back() == L' ') comp.pop_back();
    std::wstring up;
    for (wchar_t ch : comp) up.push_back(static_cast<wchar_t>(std::towupper(ch)));
    if (up == L"CON" || up == L"PRN" || up == L"AUX" || up == L"NUL" || up == L"CONIN$" || up == L"CONOUT$") return true;
    return up.size() == 4 && (up.compare(0, 3, L"COM") == 0 || up.compare(0, 3, L"LPT") == 0) && up[3] >= L'1' && up[3] <= L'9';
}

// Skladnik nazwy, ktorego Win32 nie traktuje dosłownie: nazwa urzadzenia albo koncowa kropka/spacja.
// "." i ".." nie sa tu oceniane (obsluguje je normalizacja).
[[nodiscard]] inline bool isUnsafeComponentName(const std::wstring& comp)
{
    if (comp.empty() || comp == L"." || comp == L"..") return false;
    return comp.back() == L'.' || comp.back() == L' ' || isReservedDeviceName(comp);
}

// Sciezka sieciowa/urzadzenia: zaczyna sie od dwoch separatorow (UNC, \\?\, \\.\).
[[nodiscard]] inline bool startsWithDoubleSeparator(std::wstring_view s)
{
    return s.size() >= 2 && (s[0] == L'\\' || s[0] == L'/') && (s[1] == L'\\' || s[1] == L'/');
}

// Sciezka zapisana w pliku JSON (UTF-8). Odrzuca: pusta/NUL, URL (schemat://), przestrzen urzadzen
// (\\?\, \\.\), potoki (\\host\pipe\...) oraz - gdy !allowNetwork - kazda sciezke UNC.
[[nodiscard]] inline bool isUnsafeStoredPath(const std::string& s, bool allowNetwork)
{
    if (s.empty() || s.find('\0') != std::string::npos) return true;
    std::string n = s;
    for (char& c : n)
        if (c == '/') c = '\\';
    if (n.size() >= 2 && n[0] == '\\' && n[1] == '\\') {
        if (n.size() >= 3 && (n[2] == '?' || n[2] == '.')) return true;   // przestrzen urzadzen
        if (!allowNetwork) return true;
        std::string low = n;
        for (char& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        const std::size_t hostEnd = low.find('\\', 2);
        if (hostEnd != std::string::npos && (low.compare(hostEnd + 1, 5, "pipe\\") == 0 || low.substr(hostEnd + 1) == "pipe"))
            return true;
        return false;
    }
    if (std::isalpha(static_cast<unsigned char>(n[0]))) {
        std::size_t i = 1;
        while (i < n.size() && (std::isalnum(static_cast<unsigned char>(n[i])) || n[i] == '+' || n[i] == '-' || n[i] == '.')) ++i;
        if (i + 2 < n.size() && n[i] == ':' && n[i + 1] == '\\' && n[i + 2] == '\\') return true;   // "://" po normalizacji
    }
    return false;
}

// ---- teksty ----

// Tytul z zewnetrznego pliku: usuwa znaki sterujace (TAB -> spacja), C1, znaki kierunku/niewidoczne
// (U+200B-200F, U+202A-202E, U+2066-2069), niepoprawny UTF-8 -> U+FFFD; limit `maxBytes` na granicy znaku.
[[nodiscard]] inline std::string sanitizeTitle(std::string_view in, std::size_t maxBytes = 512)
{
    std::string out;
    std::size_t i = 0;
    const auto cont = [&](std::size_t k) { return k < in.size() && (static_cast<unsigned char>(in[k]) & 0xC0) == 0x80; };
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        char32_t cp = 0xFFFD;
        std::size_t len = 1;
        if (c < 0x80) cp = c;
        else if (c >= 0xC2 && c <= 0xDF && cont(i + 1)) {
            cp = ((c & 0x1Fu) << 6) | (in[i + 1] & 0x3Fu);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
            const char32_t v = ((c & 0x0Fu) << 12) | ((in[i + 1] & 0x3Fu) << 6) | (in[i + 2] & 0x3Fu);
            if (v >= 0x800 && !(v >= 0xD800 && v <= 0xDFFF)) { cp = v; len = 3; }
        } else if ((c & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            const char32_t v = ((c & 0x07u) << 18) | ((in[i + 1] & 0x3Fu) << 12) | ((in[i + 2] & 0x3Fu) << 6) | (in[i + 3] & 0x3Fu);
            if (v >= 0x10000 && v <= 0x10FFFF) { cp = v; len = 4; }
        }
        i += len;
        if (cp == 0x09) cp = 0x20;
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || (cp >= 0x200B && cp <= 0x200F) ||
            (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069))
            continue;
        char buf[4];
        std::size_t n = 0;
        if (cp < 0x80) buf[n++] = static_cast<char>(cp);
        else if (cp < 0x800) { buf[n++] = static_cast<char>(0xC0 | (cp >> 6)); buf[n++] = static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            buf[n++] = static_cast<char>(0xE0 | (cp >> 12));
            buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            buf[n++] = static_cast<char>(0xF0 | (cp >> 18));
            buf[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        }
        if (out.size() + n > maxBytes) break;
        out.append(buf, n);
    }
    return out;
}

inline constexpr std::uint64_t kMaxDurationMs = 24ull * 3600 * 1000;
[[nodiscard]] inline std::uint64_t clampDurationMs(std::uint64_t v) noexcept { return v > kMaxDurationMs ? kMaxDurationMs : v; }

} // namespace vgmplay::fsutil
