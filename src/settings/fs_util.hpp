// vgmplay - WEWNETRZNY naglowek pomocniczy modulow settings/playlist/library (nie jest kontraktem).
// Konwersje UTF-8 <-> UTF-16, ograniczony odczyt pliku oraz zapis atomowy (plik tymczasowy w tym
// samym katalogu + ReplaceFileW / MoveFileExW). Blokujace - tylko z watku workera.
// Trzeci modul (playlist, library) dolacza ten plik przez PRIVATE include_directories
// (../settings); to jedyne miejsce ze wspolnym kodem plikowym, zeby nie dublowac WinAPI.
#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "vgmplay/core.hpp"

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

// ---- zapis atomowy ----

// Zapisuje `data` do `file` tak, by odczytujacy widzial stara albo nowa tresc (nigdy polowe):
// plik tymczasowy w TYM SAMYM katalogu -> FlushFileBuffers -> ReplaceFileW (gdy cel istnieje)
// albo MoveFileExW. Tworzy brakujace katalogi. Przy bledzie usuwa plik tymczasowy.
[[nodiscard]] inline Result<void> atomicWriteFile(const std::filesystem::path& file, std::string_view data)
{
    static std::atomic<std::uint32_t> counter{0};
    std::error_code ec;
    const std::filesystem::path dir = file.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec) return makeError(ErrorCode::Io, "cannot create directory: " + ec.message());
    }
    std::filesystem::path tmp = file;
    tmp += L".tmp" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(counter.fetch_add(1));

    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return winError("cannot create temporary file", ::GetLastError());
    DWORD werr = 0;
    std::size_t put = 0;
    while (put < data.size()) {
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

// Kopiuje uszkodzony plik obok oryginalu (file + suffix, np. ".bak"), nadpisujac poprzednia kopie.
// Oryginal zostaje nietkniety. Zwraca true, gdy kopia powstala.
inline bool preserveCorruptCopy(const std::filesystem::path& file, const wchar_t* suffix) noexcept
{
    std::filesystem::path dst = file;
    dst += suffix;
    return ::CopyFileW(file.c_str(), dst.c_str(), FALSE) != 0;
}

} // namespace vgmplay::fsutil
