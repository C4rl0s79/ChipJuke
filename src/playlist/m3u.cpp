// vgmplay - playlist: import/eksport M3U/M3U8 z walidacja sciezek (patrz docs/adr/0003).
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cmath>
#include <new>
#include <string>
#include <vector>

#include "fs_util.hpp"
#include "vgmplay/playlist.hpp"

namespace vgmplay::playlist {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxLineChars = 32768;
constexpr std::uint32_t kHardMaxEntries = 100000;

// Skladniki sciezki (bez pustych i "."), do porownan bez rozrozniania wielkosci liter.
std::vector<std::wstring> components(const fs::path& p)
{
    std::vector<std::wstring> out;
    for (const auto& c : p) {
        std::wstring s = c.wstring();
        if (s.empty() || s == L".") continue;
        out.push_back(std::move(s));
    }
    return out;
}

// true, gdy `p` lezy w `root` (albo jest nim); obie sciezki juz znormalizowane i bezwzgledne.
bool isWithin(const fs::path& root, const fs::path& p)
{
    const auto r = components(root);
    const auto c = components(p);
    if (c.size() < r.size()) return false;
    for (std::size_t i = 0; i < r.size(); ++i)
        if (!fsutil::equalsIgnoreCase(r[i], c[i])) return false;
    return true;
}

bool isReservedDeviceName(std::wstring comp)
{
    const std::size_t dot = comp.find(L'.');
    if (dot != std::wstring::npos) comp.resize(dot);
    while (!comp.empty() && comp.back() == L' ') comp.pop_back();
    std::wstring up;
    for (wchar_t ch : comp) up.push_back(static_cast<wchar_t>(std::towupper(ch)));
    if (up == L"CON" || up == L"PRN" || up == L"AUX" || up == L"NUL" || up == L"CONIN$" || up == L"CONOUT$") return true;
    if (up.size() == 4 && (up.compare(0, 3, L"COM") == 0 || up.compare(0, 3, L"LPT") == 0) && up[3] >= L'1' && up[3] <= L'9')
        return true;
    return false;
}

enum class Kind { Relative, DriveAbsolute, Rooted };

// Skladnia sciezki z M3U. Zwraca "" gdy OK, inaczej powod odrzucenia. `w` po konwersji '/'->'\'.
const char* checkSyntax(std::wstring& w, Kind& kind)
{
    if (w.empty()) return "empty path";
    for (wchar_t ch : w) {
        if (ch < 0x20) return "control character in path";
        if (ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') return "wildcard or invalid character";
    }
    // URL: schemat "://" (takze jednoliterowy).
    if (std::iswalpha(w[0])) {
        std::size_t i = 1;
        while (i < w.size() && (std::iswalnum(w[i]) || w[i] == L'+' || w[i] == L'-' || w[i] == L'.')) ++i;
        if (i + 2 < w.size() && w[i] == L':' && w[i + 1] == L'/' && w[i + 2] == L'/') return "URL";
    }
    std::replace(w.begin(), w.end(), L'/', L'\\');
    if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\') return "UNC or device path";
    // dwukropek tylko jako litera dysku na pozycji 1 (blokuje strumienie ADS i "schemat:x")
    const std::size_t colon = w.find(L':');
    if (colon != std::wstring::npos) {
        if (colon != 1 || !std::iswalpha(w[0]) || w.find(L':', 2) != std::wstring::npos) return "unexpected colon in path";
        if (w.size() == 2 || w[2] != L'\\') return "drive-relative path";
        kind = Kind::DriveAbsolute;
    } else if (w[0] == L'\\') {
        kind = Kind::Rooted;
    } else {
        kind = Kind::Relative;
    }
    // skladniki
    std::size_t start = 0;
    while (start <= w.size()) {
        std::size_t end = w.find(L'\\', start);
        if (end == std::wstring::npos) end = w.size();
        const std::wstring comp = w.substr(start, end - start);
        start = end + 1;
        if (comp.empty() || comp == L"." || comp == L"..") continue;
        if (kind == Kind::DriveAbsolute && comp.size() == 2 && comp[1] == L':') continue;   // "C:"
        if (comp.back() == L'.' || comp.back() == L' ') return "component with trailing dot or space";
        if (isReservedDeviceName(comp)) return "reserved device name";
    }
    return "";
}

fs::path lexicalNormal(const fs::path& p)
{
    fs::path n = p.lexically_normal();
    return n;
}

// Zamiana sekund z #EXTINF (calkowite lub z ulamkiem; ujemne = nieznane) na ms.
std::uint64_t parseDurationMs(const std::wstring& s)
{
    if (s.empty()) return 0;
    try {
        std::size_t used = 0;
        const double v = std::stod(s, &used);
        if (!(v > 0.0) || v > 24.0 * 3600.0 * 365.0) return 0;   // NaN, <=0, absurd
        return static_cast<std::uint64_t>(std::llround(v * 1000.0));
    } catch (...) {
        return 0;
    }
}

std::wstring trim(const std::wstring& s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t')) --b;
    return s.substr(a, b - a);
}

} // namespace

Result<M3uImportResult> importM3u(std::string_view text, const M3uImportOptions& options)
{
    try {
        if (options.baseDir.empty()) return makeError(ErrorCode::InvalidArgument, "baseDir is empty");
        std::error_code ec;
        const fs::path base = lexicalNormal(fs::absolute(options.baseDir, ec));
        if (ec) return makeError(ErrorCode::InvalidArgument, "cannot resolve baseDir");
        const fs::path rootDir = options.allowedRoot.empty() ? base : lexicalNormal(fs::absolute(options.allowedRoot, ec));
        if (ec) return makeError(ErrorCode::InvalidArgument, "cannot resolve allowedRoot");
        fs::path weakRoot;
        if (!fsutil::weaklyResolve(rootDir, weakRoot)) return makeError(ErrorCode::InvalidArgument, "cannot resolve allowedRoot");

        bool utf8 = options.assumeUtf8;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            text.remove_prefix(3);
            utf8 = true;   // BOM wygrywa z assumeUtf8
        } else if (text.size() >= 2 && ((static_cast<unsigned char>(text[0]) == 0xFF && static_cast<unsigned char>(text[1]) == 0xFE) ||
                                        (static_cast<unsigned char>(text[0]) == 0xFE && static_cast<unsigned char>(text[1]) == 0xFF))) {
            return makeError(ErrorCode::ParseError, "UTF-16 M3U is not supported");
        }

        const std::uint32_t maxEntries = (std::min)(options.maxEntries, kHardMaxEntries);
        M3uImportResult res;
        auto warn = [&](WarningCode code, std::string msg) {
            if (res.warnings.size() < 200) res.warnings.push_back(Warning{code, kNoOffset, std::move(msg)});
        };

        bool havePending = false;
        std::uint64_t pendingMs = 0;
        std::string pendingTitle;
        std::size_t lineNo = 0;

        std::size_t pos = 0;
        while (pos <= text.size()) {
            std::size_t end = pos;
            while (end < text.size() && text[end] != '\n' && text[end] != '\r') ++end;
            std::string_view rawLine = text.substr(pos, end - pos);
            // CRLF / LF / samotny CR
            if (end < text.size() && text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n') ++end;
            pos = end + 1;
            ++lineNo;
            if (rawLine.empty()) { if (pos > text.size()) break; continue; }
            if (rawLine.size() > kMaxLineChars * 4) { ++res.rejected; warn(WarningCode::EntrySkipped, "line " + std::to_string(lineNo) + ": line too long"); havePending = false; continue; }

            auto wopt = utf8 ? fsutil::utf8ToWide(rawLine, true) : fsutil::acpToWide(rawLine);
            if (!wopt) {
                ++res.rejected;
                warn(WarningCode::EntrySkipped, "line " + std::to_string(lineNo) + ": invalid text encoding");
                havePending = false;
                continue;
            }
            std::wstring line = trim(*wopt);
            if (line.empty()) continue;

            if (line[0] == L'#') {
                if (line.rfind(L"#EXTINF:", 0) == 0) {
                    const std::wstring rest = line.substr(8);
                    const std::size_t comma = rest.find(L',');
                    const std::wstring dur = trim(comma == std::wstring::npos ? rest : rest.substr(0, comma));
                    pendingMs = parseDurationMs(dur);
                    pendingTitle = comma == std::wstring::npos ? std::string() : fsutil::wideToUtf8(trim(rest.substr(comma + 1)));
                    havePending = true;
                }
                continue;   // #EXTM3U i inne komentarze
            }

            // --- pozycja ---
            const bool hadInfo = havePending;
            havePending = false;
            if (res.entries.size() >= maxEntries) {
                warn(WarningCode::LimitReached, "entry limit reached; remaining lines ignored");
                break;
            }
            auto reject = [&](const char* why) {
                ++res.rejected;
                warn(WarningCode::PathRejected, "line " + std::to_string(lineNo) + ": " + why);
            };
            if (line.size() > kMaxLineChars) { reject("path too long"); continue; }

            Kind kind = Kind::Relative;
            std::wstring w = line;
            if (const char* why = checkSyntax(w, kind); *why) { reject(why); continue; }
            if (kind != Kind::Relative && !options.allowAbsolutePaths) { reject("absolute path not allowed"); continue; }

            fs::path full;
            if (kind == Kind::Relative) full = base / fs::path(w);
            else if (kind == Kind::Rooted) full = base.root_name() / fs::path(w);
            else full = fs::path(w);
            full = lexicalNormal(full);
            if (!full.has_filename()) { reject("path has no file name"); continue; }
            if (!isWithin(rootDir, full)) { reject("path escapes allowed root"); continue; }
            // Dowiazania (junction/symlink) w istniejacej czesci sciezki: sprawdz skutek po rozwiazaniu.
            fs::path weak;
            if (!fsutil::weaklyResolve(full, weak)) { reject("cannot resolve path"); continue; }
            if (!isWithin(weakRoot, weak)) { reject("link escapes allowed root"); continue; }

            Entry e;
            e.path = std::move(full);
            if (hadInfo) {
                e.durationMs = pendingMs;
                e.title = std::move(pendingTitle);
            }
            pendingTitle.clear();
            res.entries.push_back(std::move(e));
        }
        return res;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory importing M3U");
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::Internal, std::string("M3U import: ") + ex.what());
    }
}

std::string exportM3u(const Playlist& list, const M3uExportOptions& options)
{
    std::wstring out;
    auto emit = [&](std::wstring_view s) { out.append(s); };
    if (options.extInf) emit(L"#EXTM3U\r\n");

    std::error_code ec;
    fs::path base;
    if (!options.baseDir.empty()) {
        base = lexicalNormal(fs::absolute(options.baseDir, ec));
        if (ec) base.clear();
    }
    for (const Entry& e : list.entries) {
        if (options.extInf) {
            std::wstring title = fsutil::utf8ToWide(e.title, false).value_or(std::wstring());
            for (wchar_t& ch : title) if (ch == L'\r' || ch == L'\n') ch = L' ';
            const long long sec = e.durationMs == 0 ? -1 : static_cast<long long>((e.durationMs + 500) / 1000);
            emit(L"#EXTINF:" + std::to_wstring(sec) + L"," + title + L"\r\n");
        }
        fs::path p = e.path;
        if (!base.empty() && p.is_absolute()) {
            const fs::path np = lexicalNormal(p);
            if (isWithin(base, np)) {
                fs::path rel = np.lexically_relative(base);
                if (!rel.empty() && rel.native().rfind(L"..", 0) != 0) p = rel;
            }
        }
        std::wstring s = p.generic_wstring();
        for (wchar_t& ch : s) if (ch == L'\r' || ch == L'\n') ch = L' ';
        emit(s);
        emit(L"\r\n");
    }
    return options.utf8 ? fsutil::wideToUtf8(out) : fsutil::wideToAcp(out);
}

} // namespace vgmplay::playlist
