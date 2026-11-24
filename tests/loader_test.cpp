// Testy modulu loader: loadTrackFromMemory / loadTrackFromFile (dane budowane z bajtow).
#include <doctest/doctest.h>

#include <ostream>   // wymagane przez doctest dla stringifikacji std::string_view

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "vgmplay/loader.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using namespace vgmplay;
using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;

void put32(Bytes& v, std::size_t ofs, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v[ofs + i] = static_cast<std::uint8_t>(x >> (8 * i));
}
void append32(Bytes& v, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

// Minimalny VGM 1.71: naglowek 0x100 B, SN76496 3.58 MHz, komendy, opcjonalny GD3 (surowe bajty).
Bytes makeVgm(const Bytes& cmds, const Bytes& gd3 = {})
{
    Bytes v(0x100, 0);
    std::memcpy(v.data(), "Vgm ", 4);
    put32(v, 0x08, 0x171);
    put32(v, 0x34, 0x100 - 0x34);
    put32(v, 0x0C, 3579545);
    v.insert(v.end(), cmds.begin(), cmds.end());
    if (!gd3.empty()) {
        put32(v, 0x14, static_cast<std::uint32_t>(v.size() - 0x14));
        v.insert(v.end(), gd3.begin(), gd3.end());
    }
    put32(v, 0x04, static_cast<std::uint32_t>(v.size() - 4));
    return v;
}

const Bytes kCmds = {0x50, 0x9F, 0x62, 0x63, 0x66};   // 735 + 882 ticks

Bytes makeGd3(const std::u16string& title)
{
    Bytes body;
    for (char16_t c : title) {
        body.push_back(static_cast<std::uint8_t>(c & 0xFF));
        body.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    body.push_back(0);
    body.push_back(0);
    Bytes g = {'G', 'd', '3', ' '};
    append32(g, 0x100);
    append32(g, static_cast<std::uint32_t>(body.size()));
    g.insert(g.end(), body.begin(), body.end());
    return g;
}

std::uint32_t crc32(const Bytes& d)
{
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::uint8_t b : d) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

Bytes gzipStored(const Bytes& data)
{
    Bytes g = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
    const std::size_t n = data.size();   // < 65536 w tych testach
    g.push_back(1);
    g.push_back(static_cast<std::uint8_t>(n & 0xFF));
    g.push_back(static_cast<std::uint8_t>(n >> 8));
    g.push_back(static_cast<std::uint8_t>(~n & 0xFF));
    g.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
    g.insert(g.end(), data.begin(), data.end());
    append32(g, crc32(data));
    append32(g, static_cast<std::uint32_t>(n));
    return g;
}

bool hasWarning(const std::vector<Warning>& w, WarningCode c)
{
    return std::any_of(w.begin(), w.end(), [c](const Warning& x) { return x.code == c; });
}

// Katalog tymczasowy sprzatany w destruktorze.
struct TempDir {
    fs::path path;
    TempDir()
    {
        std::error_code ec;
        path = fs::temp_directory_path(ec) / (L"vgmplay_loader_test_\u0105\u0119_" + std::to_wstring(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path, ec);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path& p, const Bytes& data)
{
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

} // namespace

TEST_CASE("loader: poprawny VGM z pamieci - pola, dopelnienie zerami, niemutowalnosc")
{
    const Bytes raw = makeVgm(kCmds, makeGd3(u"Song"));
    auto r = loader::loadTrackFromMemory(raw, "memory.vgm");
    REQUIRE(r.ok());
    const loader::LoadedTrack& t = r.value();

    CHECK_FALSE(t.wasGzip);
    CHECK(t.path == fs::path("memory.vgm"));
    CHECK(t.header.fileSize == raw.size());
    CHECK(t.stream.totalSamples == 735 + 882);
    CHECK(t.stream.hasEndCommand);
    CHECK(t.gd3.present);
    CHECK(t.gd3.trackEn == "Song");
    CHECK(t.warnings.empty());

    REQUIRE(t.bytes);
    CHECK(t.bytes->size() == raw.size() + format::kParserPadding);
    CHECK(format::kParserPadding >= 0x110);
    CHECK(std::equal(raw.begin(), raw.end(), t.bytes->begin()));
    CHECK(std::all_of(t.bytes->begin() + static_cast<std::ptrdiff_t>(raw.size()), t.bytes->end(),
                      [](std::uint8_t b) { return b == 0; }));
    CHECK(t.raw().size() == raw.size());
    CHECK(std::equal(raw.begin(), raw.end(), t.raw().begin()));

    static_assert(std::is_same_v<decltype(t.bytes), std::shared_ptr<const ByteBuffer>>);
}

TEST_CASE("loader: wejscie nie jest zmieniane, a wynik nie zalezy od jego czasu zycia")
{
    Bytes raw = makeVgm(kCmds);
    const Bytes copy = raw;
    auto r = loader::loadTrackFromMemory(raw);
    REQUIRE(r.ok());
    CHECK(raw == copy);
    std::fill(raw.begin(), raw.end(), 0xEE);
    CHECK(std::equal(copy.begin(), copy.end(), r.value().bytes->begin()));
}

TEST_CASE("loader: gzip rozpoznawany po magic, wasGzip = true, wynik identyczny z surowym")
{
    const Bytes raw = makeVgm(kCmds, makeGd3(u"Zipped"));
    auto plain = loader::loadTrackFromMemory(raw);
    auto gz = loader::loadTrackFromMemory(gzipStored(raw));
    REQUIRE(plain.ok());
    REQUIRE(gz.ok());
    CHECK(gz.value().wasGzip);
    CHECK(*gz.value().bytes == *plain.value().bytes);
    CHECK(gz.value().gd3.trackEn == "Zipped");

    // uciety gzip
    Bytes cut = gzipStored(raw);
    cut.resize(cut.size() - 5);
    auto bad = loader::loadTrackFromMemory(cut);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::DecompressionFailed);
}

TEST_CASE("loader: bledy walidacji sa przekazywane z kodem i offsetem")
{
    // sygnatura
    Bytes sig = makeVgm(kCmds);
    sig[1] = 'X';
    auto a = loader::loadTrackFromMemory(sig);
    REQUIRE_FALSE(a.ok());
    CHECK(a.error().code == ErrorCode::BadSignature);

    // pusty i krotki
    CHECK(loader::loadTrackFromMemory(Bytes{}).error().code == ErrorCode::Truncated);
    CHECK(loader::loadTrackFromMemory(Bytes(10, 0)).error().code == ErrorCode::Truncated);

    // komenda niewazna
    auto b = loader::loadTrackFromMemory(makeVgm(Bytes{0x63, 0x05, 0x66}));
    REQUIRE_FALSE(b.ok());
    CHECK(b.error().code == ErrorCode::BadCommandStream);
    CHECK(b.error().byteOffset == 0x101);

    // blok danych wiekszy niz plik
    Bytes blk = {0x67, 0x66, 0x00, 0xF0, 0xFF, 0xFF, 0x7F, 1, 2, 3, 0x66};
    auto c = loader::loadTrackFromMemory(makeVgm(blk));
    REQUIRE_FALSE(c.ok());
    CHECK(c.error().code == ErrorCode::BadDataBlock);
}

TEST_CASE("loader: uszkodzony GD3 jest ostrzezeniem, nie bledem")
{
    Bytes gd3 = makeGd3(u"x");
    gd3[0] = 'Z';   // zla sygnatura
    auto r = loader::loadTrackFromMemory(makeVgm(kCmds, gd3));
    REQUIRE(r.ok());
    CHECK_FALSE(r.value().gd3.present);
    CHECK(hasWarning(r.value().warnings, WarningCode::Gd3Invalid));
}

TEST_CASE("loader: ostrzezenia z naglowka, strumienia i GD3 sa zsumowane")
{
    // brak 0x66 (stream), eof = 0 (header), niesparowany surogat (GD3)
    Bytes v = makeVgm(Bytes{0x50, 0x9F, 0x63}, makeGd3(std::u16string{u'a', 0xD800}));
    put32(v, 0x04, 0);
    auto r = loader::loadTrackFromMemory(v);
    REQUIRE(r.ok());
    const auto& w = r.value().warnings;
    CHECK(hasWarning(w, WarningCode::EofOffsetMismatch));
    CHECK(hasWarning(w, WarningCode::EndsEarly));
    CHECK(hasWarning(w, WarningCode::TagSanitized));
}

TEST_CASE("loader: limit rozmiaru wejscia i anulowanie")
{
    const Bytes raw = makeVgm(kCmds);
    format::Limits l;
    l.maxInputBytes = raw.size() - 1;
    auto big = loader::loadTrackFromMemory(raw, {}, l);
    REQUIRE_FALSE(big.ok());
    CHECK(big.error().code == ErrorCode::TooLarge);

    CancelToken tok;
    tok.requestCancel();
    auto c = loader::loadTrackFromMemory(raw, {}, {}, tok);
    REQUIRE_FALSE(c.ok());
    CHECK(c.error().code == ErrorCode::Cancelled);
}

TEST_CASE("loader: wspoldzielony wynik czytany z wielu watkow")
{
    const Bytes raw = makeVgm(kCmds);
    auto r = loader::loadTrackFromMemory(raw);
    REQUIRE(r.ok());
    auto shared = std::make_shared<const loader::LoadedTrack>(std::move(r).value());
    std::vector<std::thread> th;
    std::vector<int> okFlags(4, 0);
    for (int i = 0; i < 4; ++i) {
        th.emplace_back([&, i] {
            auto again = loader::loadTrackFromMemory(shared->raw());   // rownolegle ladowanie bez stanu globalnego
            okFlags[static_cast<std::size_t>(i)] = again.ok() && *again.value().bytes == *shared->bytes;
        });
    }
    for (auto& t : th) t.join();
    CHECK(std::all_of(okFlags.begin(), okFlags.end(), [](int f) { return f == 1; }));
}

TEST_CASE("loader: loadTrackFromFile - sciezka Unicode, gzip, bledy")
{
    TempDir dir;
    REQUIRE(fs::is_directory(dir.path));

    const Bytes raw = makeVgm(kCmds, makeGd3(u"File"));
    const fs::path plainPath = dir.path / fs::path(L"utwor_\u30c6\u30b9\u30c8_\u0142.vgm");
    const fs::path gzPath = dir.path / fs::path(L"utwor.vgz");
    writeFile(plainPath, raw);
    writeFile(gzPath, gzipStored(raw));

    auto r = loader::loadTrackFromFile(plainPath);
    REQUIRE(r.ok());
    CHECK(r.value().path == plainPath);
    CHECK(r.value().gd3.trackEn == "File");
    CHECK(r.value().header.fileSize == raw.size());
    CHECK_FALSE(r.value().wasGzip);

    auto g = loader::loadTrackFromFile(gzPath);
    REQUIRE(g.ok());
    CHECK(g.value().wasGzip);
    CHECK(*g.value().bytes == *r.value().bytes);

    // gzip rozpoznawany po zawartosci, nie po rozszerzeniu
    const fs::path disguised = dir.path / fs::path(L"niby.vgm");
    writeFile(disguised, gzipStored(raw));
    auto d = loader::loadTrackFromFile(disguised);
    REQUIRE(d.ok());
    CHECK(d.value().wasGzip);

    // nieistniejacy plik
    auto nf = loader::loadTrackFromFile(dir.path / "brak.vgm");
    REQUIRE_FALSE(nf.ok());
    CHECK(nf.error().code == ErrorCode::NotFound);

    // katalog
    auto dd = loader::loadTrackFromFile(dir.path);
    REQUIRE_FALSE(dd.ok());
    CHECK(dd.error().code == ErrorCode::InvalidArgument);

    // pusta sciezka i URL
    CHECK(loader::loadTrackFromFile(fs::path()).error().code == ErrorCode::InvalidArgument);
    CHECK(loader::loadTrackFromFile(fs::path("http://example.com/a.vgm")).error().code == ErrorCode::PathRejected);
    CHECK(loader::loadTrackFromFile(fs::path("file:///C:/a.vgm")).error().code == ErrorCode::PathRejected);

    // limit rozmiaru sprawdzany PRZED odczytem
    format::Limits l;
    l.maxInputBytes = 16;
    auto big = loader::loadTrackFromFile(plainPath, l);
    REQUIRE_FALSE(big.ok());
    CHECK(big.error().code == ErrorCode::TooLarge);

    // pusty plik
    const fs::path emptyPath = dir.path / "pusty.vgm";
    writeFile(emptyPath, {});
    auto e = loader::loadTrackFromFile(emptyPath);
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().code == ErrorCode::Truncated);

    // anulowanie
    CancelToken tok;
    tok.requestCancel();
    auto c = loader::loadTrackFromFile(plainPath, {}, tok);
    REQUIRE_FALSE(c.ok());
    CHECK(c.error().code == ErrorCode::Cancelled);
}

#if defined(_WIN32)
TEST_CASE("loader: urzadzenia (NUL, CON) odrzucane - tylko zwykle pliki")
{
    for (const char* name : {"NUL", "CON"}) {
        auto r = loader::loadTrackFromFile(fs::path(name));
        REQUIRE_FALSE(r.ok());
        CHECK_MESSAGE((r.error().code == ErrorCode::PathRejected || r.error().code == ErrorCode::NotFound ||
                       r.error().code == ErrorCode::AccessDenied),
                      name, ": code ", static_cast<int>(r.error().code), " ", r.error().message);
    }
}
#endif

// ---------------------------------------------------------------------------
// Poprawki z recenzji bezpieczenstwa (ADR 0002 pkt 8.15-8.18)
// ---------------------------------------------------------------------------

TEST_CASE("loader: eofOfs w kopii bufora wskazuje dokladnie eof uzyty przez parser (libvgm liczy rozmiar z paddingiem)")
{
    const Bytes v = makeVgm(kCmds);
    const std::size_t n = v.size();
    auto rd32 = [](const Bytes& b, std::size_t o) {
        return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
               (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
    };
    // 0 (libvgm: rozmiar Z paddingiem), w padding, na koniec paddingu, poza paddingiem, ogromny
    for (std::uint32_t rel : {0u, static_cast<std::uint32_t>(n + 8 - 4), static_cast<std::uint32_t>(n + 0x110 - 4),
                              static_cast<std::uint32_t>(n + 0x400), 0x7FFFFFFFu}) {
        Bytes f = v;
        put32(f, 0x04, rel);
        auto r = loader::loadTrackFromMemory(f);
        REQUIRE(r.ok());
        CHECK(hasWarning(r.value().warnings, WarningCode::EofOffsetMismatch));
        const Bytes& b = *r.value().bytes;
        REQUIRE(b.size() == n + format::kParserPadding);
        CHECK(rd32(b, 0x04) == n - 4);                       // libvgm: eofOfs = 4 + rel = rozmiar pliku
        CHECK(r.value().header.eofOffset == n);
        CHECK(r.value().header.dataEnd == n);
        // padding pozostaje zerami
        CHECK(std::all_of(b.begin() + static_cast<std::ptrdiff_t>(n), b.end(), [](std::uint8_t x) { return x == 0; }));
    }
    // poprawny eof (w tym ogon za eof) - zapisywana ta sama wartosc, bez ostrzezenia
    Bytes tail = v;
    tail.insert(tail.end(), {0, 0, 0, 0});
    auto t = loader::loadTrackFromMemory(tail);
    REQUIRE(t.ok());
    CHECK_FALSE(hasWarning(t.value().warnings, WarningCode::EofOffsetMismatch));
    CHECK(rd32(*t.value().bytes, 0x04) == n - 4);
    CHECK(t.value().header.dataEnd == n);
}

#if defined(_WIN32)
TEST_CASE("loader: potok nazwany jest odrzucany bez polaczenia klienta (typ pliku sprawdzany przed otwarciem z GENERIC_READ)")
{
    const std::wstring name = L"\\\\.\\pipe\\vgmplay_loader_test_" + std::to_wstring(GetCurrentProcessId());
    HANDLE srv = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    REQUIRE(srv != INVALID_HANDLE_VALUE);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(ov.hEvent != nullptr);
    const BOOL immediate = ConnectNamedPipe(srv, &ov);
    REQUIRE((immediate == FALSE && GetLastError() == ERROR_IO_PENDING));

    auto r = loader::loadTrackFromFile(fs::path(name));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::PathRejected);
    // serwer nie zobaczyl klienta: otwarcie z prawem odczytu (stary kod) zakonczyloby ConnectNamedPipe
    CHECK(WaitForSingleObject(ov.hEvent, 0) == WAIT_TIMEOUT);

    CancelIo(srv);
    CloseHandle(ov.hEvent);
    CloseHandle(srv);
}
#endif
