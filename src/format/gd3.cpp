// vgmplay - modul format: GD3 (UTF-16LE -> UTF-8, wlasny dekoder, bez WinAPI).
#include <algorithm>
#include <cstring>
#include <new>

#include "internal.hpp"
#include "vgmplay/format.hpp"

namespace vgmplay::format {
namespace detail {
namespace {

void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

[[nodiscard]] constexpr std::size_t utf8Len(std::uint32_t cp) noexcept
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

} // namespace

Utf8Result utf16leToUtf8(std::span<const std::uint8_t> bytes, std::uint32_t maxBytes)
{
    Utf8Result r;
    const std::size_t units = bytes.size() / 2;
    r.text.reserve(std::min<std::size_t>(units * 3, maxBytes));

    for (std::size_t i = 0; i < units; ++i) {
        std::uint32_t cp = rd16(&bytes[i * 2]);
        if (cp == 0) break;    // terminator (rozdzielenie pol robi wywolujacy; tu: obrona)

        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 < units) {
                const std::uint32_t lo = rd16(&bytes[(i + 1) * 2]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                } else {
                    cp = 0xFFFD;
                    r.sanitized = true;
                }
            } else {
                cp = 0xFFFD;
                r.sanitized = true;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
            r.sanitized = true;
        } else if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
                   (cp >= 0x2066 && cp <= 0x2069)) {
            // Znaki niewidoczne i kierunkowe (zero-width, LRM/RLM, LRE..RLO, izolaty): pozwalaja
            // podszyc/odwrocic wyswietlany tekst (np. nazwe pliku/tytul) - usuwamy.
            r.sanitized = true;
            continue;
        } else if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
            if (cp == '\t' || cp == '\n' || cp == '\r') {
                // CRLF -> jedna spacja; pojedyncze znaki biale -> spacja (jedna linia w UI). To zwykle
                // formatowanie (np. wieloliniowe "Notes" z vgmrips), nie podejrzana tresc: bez flagi sanitized.
                if (cp == '\r' && i + 1 < units && rd16(&bytes[(i + 1) * 2]) == '\n') ++i;
                cp = ' ';
            } else {
                r.sanitized = true;
                continue;   // pozostale znaki sterujace usuwamy
            }
        }

        if (r.text.size() + utf8Len(cp) > maxBytes) {
            r.truncated = true;
            break;
        }
        appendUtf8(r.text, cp);
    }
    return r;
}

} // namespace detail

const std::string& Gd3Tag::track(Gd3Language l) const noexcept
{
    const std::string& a = (l == Gd3Language::En) ? trackEn : trackJp;
    return a.empty() ? ((l == Gd3Language::En) ? trackJp : trackEn) : a;
}
const std::string& Gd3Tag::game(Gd3Language l) const noexcept
{
    const std::string& a = (l == Gd3Language::En) ? gameEn : gameJp;
    return a.empty() ? ((l == Gd3Language::En) ? gameJp : gameEn) : a;
}
const std::string& Gd3Tag::system(Gd3Language l) const noexcept
{
    const std::string& a = (l == Gd3Language::En) ? systemEn : systemJp;
    return a.empty() ? ((l == Gd3Language::En) ? systemJp : systemEn) : a;
}
const std::string& Gd3Tag::author(Gd3Language l) const noexcept
{
    const std::string& a = (l == Gd3Language::En) ? authorEn : authorJp;
    return a.empty() ? ((l == Gd3Language::En) ? authorJp : authorEn) : a;
}

Result<Gd3Tag> parseGd3(std::span<const std::uint8_t> file, const HeaderInfo& header, const Limits& limits)
{
    try {
        Gd3Tag tag;
        if (header.gd3Offset == 0) return tag;   // brak GD3 - nie blad

        const std::uint64_t size = std::min<std::uint64_t>(file.size(), header.fileSize);
        const std::uint64_t ofs = header.gd3Offset;
        if (ofs > size || size - ofs < 12)
            return makeError(ErrorCode::Truncated, "GD3 header does not fit in file", ofs);

        const std::uint8_t* base = file.data() + ofs;
        if (std::memcmp(base, "Gd3 ", 4) != 0)
            return makeError(ErrorCode::BadSignature, "bad GD3 signature", ofs);
        const std::uint32_t version = detail::rd32(base + 4);
        if (version < 0x100 || version >= 0x200)
            return makeError(ErrorCode::UnsupportedVersion, "unsupported GD3 version", ofs + 4);
        const std::uint32_t declared = detail::rd32(base + 8);

        // Dlugosc deklarowana jest przycinana do pliku (jak libvgm); nieparzysty bajt ignorujemy.
        const std::uint64_t dataStart = ofs + 12;
        const std::uint64_t dataEnd = std::min<std::uint64_t>(dataStart + declared, size);
        std::uint64_t pos = dataStart;

        std::string* fields[11] = {&tag.trackEn,  &tag.trackJp,   &tag.gameEn,   &tag.gameJp,
                                   &tag.systemEn, &tag.systemJp,  &tag.authorEn, &tag.authorJp,
                                   &tag.releaseDate, &tag.converter, &tag.notes};
        for (std::string* f : fields) {
            if (pos + 2 > dataEnd) break;     // brakujace pola = puste (bez terminatorow w kontrakcie)
            const std::uint64_t start = pos;
            while (pos + 2 <= dataEnd && detail::rd16(file.data() + pos) != 0) pos += 2;
            const std::uint64_t len = pos - start;
            // Pole jest dekodowane takze wtedy, gdy zabraklo terminatora (koniec danych).
            auto res = detail::utf16leToUtf8(
                std::span<const std::uint8_t>(file.data() + start, static_cast<std::size_t>(len)),
                limits.maxTagFieldBytes);
            *f = std::move(res.text);
            tag.sanitized = tag.sanitized || res.sanitized;
            tag.truncated = tag.truncated || res.truncated;
            pos += 2;   // terminator
        }
        tag.present = true;
        return tag;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory while parsing GD3");
    } catch (...) {
        return makeError(ErrorCode::Internal, "unexpected exception in parseGd3");
    }
}

} // namespace vgmplay::format
