// vgmplay - modul loader: loadTrackFromFile / loadTrackFromMemory (ADR 0002 pkt 8).
// Watek: blokujace; wolac z workera. Bez wyjatkow na zewnatrz (bad_alloc -> OutOfMemory).
#include "vgmplay/loader.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fstream>
#endif

namespace vgmplay::loader {
namespace {

[[nodiscard]] Error cancelled()
{
    return makeError(ErrorCode::Cancelled, "loading cancelled");
}

[[nodiscard]] bool isGzipMagic(std::span<const std::uint8_t> d) noexcept
{
    return d.size() >= 2 && d[0] == 0x1F && d[1] == 0x8B;
}

// Sciezki w stylu URL ("http://...", "file://...") odrzucamy jawnie - loader czyta tylko pliki lokalne.
[[nodiscard]] bool looksLikeUrl(const std::filesystem::path& p)
{
    const std::wstring s = p.generic_wstring();
    const std::size_t pos = s.find(L"://");
    if (pos == std::wstring::npos || pos < 2) return false;   // pos 1 = litera dysku "C://"
    for (std::size_t i = 0; i < pos; ++i) {
        const wchar_t c = s[i];
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                        c == L'+' || c == L'-' || c == L'.';
        if (!ok) return false;
    }
    return true;
}

// Wspolna czesc: dane (juz w pamieci) -> LoadedTrack. `owned` (jesli nie null) jest przejmowany bez kopii.
Result<LoadedTrack> loadImpl(std::span<const std::uint8_t> input, ByteBuffer* owned,
                             const std::filesystem::path& path, const format::Limits& limits,
                             const CancelToken& cancel)
{
    if (cancel.isCancelled()) return cancelled();

    format::Decompressed dec;
    if (isGzipMagic(input)) {
        auto r = format::decompressIfGzip(input, limits);
        if (!r) return r.error();
        dec = std::move(r).value();
    } else {
        if (input.size() > limits.maxInputBytes)
            return makeError(ErrorCode::TooLarge, "input exceeds size limit");
        if (owned) dec.bytes = std::move(*owned);
        else dec.bytes.assign(input.begin(), input.end());
        dec.wasGzip = false;
    }
    if (cancel.isCancelled()) return cancelled();

    LoadedTrack track;
    {
        auto h = format::parseVgmHeader(dec.bytes, limits);
        if (!h) return h.error();
        track.header = std::move(h).value();
    }
    if (cancel.isCancelled()) return cancelled();

    {
        auto s = format::validateCommandStream(dec.bytes, track.header, limits);
        if (!s) return s.error();
        track.stream = std::move(s).value();
    }
    if (cancel.isCancelled()) return cancelled();

    track.warnings = track.header.warnings;
    track.warnings.insert(track.warnings.end(), track.stream.warnings.begin(), track.stream.warnings.end());

    // GD3 jest niekrytyczny: blad -> ostrzezenie Gd3Invalid i puste tagi.
    {
        auto g = format::parseGd3(dec.bytes, track.header, limits);
        if (g) {
            track.gd3 = std::move(g).value();
            if (track.gd3.truncated)
                track.warnings.push_back({WarningCode::TagTruncated, track.header.gd3Offset, "GD3 field truncated to the length limit"});
            if (track.gd3.sanitized)
                track.warnings.push_back({WarningCode::TagSanitized, track.header.gd3Offset, "GD3 text sanitized (control characters / unpaired surrogates)"});
        } else {
            track.warnings.push_back({WarningCode::Gd3Invalid, g.error().byteOffset, "GD3 tag invalid: " + g.error().message});
        }
    }

    // Dopelnienie zerami (pkt 8.7): resize wartosciuje nowe bajty zerami.
    const std::size_t rawSize = dec.bytes.size();
    dec.bytes.resize(rawSize + format::kParserPadding);
    track.bytes = std::make_shared<const ByteBuffer>(std::move(dec.bytes));
    track.path = path;
    track.wasGzip = dec.wasGzip;
    return track;
}

template <class F>
Result<LoadedTrack> guarded(F&& f) noexcept
{
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory while loading track");
    } catch (...) {
        return makeError(ErrorCode::Internal, "unexpected exception while loading track");
    }
}

#if defined(_WIN32)

[[nodiscard]] Error winError(DWORD code, const char* what)
{
    ErrorCode ec = ErrorCode::Io;
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME: ec = ErrorCode::NotFound; break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: ec = ErrorCode::AccessDenied; break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: ec = ErrorCode::OutOfMemory; break;
    default: break;
    }
    return makeError(ec, std::string(what) + " (Win32 error " + std::to_string(code) + ")");
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle()
    {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

Result<ByteBuffer> readFile(const std::filesystem::path& path, const format::Limits& limits,
                            const CancelToken& cancel)
{
    Handle f;
    f.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) {
            const DWORD attr = GetFileAttributesW(path.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                return makeError(ErrorCode::InvalidArgument, "path is a directory");
        }
        return winError(e, "cannot open file");
    }
    // Tylko zwykly plik na dysku: odrzuca urzadzenia znakowe (CON, NUL, COMx) i potoki.
    if (GetFileType(f.h) != FILE_TYPE_DISK)
        return makeError(ErrorCode::PathRejected, "not a regular file (device or pipe)");
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(f.h, &info)) return winError(GetLastError(), "cannot query file information");
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return makeError(ErrorCode::InvalidArgument, "path is a directory");

    // Rozmiar z otwartego uchwytu (bez TOCTOU), PRZED alokacja.
    const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > limits.maxInputBytes) return makeError(ErrorCode::TooLarge, "file exceeds size limit");

    ByteBuffer buf(static_cast<std::size_t>(size));
    constexpr DWORD kChunk = 1u << 20;
    std::size_t done = 0;
    while (done < buf.size()) {
        if (cancel.isCancelled()) return cancelled();
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(kChunk, buf.size() - done));
        DWORD got = 0;
        if (!ReadFile(f.h, buf.data() + done, want, &got, nullptr)) return winError(GetLastError(), "read error");
        if (got == 0) break;    // plik skrocony w trakcie czytania
        done += got;
    }
    if (done != buf.size()) return makeError(ErrorCode::Io, "file shrank while reading");
    return buf;
}

#else  // przenosny wariant zapasowy (projekt celuje w Windows)

Result<ByteBuffer> readFile(const std::filesystem::path& path, const format::Limits& limits,
                            const CancelToken& cancel)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return makeError(ec ? ErrorCode::NotFound : ErrorCode::PathRejected, "not a regular file");
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return makeError(ErrorCode::Io, "cannot open file");
    const std::streamoff size = in.tellg();
    if (size < 0) return makeError(ErrorCode::Io, "cannot determine file size");
    if (static_cast<std::uint64_t>(size) > limits.maxInputBytes)
        return makeError(ErrorCode::TooLarge, "file exceeds size limit");
    in.seekg(0);
    ByteBuffer buf(static_cast<std::size_t>(size));
    if (cancel.isCancelled()) return cancelled();
    if (!buf.empty() && !in.read(reinterpret_cast<char*>(buf.data()), size)) return makeError(ErrorCode::Io, "read error");
    return buf;
}

#endif

} // namespace

Result<LoadedTrack> loadTrackFromFile(const std::filesystem::path& path, const format::Limits& limits,
                                      const CancelToken& cancel)
{
    return guarded([&]() -> Result<LoadedTrack> {
        if (path.empty()) return makeError(ErrorCode::InvalidArgument, "empty path");
        if (looksLikeUrl(path)) return makeError(ErrorCode::PathRejected, "URLs are not supported");
        if (cancel.isCancelled()) return cancelled();
        auto data = readFile(path, limits, cancel);
        if (!data) return data.error();
        ByteBuffer buf = std::move(data).value();
        return loadImpl(buf, &buf, path, limits, cancel);
    });
}

Result<LoadedTrack> loadTrackFromMemory(std::span<const std::uint8_t> input, const std::filesystem::path& path,
                                        const format::Limits& limits, const CancelToken& cancel)
{
    return guarded([&]() -> Result<LoadedTrack> { return loadImpl(input, nullptr, path, limits, cancel); });
}

} // namespace vgmplay::loader
