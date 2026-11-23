// vgmplay - modul format: decompressIfGzip (zlib, strumieniowo, z limitami).
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include "vgmplay/format.hpp"

namespace vgmplay::format {
namespace {

constexpr std::size_t kChunk = 64 * 1024;
// Wspolczynnik kompresji jest sprawdzany dopiero powyzej tego rozmiaru wyjscia (male pliki
// o duzej redundancji sa uprawnione, a i tak ogranicza je maxDecompressedBytes).
constexpr std::uint64_t kRatioFloor = 1ull << 20;

[[nodiscard]] bool isGzipMagic(std::span<const std::uint8_t> d) noexcept
{
    return d.size() >= 2 && d[0] == 0x1F && d[1] == 0x8B;
}

Result<Decompressed> inflateGzip(std::span<const std::uint8_t> input, const Limits& limits)
{
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    // 16 + 15: WYLACZNIE gzip (magic juz sprawdzony); zlib sam weryfikuje CRC32 i ISIZE mod 2^32
    // na koncu, ale rozmiar docelowy NIE pochodzi z ISIZE.
    const int initRet = inflateInit2(&zs, 16 + 15);
    if (initRet == Z_MEM_ERROR) return makeError(ErrorCode::OutOfMemory, "inflateInit2: out of memory");
    if (initRet != Z_OK) return makeError(ErrorCode::Internal, "inflateInit2 failed");

    struct Guard {
        z_stream* z;
        ~Guard() { inflateEnd(z); }
    } guard{&zs};

    Decompressed result;
    result.wasGzip = true;
    ByteBuffer& out = result.bytes;

    const std::uint8_t* inPtr = input.data();
    std::uint64_t inRemaining = input.size();
    std::uint64_t ratioCap = limits.maxDecompressRatio == 0
                                 ? std::numeric_limits<std::uint64_t>::max()
                                 : static_cast<std::uint64_t>(input.size()) * limits.maxDecompressRatio;

    for (;;) {
        // Porcja wejscia (uInt moze byc 32-bitowy).
        const std::uint64_t inChunk =
            std::min<std::uint64_t>(inRemaining, std::numeric_limits<uInt>::max());
        zs.next_in = const_cast<Bytef*>(inPtr);
        zs.avail_in = static_cast<uInt>(inChunk);

        const std::uint64_t room = limits.maxDecompressedBytes - out.size();
        // +1: pozwala wykryc przekroczenie limitu (dane ponad limit nie sa zachowywane).
        const std::size_t want = room >= kChunk ? kChunk : static_cast<std::size_t>(room + 1);
        const std::size_t oldSize = out.size();
        out.resize(oldSize + want);
        zs.next_out = out.data() + oldSize;
        zs.avail_out = static_cast<uInt>(want);

        const int ret = inflate(&zs, Z_NO_FLUSH);
        const std::size_t produced = want - zs.avail_out;
        out.resize(oldSize + produced);
        const std::uint64_t consumed = inChunk - zs.avail_in;
        inPtr += consumed;
        inRemaining -= consumed;

        if (out.size() > limits.maxDecompressedBytes)
            return makeError(ErrorCode::TooLarge, "decompressed size exceeds limit");
        if (out.size() > kRatioFloor && out.size() > ratioCap)
            return makeError(ErrorCode::TooLarge, "decompression ratio exceeds limit (gzip bomb?)");

        if (ret == Z_STREAM_END) break;
        if (ret == Z_MEM_ERROR) return makeError(ErrorCode::OutOfMemory, "inflate: out of memory");
        if (ret == Z_DATA_ERROR || ret == Z_NEED_DICT || ret == Z_STREAM_ERROR)
            return makeError(ErrorCode::DecompressionFailed, "corrupt gzip data");
        // Z_OK albo Z_BUF_ERROR: brak postepu przy wyczerpanym wejsciu = uciety strumien.
        if (inRemaining == 0 && zs.avail_in == 0 && zs.avail_out != 0)
            return makeError(ErrorCode::DecompressionFailed, "truncated gzip stream (no end marker)",
                             input.size());
        if (consumed == 0 && produced == 0)
            return makeError(ErrorCode::DecompressionFailed, "gzip stream makes no progress");
    }

    out.shrink_to_fit();
    // Gzip w gzip (po rozpakowaniu znowu 1F 8B) - odrzucamy (ADR 0002 pkt 8.2).
    if (isGzipMagic(out)) return makeError(ErrorCode::BadSignature, "nested gzip stream");
    return result;
}

} // namespace

Result<Decompressed> decompressIfGzip(std::span<const std::uint8_t> input, const Limits& limits)
{
    try {
        if (input.size() > limits.maxInputBytes)
            return makeError(ErrorCode::TooLarge, "input exceeds size limit");

        if (isGzipMagic(input)) return inflateGzip(input, limits);

        Decompressed plain;
        plain.wasGzip = false;
        plain.bytes.assign(input.begin(), input.end());
        return plain;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory while decompressing");
    } catch (...) {
        return makeError(ErrorCode::Internal, "unexpected exception in decompressIfGzip");
    }
}

} // namespace vgmplay::format
