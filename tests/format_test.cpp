// Testy modulu format: gzip, naglowek VGM, GD3, pre-skan strumienia komend, nazwy ukladow.
// Dane buduje sie w kodzie z bajtow (bez plikow).
#include <doctest/doctest.h>

#include <ostream>   // wymagane przez doctest dla stringifikacji std::string_view

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "vgmplay/format.hpp"

// Tabela komend jest wewnetrzna dla modulu format, ale test sprawdza jej wybrane wpisy.
#include "../src/format/cmdtable.hpp"

namespace {

using namespace vgmplay;
using namespace vgmplay::format;
using Bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------------------
// Budowanie danych
// ---------------------------------------------------------------------------

void put32(Bytes& v, std::size_t ofs, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v[ofs + i] = static_cast<std::uint8_t>(x >> (8 * i));
}
void append(Bytes& v, const Bytes& b) { v.insert(v.end(), b.begin(), b.end()); }
void append32(Bytes& v, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

struct Opt {
    std::uint32_t version = 0x171;
    std::size_t hdrSize = 0x100;
    std::uint32_t snClock = 3579545;
    std::vector<std::pair<std::size_t, std::uint32_t>> clocks;   // dodatkowe pola (offset, wartosc)
    std::int64_t loopAt = -1;                                    // offset w cmds
    std::uint32_t loopSamples = 100;
    Bytes gd3;
    Bytes tail;                                                  // bajty po komendach i GD3 (poza EOF jesli eofCoversTail=false)
};

Bytes makeVgm(const Bytes& cmds, const Opt& o = {})
{
    Bytes v(o.hdrSize, 0);
    std::memcpy(v.data(), "Vgm ", 4);
    put32(v, 0x08, o.version);
    if (o.version >= 0x150) put32(v, 0x34, static_cast<std::uint32_t>(o.hdrSize - 0x34));
    put32(v, 0x0C, o.snClock);
    for (auto& [ofs, val] : o.clocks) put32(v, ofs, val);
    append(v, cmds);
    if (!o.gd3.empty()) {
        put32(v, 0x14, static_cast<std::uint32_t>(v.size() - 0x14));
        append(v, o.gd3);
    }
    append(v, o.tail);
    put32(v, 0x04, static_cast<std::uint32_t>(v.size() - 4));
    put32(v, 0x18, 1000);
    if (o.loopAt >= 0) {
        put32(v, 0x1C, static_cast<std::uint32_t>(o.hdrSize + o.loopAt - 0x1C));
        put32(v, 0x20, o.loopSamples);
    }
    return v;
}

Bytes block(std::uint8_t type, const Bytes& payload, bool chip2 = false)
{
    Bytes b = {0x67, 0x66, type};
    append32(b, static_cast<std::uint32_t>(payload.size()) | (chip2 ? 0x80000000u : 0u));
    append(b, payload);
    return b;
}

Bytes comprPayload(std::uint8_t comprType, std::uint32_t decmpLen, std::uint8_t bitsDec, std::uint8_t bitsCmp,
                   std::uint8_t sub, const Bytes& data)
{
    Bytes p = {comprType};
    append32(p, decmpLen);
    p.push_back(bitsDec);
    p.push_back(bitsCmp);
    p.push_back(sub);
    p.push_back(0);
    p.push_back(0);
    append(p, data);
    return p;
}

Bytes cat(std::initializer_list<Bytes> parts)
{
    Bytes r;
    for (const Bytes& p : parts) append(r, p);
    return r;
}

Bytes utf16(const std::u16string& s, bool terminate = true)
{
    Bytes b;
    for (char16_t c : s) {
        b.push_back(static_cast<std::uint8_t>(c & 0xFF));
        b.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    if (terminate) { b.push_back(0); b.push_back(0); }
    return b;
}

Bytes makeGd3(const std::vector<std::u16string>& fields, std::uint32_t version = 0x100)
{
    Bytes body;
    for (const auto& f : fields) append(body, utf16(f));
    Bytes g = {'G', 'd', '3', ' '};
    append32(g, version);
    append32(g, static_cast<std::uint32_t>(body.size()));
    append(g, body);
    return g;
}

const Bytes kSimple = {0x50, 0x9F, 0x61, 0x10, 0x00, 0x63, 0x66};   // SN write, wait 16, wait 882, end

// Parsuje naglowek i skanuje strumien; wymaga sukcesu naglowka.
struct Scan {
    HeaderInfo hdr;
    Result<StreamReport> rep;
};
Scan scan(const Bytes& file, const Limits& l = {})
{
    auto h = parseVgmHeader(file, l);
    REQUIRE(h.ok());
    HeaderInfo hi = h.value();
    return Scan{hi, validateCommandStream(file, hi, l)};
}

bool hasWarning(const std::vector<Warning>& w, WarningCode c)
{
    return std::any_of(w.begin(), w.end(), [c](const Warning& x) { return x.code == c; });
}

// --- CRC32 i gzip z blokow "stored" ---
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
    std::size_t pos = 0;
    do {
        const std::size_t n = std::min<std::size_t>(0xFFFF, data.size() - pos);
        const bool last = pos + n >= data.size();
        g.push_back(last ? 1 : 0);
        g.push_back(static_cast<std::uint8_t>(n & 0xFF));
        g.push_back(static_cast<std::uint8_t>(n >> 8));
        g.push_back(static_cast<std::uint8_t>(~n & 0xFF));
        g.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
        g.insert(g.end(), data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < data.size());
    append32(g, crc32(data));
    append32(g, static_cast<std::uint32_t>(data.size()));
    return g;
}

// Gzip z bardzo dobrze kompresujacych sie zer: stala Huffmana, dopasowania 258 B / odleglosc 1.
Bytes gzipZeros(std::size_t outBytes)
{
    Bytes deflate;
    unsigned acc = 0, nbits = 0;
    auto putBits = [&](unsigned value, unsigned count, bool msbFirst) {
        for (unsigned i = 0; i < count; ++i) {
            const unsigned bit = msbFirst ? (value >> (count - 1 - i)) & 1u : (value >> i) & 1u;
            acc |= bit << nbits;
            if (++nbits == 8) { deflate.push_back(static_cast<std::uint8_t>(acc)); acc = 0; nbits = 0; }
        }
    };
    putBits(1, 1, false);        // BFINAL
    putBits(1, 2, false);        // BTYPE = 01 (stala Huffmana)
    putBits(0x30, 8, true);      // literal 0
    std::size_t produced = 1;
    while (produced + 258 <= outBytes) {
        putBits(0xC5, 8, true);  // symbol 285 = dlugosc 258
        putBits(0, 5, true);     // odleglosc 1
        produced += 258;
    }
    while (produced < outBytes) { putBits(0x30, 8, true); ++produced; }
    putBits(0, 7, true);         // koniec bloku (symbol 256)
    if (nbits) deflate.push_back(static_cast<std::uint8_t>(acc));

    Bytes g = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
    append(g, deflate);
    append32(g, crc32(Bytes(outBytes, 0)));
    append32(g, static_cast<std::uint32_t>(outBytes));
    return g;
}

} // namespace

// ===========================================================================
// chipTypeName
// ===========================================================================

TEST_CASE("format: chipTypeName - unikalne, stabilne nazwy dla wszystkich ukladow")
{
    std::set<std::string> seen;
    for (unsigned i = 0; i < static_cast<unsigned>(ChipType::Count_); ++i) {
        const auto n = chipTypeName(static_cast<ChipType>(i));
        CHECK_FALSE(n.empty());
        CHECK(seen.insert(std::string(n)).second);
        if (i != 0) CHECK(n != "Unknown");
    }
    CHECK(chipTypeName(ChipType::Unknown) == "Unknown");
    CHECK(chipTypeName(ChipType::Count_) == "Unknown");
    CHECK(chipTypeName(static_cast<ChipType>(255)) == "Unknown");
    CHECK(chipTypeName(ChipType::YM2612) == "YM2612");
    CHECK(chipTypeName(ChipType::SN76496) == "SN76496");
    CHECK(chipTypeName(ChipType::MultiPCM) == "MultiPCM");
    CHECK(chipTypeName(ChipType::C219) == "C219");
}

TEST_CASE("format: tabela komend zgodna z _CMD_INFO libvgm (wybrane wpisy)")
{
    using detail::kCmdTable;
    CHECK(kCmdTable[0x50].len == 2);
    CHECK(kCmdTable[0x52].len == 3);
    CHECK(kCmdTable[0x61].len == 3);
    CHECK(kCmdTable[0x62].len == 1);
    CHECK(kCmdTable[0x66].kind == 'E');
    CHECK(kCmdTable[0x67].kind == 'B');
    CHECK(kCmdTable[0x68].len == 12);
    CHECK(kCmdTable[0x70].len == 1);
    CHECK(kCmdTable[0x80].len == 1);
    CHECK(kCmdTable[0x90].len == 5);
    CHECK(kCmdTable[0x93].len == 11);
    CHECK(kCmdTable[0xA0].len == 3);
    CHECK(kCmdTable[0xC0].len == 4);
    CHECK(kCmdTable[0xE0].len == 5);
    CHECK(kCmdTable[0x01].kind == 'I');
    CHECK(kCmdTable[0x00].kind == 'U');
    CHECK(kCmdTable[0x33].kind == 'U');
    CHECK(kCmdTable[0x52].chip == 0x02);
    CHECK(kCmdTable[0xA0].chip == 0x12);
}

// ===========================================================================
// decompressIfGzip
// ===========================================================================

TEST_CASE("gzip: dane nie-gzip sa kopiowane bez zmian (wykrywanie po magic, nie po rozszerzeniu)")
{
    const Bytes in = {'V', 'g', 'm', ' ', 1, 2, 3};
    auto r = decompressIfGzip(in);
    REQUIRE(r.ok());
    CHECK_FALSE(r.value().wasGzip);
    CHECK(r.value().bytes == in);

    auto one = decompressIfGzip(Bytes{0x1F});     // pojedynczy 0x1F to nie magic
    REQUIRE(one.ok());
    CHECK_FALSE(one.value().wasGzip);

    auto empty = decompressIfGzip(Bytes{});
    REQUIRE(empty.ok());
    CHECK(empty.value().bytes.empty());
}

TEST_CASE("gzip: poprawny strumien jest rozpakowany")
{
    Bytes data(200000);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>((i * 7) ^ (i >> 8));
    auto r = decompressIfGzip(gzipStored(data));
    REQUIRE(r.ok());
    CHECK(r.value().wasGzip);
    CHECK(r.value().bytes == data);
}

TEST_CASE("gzip: uciety strumien -> DecompressionFailed")
{
    const Bytes data(1000, 0x42);
    const Bytes gz = gzipStored(data);
    for (std::size_t cut : {gz.size() - 1, gz.size() - 8, gz.size() - 9, std::size_t{20}, std::size_t{10}, std::size_t{2}}) {
        Bytes t(gz.begin(), gz.begin() + static_cast<std::ptrdiff_t>(cut));
        auto r = decompressIfGzip(t);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::DecompressionFailed);
    }
}

TEST_CASE("gzip: uszkodzone CRC i klamliwe ISIZE nie sa akceptowane ani nie powoduja duzej alokacji")
{
    Bytes gz = gzipStored(Bytes(100, 7));
    Bytes badCrc = gz;
    badCrc[badCrc.size() - 8] ^= 0xFF;
    auto r1 = decompressIfGzip(badCrc);
    REQUIRE_FALSE(r1.ok());
    CHECK(r1.error().code == ErrorCode::DecompressionFailed);

    Bytes badSize = gz;
    put32(badSize, badSize.size() - 4, 0xFFFFFFFFu);   // ISIZE ~4 GiB
    auto r2 = decompressIfGzip(badSize);
    REQUIRE_FALSE(r2.ok());
    CHECK(r2.error().code == ErrorCode::DecompressionFailed);
}

TEST_CASE("gzip: limit rozmiaru wyjscia (TooLarge)")
{
    Limits l;
    l.maxDecompressedBytes = 1000;
    auto r = decompressIfGzip(gzipStored(Bytes(5000, 1)), l);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::TooLarge);

    auto ok = decompressIfGzip(gzipStored(Bytes(1000, 1)), l);   // dokladnie na limicie
    CHECK(ok.ok());
}

TEST_CASE("gzip: bomba - limit wspolczynnika kompresji")
{
    const Bytes bomb = gzipZeros(3u << 20);
    REQUIRE(bomb.size() < 30000);
    Limits l;
    l.maxDecompressRatio = 50;
    auto r = decompressIfGzip(bomb, l);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::TooLarge);

    // Bez limitu wspolczynnika ten sam strumien jest poprawny (sprawdza tez wlasny generator).
    l.maxDecompressRatio = 0;
    auto ok = decompressIfGzip(bomb, l);
    REQUIRE(ok.ok());
    CHECK(ok.value().bytes.size() == (3u << 20));

    // Limit bezwzgledny.
    Limits abs;
    abs.maxDecompressedBytes = 1u << 20;
    abs.maxDecompressRatio = 0;
    auto r2 = decompressIfGzip(bomb, abs);
    REQUIRE_FALSE(r2.ok());
    CHECK(r2.error().code == ErrorCode::TooLarge);
}

TEST_CASE("gzip: gzip w gzip -> BadSignature; wejscie za duze -> TooLarge")
{
    const Bytes inner = gzipStored(Bytes(50, 3));
    auto r = decompressIfGzip(gzipStored(inner));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::BadSignature);

    Limits l;
    l.maxInputBytes = 10;
    auto big = decompressIfGzip(Bytes(11, 0), l);
    REQUIRE_FALSE(big.ok());
    CHECK(big.error().code == ErrorCode::TooLarge);
}

TEST_CASE("gzip: smieci po magic -> blad, nie wyjatek")
{
    Bytes junk = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    auto r = decompressIfGzip(junk);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::DecompressionFailed);
}

// ===========================================================================
// parseVgmHeader
// ===========================================================================

TEST_CASE("naglowek: poprawny plik 1.71 - pola, uklady, offsety")
{
    Opt o;
    o.clocks = {{0x2C, 7670454}, {0x74, 1789773}};
    o.loopAt = 2;
    auto file = makeVgm(kSimple, o);
    auto r = parseVgmHeader(file);
    REQUIRE(r.ok());
    const HeaderInfo& h = r.value();
    CHECK(h.versionBcd == 0x171);
    CHECK(h.fileSize == file.size());
    CHECK(h.dataOffset == 0x100);
    CHECK(h.dataEnd == file.size());
    CHECK(h.gd3Offset == 0);
    CHECK(h.hasLoop());
    CHECK(h.loopOffset == 0x102);
    CHECK(h.loopSamples == 100);
    CHECK(h.totalSamples == 1000);
    REQUIRE(h.chips.size() == 3);
    CHECK(h.chips[0].type == ChipType::SN76496);
    CHECK(h.chips[0].clockHz == 3579545);
    CHECK(h.chips[1].type == ChipType::YM2612);
    CHECK(h.chips[1].clockHz == 7670454);
    CHECK(h.chips[2].type == ChipType::AY8910);
    CHECK(h.warnings.empty());
}

TEST_CASE("naglowek: dual-chip bit 30 -> dwie instancje; zegar zamaskowany; zegar 2. ukladu z naglowka rozszerzonego")
{
    Opt o;
    o.snClock = 3579545 | 0x40000000u;
    auto r = parseVgmHeader(makeVgm(kSimple, o));
    REQUIRE(r.ok());
    REQUIRE(r.value().chips.size() == 2);
    CHECK(r.value().chips[0].instance == 0);
    CHECK(r.value().chips[1].instance == 1);
    CHECK(r.value().chips[1].clockHz == 3579545);

    // Naglowek rozszerzony tuz za 0x100 bajtami naglowka glownego (0xBC -> 0x100); dane za nim.
    Bytes full(0x100, 0);
    std::memcpy(full.data(), "Vgm ", 4);
    put32(full, 0x08, 0x171);
    put32(full, 0x0C, 3000000 | 0x40000000u);
    put32(full, 0xBC, 0x100 - 0xBC);
    Bytes ex;
    append32(ex, 12);                                   // [0]  dlugosc naglowka rozszerzonego
    append32(ex, 8);                                    // [4]  lista zegarow: (extra+4) + 8 = extra+12
    append32(ex, 10);                                   // [8]  lista glosnosci: (extra+8) + 10 = extra+18
    ex.push_back(1); ex.push_back(0x00); append32(ex, 1500000);   // extra+12: count, SN76496, 1.5 MHz
    ex.push_back(1); ex.push_back(0x00); ex.push_back(0x01);      // extra+18: count, SN76496, flagi=1 (instancja 1)
    ex.push_back(0x81); ex.push_back(0x00);                       //           wartosc 0x0081
    put32(full, 0x34, static_cast<std::uint32_t>(0x100 + ex.size()) - 0x34);
    full.insert(full.end(), ex.begin(), ex.end());
    full.insert(full.end(), kSimple.begin(), kSimple.end());
    put32(full, 0x04, static_cast<std::uint32_t>(full.size() - 4));

    auto rr = parseVgmHeader(full);
    REQUIRE(rr.ok());
    REQUIRE(rr.value().chips.size() == 2);
    CHECK(rr.value().chips[0].clockHz == 3000000);
    CHECK(rr.value().chips[1].clockHz == 1500000);
    CHECK(rr.value().chips[1].volume == 0x0081);
    CHECK(rr.value().chips[0].volume == 0);
    CHECK(rr.value().dataOffset == 0x100 + ex.size());
}

TEST_CASE("naglowek: rozmiar naglowka zalezy od dataOfs - pola poza nim to zera")
{
    // Wersja 1.51 z dataOfs = 0x40: pole 0x80 (GameBoy) jest poza naglowkiem -> ignorowane.
    Opt o;
    o.version = 0x151;
    o.hdrSize = 0x40;
    o.clocks = {};
    Bytes f = makeVgm(kSimple, o);
    // Wstaw "zegar" w obszar komend na offset 0x80 - to dane, nie naglowek.
    f.resize(0x100, 0);
    f[0x40] = 0x66;
    put32(f, 0x80, 4194304);
    put32(f, 0x04, static_cast<std::uint32_t>(f.size() - 4));
    auto r = parseVgmHeader(f);
    REQUIRE(r.ok());
    REQUIRE(r.value().chips.size() == 1);
    CHECK(r.value().chips[0].type == ChipType::SN76496);

    // Ten sam offset przy dataOfs = 0x100 jest naglowkiem.
    Opt big;
    big.clocks = {{0x80, 4194304}};
    auto r2 = parseVgmHeader(makeVgm(kSimple, big));
    REQUIRE(r2.ok());
    REQUIRE(r2.value().chips.size() == 2);
    CHECK(r2.value().chips[1].type == ChipType::GameBoyDMG);
}

TEST_CASE("naglowek: wersja 1.00 ma stale dataOfs 0x40 niezaleznie od pola 0x34")
{
    Opt o;
    o.version = 0x100;
    o.hdrSize = 0x40;
    Bytes f = makeVgm(kSimple, o);
    put32(f, 0x34, 0x1000);   // wersje < 1.50 ignoruja to pole
    auto r = parseVgmHeader(f);
    REQUIRE(r.ok());
    CHECK(r.value().dataOffset == 0x40);
}

TEST_CASE("naglowek: v1.01 - pole 'FM clock' przypisane do pierwszego uzytego ukladu FM")
{
    Opt o;
    o.version = 0x101;
    o.hdrSize = 0x40;
    o.snClock = 3579545;
    o.clocks = {{0x10, 7670454}};
    auto r = parseVgmHeader(makeVgm(Bytes{0x52, 0x2A, 0x00, 0x66}, o));
    REQUIRE(r.ok());
    REQUIRE(r.value().chips.size() == 2);
    CHECK(r.value().chips[1].type == ChipType::YM2612);
    CHECK(r.value().chips[1].clockHz == 7670454);

    auto r2 = parseVgmHeader(makeVgm(Bytes{0x51, 0x20, 0x00, 0x66}, o));
    REQUIRE(r2.ok());
    CHECK(r2.value().chips[1].type == ChipType::YM2413);

    auto r3 = parseVgmHeader(makeVgm(Bytes{0x54, 0x20, 0x00, 0x66}, o));
    REQUIRE(r3.ok());
    CHECK(r3.value().chips[1].type == ChipType::YM2151);
}

TEST_CASE("naglowek: C140 z typem 2 (bajt 0x96) to C219; T6W28 to jedna instancja")
{
    Opt o;
    o.clocks = {{0xA8, 12288000}};
    Bytes f = makeVgm(kSimple, o);
    f[0x96] = 0;
    auto r = parseVgmHeader(f);
    REQUIRE(r.ok());
    CHECK(r.value().chips.back().type == ChipType::C140);
    f[0x96] = 2;
    auto r2 = parseVgmHeader(f);
    REQUIRE(r2.ok());
    CHECK(r2.value().chips.back().type == ChipType::C219);

    Opt t;
    t.snClock = 3579545 | 0xC0000000u;   // T6W28 + dual
    auto r3 = parseVgmHeader(makeVgm(kSimple, t));
    REQUIRE(r3.ok());
    CHECK(r3.value().chips.size() == 1);
    CHECK((r3.value().chips[0].flags & 0x80000000u) != 0);
    CHECK(r3.value().chips[0].clockHz == 3579545);
}

TEST_CASE("naglowek: bledy krytyczne - rozmiar, sygnatura, wersja")
{
    CHECK(parseVgmHeader(Bytes{}).error().code == ErrorCode::Truncated);
    Bytes small(0x37, 0);
    CHECK(parseVgmHeader(small).error().code == ErrorCode::Truncated);

    Bytes f = makeVgm(kSimple);
    Bytes sig = f;
    sig[0] = 'X';
    CHECK(parseVgmHeader(sig).error().code == ErrorCode::BadSignature);

    for (std::uint32_t v : {0u, 0x99u, 0x300u, 0xFFFFFFFFu, 0x17Au}) {
        Bytes bv = f;
        put32(bv, 0x08, v);
        auto r = parseVgmHeader(bv);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::UnsupportedVersion);
    }

    Bytes newer = f;
    put32(newer, 0x08, 0x180);
    auto rn = parseVgmHeader(newer);
    REQUIRE(rn.ok());
    CHECK(hasWarning(rn.value().warnings, WarningCode::UnknownVersion));
}

TEST_CASE("naglowek: offsety liczone w UINT64 - bez zawiniecia UINT32")
{
    Bytes f = makeVgm(kSimple);
    // dataOfs: 0x34 + 0xFFFFFFF0 zawinelo by sie do 0x24 w UINT32 (-> 0x38); w UINT64 wychodzi poza plik.
    Bytes a = f;
    put32(a, 0x34, 0xFFFFFFF0u);
    auto ra = parseVgmHeader(a);
    REQUIRE_FALSE(ra.ok());
    CHECK(ra.error().code == ErrorCode::BadHeader);

    // dataOfs wskazuje na koniec pliku (brak danych).
    Bytes b = f;
    put32(b, 0x34, static_cast<std::uint32_t>(f.size() - 0x34));
    CHECK(parseVgmHeader(b).error().code == ErrorCode::BadHeader);

    // eofOfs 0xFFFFFFFF: 0x04 + rel zawinelo by sie do 3; w UINT64 > rozmiar -> korekta + ostrzezenie.
    Bytes c = f;
    put32(c, 0x04, 0xFFFFFFFFu);
    auto rc = parseVgmHeader(c);
    REQUIRE(rc.ok());
    CHECK(rc.value().dataEnd == f.size());
    CHECK(hasWarning(rc.value().warnings, WarningCode::EofOffsetMismatch));

    // eofOfs == 0 -> korekta.
    Bytes d = f;
    put32(d, 0x04, 0);
    auto rd = parseVgmHeader(d);
    REQUIRE(rd.ok());
    CHECK(hasWarning(rd.value().warnings, WarningCode::EofOffsetMismatch));

    // eofOfs przed danymi -> blad.
    Bytes e = f;
    put32(e, 0x04, 0x20);
    CHECK(parseVgmHeader(e).error().code == ErrorCode::BadHeader);

    // loopOfs 0xFFFFFFF0: zawiniecie dalo by 0x0C (<dataOfs), w UINT64 poza plikiem - tak czy inaczej ignorowany.
    Bytes g = f;
    put32(g, 0x1C, 0xFFFFFFF0u);
    put32(g, 0x20, 50);
    auto rg = parseVgmHeader(g);
    REQUIRE(rg.ok());
    CHECK_FALSE(rg.value().hasLoop());
    CHECK(rg.value().loopOffset == 0);

    // gd3Ofs 0xFFFFFFF0 -> ostrzezenie, brak GD3.
    Bytes h = f;
    put32(h, 0x14, 0xFFFFFFF0u);
    auto rh = parseVgmHeader(h);
    REQUIRE(rh.ok());
    CHECK(rh.value().gd3Offset == 0);
    CHECK(hasWarning(rh.value().warnings, WarningCode::Gd3Invalid));

    // naglowek rozszerzony poza plikiem -> ignorowany, bez crasha.
    Bytes x = f;
    put32(x, 0xBC, 0xFFFFFFF0u);
    auto rx = parseVgmHeader(x);
    REQUIRE(rx.ok());
    CHECK(hasWarning(rx.value().warnings, WarningCode::HeaderLengthMismatch));
}

TEST_CASE("naglowek: petla - musi lezec w [dataOfs, dataEnd) i miec niezerowa dlugosc")
{
    Bytes f = makeVgm(kSimple);
    // przed danymi
    Bytes a = f;
    put32(a, 0x1C, 0x40 - 0x1C);
    put32(a, 0x20, 10);
    CHECK_FALSE(parseVgmHeader(a).value().hasLoop());
    // dokladnie na dataEnd
    Bytes b = f;
    put32(b, 0x1C, static_cast<std::uint32_t>(f.size() - 0x1C));
    put32(b, 0x20, 10);
    CHECK_FALSE(parseVgmHeader(b).value().hasLoop());
    // dlugosc zero
    Bytes c = f;
    put32(c, 0x1C, 0x100 - 0x1C);
    put32(c, 0x20, 0);
    auto rc = parseVgmHeader(c);
    CHECK_FALSE(rc.value().hasLoop());
    CHECK(rc.value().loopOffset == 0);
    // poprawna, na pierwszym bajcie danych
    Bytes d = f;
    put32(d, 0x1C, 0x100 - 0x1C);
    put32(d, 0x20, 10);
    CHECK(parseVgmHeader(d).value().hasLoop());
}

TEST_CASE("naglowek: GD3 skraca dataEnd; GD3 poza plikiem jest odrzucany")
{
    Opt o;
    o.gd3 = makeGd3({u"Title"});
    Bytes f = makeVgm(kSimple, o);
    auto r = parseVgmHeader(f);
    REQUIRE(r.ok());
    CHECK(r.value().gd3Offset == 0x100 + kSimple.size());
    CHECK(r.value().dataEnd == r.value().gd3Offset);

    Bytes cut = f;
    put32(cut, 0x14, static_cast<std::uint32_t>(f.size() - 6 - 0x14));   // GD3 nie miesci sie (12 B naglowka)
    auto rc = parseVgmHeader(cut);
    REQUIRE(rc.ok());
    CHECK(rc.value().gd3Offset == 0);
    CHECK(hasWarning(rc.value().warnings, WarningCode::Gd3Invalid));
}

TEST_CASE("naglowek: odporne na dowolnie przyciete wejscie (bez crasha)")
{
    Opt o;
    o.gd3 = makeGd3({u"a", u"b"});
    o.loopAt = 2;
    const Bytes f = makeVgm(kSimple, o);
    for (std::size_t n = 0; n <= f.size(); ++n) {
        Bytes t(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(n));
        auto r = parseVgmHeader(t);
        if (r.ok()) {
            auto s = validateCommandStream(t, r.value());
            (void)s;
            auto g = parseGd3(t, r.value());
            (void)g;
        }
    }
    // pseudolosowe uszkodzenia naglowka
    std::uint32_t seed = 12345;
    for (int it = 0; it < 3000; ++it) {
        Bytes t = f;
        for (int k = 0; k < 4; ++k) {
            seed = seed * 1664525u + 1013904223u;
            t[(seed >> 8) % 0x100] = static_cast<std::uint8_t>(seed >> 24);
        }
        auto r = parseVgmHeader(t);
        if (r.ok()) {
            auto s = validateCommandStream(t, r.value());
            (void)s;
            auto g = parseGd3(t, r.value());
            (void)g;
        }
    }
}

// ===========================================================================
// parseGd3
// ===========================================================================

TEST_CASE("gd3: 11 pol, UTF-8, fallback jezyka")
{
    Opt o;
    o.gd3 = makeGd3({u"Track", u"トラック", u"Game", u"", u"System", u"", u"Author", u"", u"1990/01/02", u"Me", u"Notes"});
    const Bytes f = makeVgm(kSimple, o);
    auto h = parseVgmHeader(f);
    REQUIRE(h.ok());
    auto g = parseGd3(f, h.value());
    REQUIRE(g.ok());
    const Gd3Tag& t = g.value();
    CHECK(t.present);
    CHECK(t.trackEn == "Track");
    CHECK(t.trackJp == "\xE3\x83\x88\xE3\x83\xA9\xE3\x83\x83\xE3\x82\xAF");
    CHECK(t.gameEn == "Game");
    CHECK(t.releaseDate == "1990/01/02");
    CHECK(t.converter == "Me");
    CHECK(t.notes == "Notes");
    CHECK(t.track(Gd3Language::En) == "Track");
    CHECK(t.track(Gd3Language::Jp) == t.trackJp);
    CHECK(t.game(Gd3Language::Jp) == "Game");      // fallback na EN
    CHECK(t.system(Gd3Language::Jp) == "System");
    CHECK(t.author(Gd3Language::Jp) == "Author");
    CHECK_FALSE(t.sanitized);
    CHECK_FALSE(t.truncated);
}

TEST_CASE("gd3: brak GD3 nie jest bledem")
{
    const Bytes f = makeVgm(kSimple);
    auto h = parseVgmHeader(f);
    REQUIRE(h.ok());
    auto g = parseGd3(f, h.value());
    REQUIRE(g.ok());
    CHECK_FALSE(g.value().present);
}

TEST_CASE("gd3: surogaty - poprawna para, niesparowane -> U+FFFD")
{
    const std::u16string pair = {0xD83C, 0xDFB5};                 // U+1F3B5
    const std::u16string lonelyHigh = {u'a', 0xD800, u'b'};
    const std::u16string lonelyLow = {0xDC00, u'c'};
    const std::u16string highAtEnd = {u'd', 0xD800};
    Opt o;
    o.gd3 = makeGd3({pair, lonelyHigh, lonelyLow, highAtEnd});
    const Bytes f = makeVgm(kSimple, o);
    auto h = parseVgmHeader(f);
    REQUIRE(h.ok());
    auto g = parseGd3(f, h.value());
    REQUIRE(g.ok());
    CHECK(g.value().trackEn == "\xF0\x9F\x8E\xB5");
    CHECK(g.value().trackJp == "a\xEF\xBF\xBD" "b");
    CHECK(g.value().gameEn == "\xEF\xBF\xBD" "c");
    CHECK(g.value().gameJp == "d\xEF\xBF\xBD");
    CHECK(g.value().sanitized);
}

TEST_CASE("gd3: znaki sterujace usuniete, bialy znak -> spacja; limit dlugosci pola")
{
    Opt o;
    o.gd3 = makeGd3({std::u16string(u"a\x01" u"b\x7F" u"c\r\nd\te"), std::u16string(5000, u'x')});
    const Bytes f = makeVgm(kSimple, o);
    auto h = parseVgmHeader(f);
    REQUIRE(h.ok());
    auto g = parseGd3(f, h.value());
    REQUIRE(g.ok());
    CHECK(g.value().trackEn == "abc d e");
    CHECK(g.value().sanitized);
    CHECK(g.value().trackJp.size() == 4096);
    CHECK(g.value().truncated);

    Limits l;
    l.maxTagFieldBytes = 10;
    // przyciecie nie moze przeciac znaku wielobajtowego
    Opt m;
    m.gd3 = makeGd3({u"abcdefghié"});   // 9 ASCII + 2 B
    const Bytes f2 = makeVgm(kSimple, m);
    auto h2 = parseVgmHeader(f2);
    auto g2 = parseGd3(f2, h2.value(), l);
    REQUIRE(g2.ok());
    CHECK(g2.value().trackEn == "abcdefghi");
    CHECK(g2.value().truncated);
}

TEST_CASE("gd3: bledy - sygnatura, wersja, uciety naglowek; niepelne dane = puste pola")
{
    Opt o;
    o.gd3 = makeGd3({u"x"});
    const Bytes f = makeVgm(kSimple, o);
    auto h = parseVgmHeader(f);
    REQUIRE(h.ok());

    Bytes bad = f;
    bad[h.value().gd3Offset] = 'X';
    CHECK(parseGd3(bad, h.value()).error().code == ErrorCode::BadSignature);

    Opt v;
    v.gd3 = makeGd3({u"x"}, 0x200);
    const Bytes fv = makeVgm(kSimple, v);
    auto hv = parseVgmHeader(fv);
    REQUIRE(hv.ok());
    CHECK(parseGd3(fv, hv.value()).error().code == ErrorCode::UnsupportedVersion);

    HeaderInfo fake = h.value();
    fake.gd3Offset = f.size() - 5;
    CHECK(parseGd3(f, fake).error().code == ErrorCode::Truncated);

    // Zadeklarowana dlugosc wieksza od pliku jest przycinana; brak terminatora ostatniego pola OK.
    Opt t;
    t.gd3 = makeGd3({u"abc"});
    Bytes ft = makeVgm(kSimple, t);
    Bytes cutNoTerm(ft.begin(), ft.end() - 2);
    put32(cutNoTerm, 0x04, static_cast<std::uint32_t>(cutNoTerm.size() - 4));
    auto ht = parseVgmHeader(cutNoTerm);
    REQUIRE(ht.ok());
    auto gt = parseGd3(cutNoTerm, ht.value());
    REQUIRE(gt.ok());
    CHECK(gt.value().trackEn == "abc");
    CHECK(gt.value().trackJp.empty());
}

TEST_CASE("gd3: nieparzysta liczba bajtow i dlugosc 0")
{
    Opt o;
    Bytes g = {'G', 'd', '3', ' '};
    append32(g, 0x100);
    append32(g, 0);
    o.gd3 = g;
    const Bytes f = makeVgm(kSimple, o);
    auto h = parseVgmHeader(f);
    REQUIRE(h.ok());
    auto r = parseGd3(f, h.value());
    REQUIRE(r.ok());
    CHECK(r.value().present);
    CHECK(r.value().trackEn.empty());

    Bytes odd = {'G', 'd', '3', ' '};
    append32(odd, 0x100);
    append32(odd, 3);
    odd.push_back('a'); odd.push_back(0); odd.push_back('z');   // 3 bajty: 'a' + niepelny znak
    Opt p;
    p.gd3 = odd;
    const Bytes fo = makeVgm(kSimple, p);
    auto ho = parseVgmHeader(fo);
    REQUIRE(ho.ok());
    auto ro = parseGd3(fo, ho.value());
    REQUIRE(ro.ok());
    CHECK(ro.value().trackEn == "a");
}

// ===========================================================================
// validateCommandStream
// ===========================================================================

TEST_CASE("strumien: prawdziwa dlugosc z sumy opoznien, liczba komend, brak ostrzezen")
{
    // 0x62 (735) + 0x63 (882) + 0x61 0x10 0x00 (16) + 0x70 (1) + 0x7F (16) + 0x84 (4, YM2612 PCM) + 0x66
    Opt o;
    o.clocks = {{0x2C, 7670454}};
    auto s = scan(makeVgm(Bytes{0x62, 0x63, 0x61, 0x10, 0x00, 0x70, 0x7F, 0x84, 0x66}, o));
    REQUIRE(s.rep.ok());
    const StreamReport& r = s.rep.value();
    CHECK(r.totalSamples == 735 + 882 + 16 + 1 + 16 + 4);
    CHECK(r.commandCount == 7);
    CHECK(r.hasEndCommand);
    CHECK(r.warnings.empty());
    CHECK(r.pcmBytes == 0);
}

TEST_CASE("strumien: brak 0x66 -> ostrzezenie EndsEarly; dlugosc nadal policzona")
{
    auto s = scan(makeVgm(Bytes{0x50, 0x9F, 0x63}));
    REQUIRE(s.rep.ok());
    CHECK_FALSE(s.rep.value().hasEndCommand);
    CHECK(hasWarning(s.rep.value().warnings, WarningCode::EndsEarly));
    CHECK(s.rep.value().totalSamples == 882);
}

TEST_CASE("strumien: komendy po 0x66 nie wchodza do dlugosci ani nie sa skanowane")
{
    auto s = scan(makeVgm(Bytes{0x63, 0x66, 0x01, 0x01, 0x63}));
    REQUIRE(s.rep.ok());
    CHECK(s.rep.value().totalSamples == 882);
    CHECK(s.rep.value().commandCount == 2);
}

TEST_CASE("strumien: komenda niewazna (libvgm konczy) -> BadCommandStream; nieznana pomijalna -> ostrzezenie")
{
    auto bad = scan(makeVgm(Bytes{0x63, 0x01, 0x66}));
    REQUIRE_FALSE(bad.rep.ok());
    CHECK(bad.rep.error().code == ErrorCode::BadCommandStream);
    CHECK(bad.rep.error().byteOffset == 0x101);

    // 0x33 (len 2) i 0xE5 (len 5) sa "unknown" - pomijalne
    auto unk = scan(makeVgm(Bytes{0x33, 0x00, 0xE5, 1, 2, 3, 4, 0x33, 0x00, 0x66}));
    REQUIRE(unk.rep.ok());
    int n = 0;
    for (const auto& w : unk.rep.value().warnings) n += w.code == WarningCode::UnknownCommand;
    CHECK(n == 2);    // po jednym na identyfikator komendy
    CHECK(unk.rep.value().commandCount == 4);
}

TEST_CASE("strumien: operandy ostatniej komendy za dataEnd -> Truncated")
{
    for (const Bytes& cmds : {Bytes{0x50}, Bytes{0x61, 0x01}, Bytes{0xA0, 0x01}, Bytes{0xE0, 1, 2, 3}, Bytes{0x93, 1, 2, 3, 4, 5, 6, 7, 8, 9}}) {
        auto s = scan(makeVgm(cmds));
        REQUIRE_FALSE(s.rep.ok());
        CHECK(s.rep.error().code == ErrorCode::Truncated);
    }
}

TEST_CASE("strumien: uklad bez zegara w naglowku -> ostrzezenie ChipNotInHeader (raz na uklad)")
{
    auto s = scan(makeVgm(Bytes{0x52, 1, 2, 0x53, 3, 4, 0xA2, 5, 6, 0x66}));   // YM2612 brak; 0xA2 = 2. instancja
    REQUIRE(s.rep.ok());
    int n = 0;
    for (const auto& w : s.rep.value().warnings) n += w.code == WarningCode::ChipNotInHeader;
    CHECK(n == 2);   // YM2612 instancja 0 i instancja 1

    // 2. SN76496 bez dual-chip
    auto s2 = scan(makeVgm(Bytes{0x30, 0x00, 0x66}));
    REQUIRE(s2.rep.ok());
    CHECK(hasWarning(s2.rep.value().warnings, WarningCode::ChipNotInHeader));

    // z dual-chip - bez ostrzezenia
    Opt o;
    o.snClock = 3579545 | 0x40000000u;
    auto s3 = scan(makeVgm(Bytes{0x30, 0x00, 0x50, 0x00, 0x66}, o));
    REQUIRE(s3.rep.ok());
    CHECK(s3.rep.value().warnings.empty());
}

TEST_CASE("strumien: petla - pozycja w tickach, granica komendy, ograniczenia")
{
    Opt o;
    o.loopAt = 2;   // po 0x63 + 0x62? cmds: [0]=0x63 [1]=0x50 [2]=0x00 -> misaligned? sprawdzamy ponizej
    // cmds: 0x63 (1 B), 0x61 10 00 (3 B), 0x62, 0x66  -> granice: 0,1,4,5
    const Bytes cmds = {0x63, 0x61, 0x10, 0x00, 0x62, 0x66};
    o.loopAt = 1;
    auto ok = scan(makeVgm(cmds, o));
    REQUIRE(ok.rep.ok());
    CHECK(ok.rep.value().loopStartSample == 882);
    CHECK(ok.rep.value().totalSamples == 882 + 16 + 735);

    o.loopAt = 2;   // w srodku operandu 0x61
    auto mis = scan(makeVgm(cmds, o));
    REQUIRE_FALSE(mis.rep.ok());
    CHECK(mis.rep.error().code == ErrorCode::BadHeader);

    o.loopAt = 5;   // na samym 0x66
    auto onEnd = scan(makeVgm(cmds, o));
    REQUIRE(onEnd.rep.ok());
    CHECK(onEnd.rep.value().loopStartSample == onEnd.rep.value().totalSamples);

    // petla za pierwszym 0x66 (dane po koncu)
    o.loopAt = 7;
    const Bytes after = {0x63, 0x66, 0x63, 0x63, 0x66};
    Opt p;
    p.loopAt = 3;
    auto beyond = scan(makeVgm(after, p));
    REQUIRE_FALSE(beyond.rep.ok());
    CHECK(beyond.rep.error().code == ErrorCode::BadHeader);
}

TEST_CASE("strumien: limity - liczba komend i dlugosc utworu")
{
    Limits l;
    l.maxCommands = 3;
    auto s = scan(makeVgm(Bytes{0x63, 0x63, 0x63, 0x63, 0x66}), l);
    REQUIRE_FALSE(s.rep.ok());
    CHECK(s.rep.error().code == ErrorCode::LimitExceeded);

    Limits t;
    t.maxTotalSamples = 1000;
    auto s2 = scan(makeVgm(Bytes{0x63, 0x63, 0x66}), t);
    REQUIRE_FALSE(s2.rep.ok());
    CHECK(s2.rep.error().code == ErrorCode::LimitExceeded);
}

TEST_CASE("strumien: bloki danych - poprawny blok PCM, sumy bankow")
{
    Opt o;
    o.clocks = {{0x2C, 7670454}};
    auto s = scan(makeVgm(cat({block(0x00, Bytes(100, 1)), block(0x00, Bytes(50, 2)), block(0x01, Bytes(10, 3)), Bytes{0x66}}), o));
    REQUIRE(s.rep.ok());
    CHECK(s.rep.value().pcmBytes == 160);
    CHECK(s.rep.value().commandCount == 4);
    CHECK(s.rep.value().warnings.empty());
}

TEST_CASE("strumien: blok 0x67 dluzszy niz dane -> BadDataBlock; uciety naglowek -> Truncated")
{
    Bytes b = block(0x00, Bytes(10, 0));
    put32(b, 3, 0x7FFFFFF0u);   // dlugosc ~2 GiB
    auto s = scan(makeVgm(cat({b, Bytes{0x66}})));
    REQUIRE_FALSE(s.rep.ok());
    CHECK(s.rep.error().code == ErrorCode::BadDataBlock);

    auto t = scan(makeVgm(Bytes{0x67, 0x66, 0x00}));
    REQUIRE_FALSE(t.rep.ok());
    CHECK(t.rep.error().code == ErrorCode::Truncated);

    // dlugosc o 1 wieksza niz zostalo
    Bytes c = block(0x00, Bytes(10, 0));
    put32(c, 3, 11);
    auto u = scan(makeVgm(c));
    REQUIRE_FALSE(u.rep.ok());
    CHECK(u.rep.error().code == ErrorCode::BadDataBlock);
}

TEST_CASE("strumien: budzety pamieci - blok, suma PCM, ROM")
{
    Limits l;
    l.maxSingleBlockBytes = 100;
    l.maxPcmBudgetBytes = 150;
    auto big = scan(makeVgm(cat({block(0x00, Bytes(101, 0)), Bytes{0x66}})), l);
    REQUIRE_FALSE(big.rep.ok());
    CHECK(big.rep.error().code == ErrorCode::LimitExceeded);

    auto sum = scan(makeVgm(cat({block(0x00, Bytes(100, 0)), block(0x01, Bytes(51, 0)), Bytes{0x66}})), l);
    REQUIRE_FALSE(sum.rep.ok());
    CHECK(sum.rep.error().code == ErrorCode::LimitExceeded);

    auto fits = scan(makeVgm(cat({block(0x00, Bytes(100, 0)), block(0x01, Bytes(50, 0)), Bytes{0x66}})), l);
    CHECK(fits.rep.ok());

    // ROM: memSize 4 GiB-1 przekracza limit pojedynczego bloku, nawet gdy dane sa male.
    Bytes rom;
    append32(rom, 0xFFFFFFFFu);
    append32(rom, 0);
    rom.push_back(1);
    auto r = scan(makeVgm(cat({block(0x8B, rom), Bytes{0x66}})));
    REQUIRE_FALSE(r.rep.ok());
    CHECK(r.rep.error().code == ErrorCode::LimitExceeded);
}

TEST_CASE("strumien: zapis ROM - memSize liczony raz na (typ, uklad); dane musza miescic sie w ROM")
{
    auto romBlock = [](std::uint32_t mem, std::uint32_t ofs, std::size_t n, bool chip2 = false) {
        Bytes p;
        append32(p, mem);
        append32(p, ofs);
        p.insert(p.end(), n, 0x55);
        return block(0x8B, p, chip2);
    };
    Opt o;
    o.clocks = {{0x98, 1000000}};
    auto s = scan(makeVgm(cat({romBlock(1000, 0, 100), romBlock(1000, 100, 100), romBlock(1000, 200, 100, true), Bytes{0x66}}), o));
    REQUIRE(s.rep.ok());
    CHECK(s.rep.value().romWriteBytes == 2000);   // 1000 dla ukladu 0 + 1000 dla ukladu 1

    auto over = scan(makeVgm(cat({romBlock(1000, 950, 100), Bytes{0x66}}), o));
    REQUIRE_FALSE(over.rep.ok());
    CHECK(over.rep.error().code == ErrorCode::BadDataBlock);

    auto wrap = scan(makeVgm(cat({romBlock(1000, 0xFFFFFFF0u, 100), Bytes{0x66}}), o));
    REQUIRE_FALSE(wrap.rep.ok());   // dataOfs + len w UINT64 (bez zawiniecia)
    CHECK(wrap.rep.error().code == ErrorCode::BadDataBlock);

    auto shortBlk = scan(makeVgm(cat({block(0x8B, Bytes(7, 0)), Bytes{0x66}}), o));
    REQUIRE_FALSE(shortBlk.rep.ok());
    CHECK(shortBlk.rep.error().code == ErrorCode::BadDataBlock);
}

TEST_CASE("strumien: zapis RAM - minimalna dlugosc naglowka adresu")
{
    CHECK(scan(makeVgm(cat({block(0xC0, Bytes{1, 2}), Bytes{0x66}}))).rep.ok());
    CHECK(scan(makeVgm(cat({block(0xE0, Bytes{1, 2, 3, 4, 9}), Bytes{0x66}}))).rep.ok());
    auto a = scan(makeVgm(cat({block(0xC0, Bytes{1}), Bytes{0x66}})));
    REQUIRE_FALSE(a.rep.ok());
    CHECK(a.rep.error().code == ErrorCode::BadDataBlock);
    auto b = scan(makeVgm(cat({block(0xE0, Bytes{1, 2, 3}), Bytes{0x66}})));
    REQUIRE_FALSE(b.rep.ok());
    CHECK(b.rep.error().code == ErrorCode::BadDataBlock);
}

TEST_CASE("strumien: bloki skompresowane - walidacja naglowka (H3)")
{
    const Bytes data(8, 0xAA);
    auto run = [&](const Bytes& payload) {
        return scan(makeVgm(cat({block(0x40, payload), Bytes{0x66}})));
    };
    // poprawny bit-packing 8 -> 4, kopiowanie
    CHECK(run(comprPayload(0, 16, 8, 4, 0, data)).rep.ok());
    // bitsCmp == 0 (dzielenie przez zero w libvgm)
    CHECK(run(comprPayload(0, 16, 8, 0, 0, data)).rep.error().code == ErrorCode::BadDataBlock);
    // bitsDec == 0, bitsCmp > bitsDec, bitsCmp > 16, bitsDec > 16
    CHECK(run(comprPayload(0, 16, 0, 4, 0, data)).rep.error().code == ErrorCode::BadDataBlock);
    CHECK(run(comprPayload(0, 16, 4, 8, 0, data)).rep.error().code == ErrorCode::BadDataBlock);
    CHECK(run(comprPayload(0, 16, 20, 17, 0, data)).rep.error().code == ErrorCode::BadDataBlock);
    // nieznany typ kompresji
    CHECK(run(comprPayload(2, 16, 8, 4, 0, data)).rep.error().code == ErrorCode::BadDataBlock);
    // za krotki naglowek (< 10 B)
    CHECK(run(Bytes{0, 1, 0, 0, 0, 8, 4, 0, 0}).rep.error().code == ErrorCode::BadDataBlock);
    CHECK(run(Bytes{0, 1, 0, 0}).rep.error().code == ErrorCode::BadDataBlock);
    // nieparzysta dlugosc wyjscia dla wartosci 16-bitowych
    CHECK(run(comprPayload(0, 15, 12, 8, 0, data)).rep.error().code == ErrorCode::BadDataBlock);
    CHECK(run(comprPayload(0, 16, 12, 8, 0, data)).rep.ok());
    // pusty wynik z niepustym payloadem (libvgm wykonalby payload jako komendy)
    CHECK(run(comprPayload(0, 0, 8, 4, 0, data)).rep.error().code == ErrorCode::BadDataBlock);
    CHECK(run(comprPayload(0, 0, 8, 4, 0, Bytes{})).rep.ok());
    // decmpLen ponad limit bloku
    Limits l;
    l.maxSingleBlockBytes = 10;
    CHECK(scan(makeVgm(cat({block(0x40, comprPayload(0, 16, 8, 4, 0, data)), Bytes{0x66}})), l).rep.error().code ==
          ErrorCode::LimitExceeded);
    // bomba: maly blok, ogromny decmpLen -> budzet
    CHECK(run(comprPayload(0, 0xFFFFFFF0u, 8, 4, 0, data)).rep.error().code == ErrorCode::LimitExceeded);
}

TEST_CASE("strumien: kompresja tabelowa wymaga zgodnej tabeli 0x7F o >= 2^bitsCmp wpisach (H3)")
{
    auto table = [](std::uint8_t bitsDec, std::uint8_t bitsCmp, std::uint16_t count, std::size_t valBytes) {
        Bytes p = {0, 0, bitsDec, bitsCmp, static_cast<std::uint8_t>(count & 0xFF), static_cast<std::uint8_t>(count >> 8)};
        p.insert(p.end(), valBytes, 0x11);
        return block(0x7F, p);
    };
    const Bytes data(8, 0xAA);
    const Bytes tblComp = block(0x40, comprPayload(0, 16, 8, 4, 2, data));       // subType 2 = tabela
    const Bytes dpcm = block(0x40, comprPayload(1, 16, 8, 4, 0, data));          // DPCM zawsze z tabela

    // bez tabeli
    auto none = scan(makeVgm(cat({tblComp, Bytes{0x66}})));
    REQUIRE_FALSE(none.rep.ok());
    CHECK(none.rep.error().code == ErrorCode::BadDataBlock);

    // zgodna tabela: 16 wpisow po 1 B
    CHECK(scan(makeVgm(cat({table(8, 4, 16, 16), tblComp, dpcm, Bytes{0x66}}))).rep.ok());
    // za malo wpisow (15 < 2^4) -> odczyt poza tabela
    CHECK(scan(makeVgm(cat({table(8, 4, 15, 15), tblComp, Bytes{0x66}}))).rep.error().code == ErrorCode::BadDataBlock);
    // niezgodne bitsCmp
    CHECK(scan(makeVgm(cat({table(8, 5, 32, 32), tblComp, Bytes{0x66}}))).rep.error().code == ErrorCode::BadDataBlock);
    // tabela dluzsza niz blok
    Bytes tooLong = table(8, 4, 16, 10);
    CHECK(scan(makeVgm(cat({tooLong, Bytes{0x66}}))).rep.error().code == ErrorCode::BadDataBlock);
    // tabela krotsza niz 6 B, bitsDec == 0
    CHECK(scan(makeVgm(cat({block(0x7F, Bytes{0, 0, 8}), Bytes{0x66}}))).rep.error().code == ErrorCode::BadDataBlock);
    CHECK(scan(makeVgm(cat({table(0, 4, 16, 0), Bytes{0x66}}))).rep.error().code == ErrorCode::BadDataBlock);
    // tabela 16-bitowa: 2 B na wpis
    CHECK(scan(makeVgm(cat({table(16, 4, 16, 32), Bytes{0x66}}))).rep.ok());
    CHECK(scan(makeVgm(cat({table(16, 4, 16, 31), Bytes{0x66}}))).rep.error().code == ErrorCode::BadDataBlock);
}

TEST_CASE("strumien: blok PCM po 0x91 tego samego banku odrzucony (wiszacy wskaznik DAC, H3)")
{
    const Bytes setup = {0x90, 0x00, 0x02, 0x00, 0x2A};        // strumien 0
    const Bytes setData = {0x91, 0x00, 0x00, 0x01, 0x00};      // strumien 0 -> bank 0
    Opt o;
    o.clocks = {{0x2C, 7670454}};

    auto bad = scan(makeVgm(cat({block(0x00, Bytes(8, 1)), setup, setData, block(0x00, Bytes(4, 2)), Bytes{0x66}}), o));
    REQUIRE_FALSE(bad.rep.ok());
    CHECK(bad.rep.error().code == ErrorCode::BadDataBlock);

    // inny bank - bezpieczny
    CHECK(scan(makeVgm(cat({block(0x00, Bytes(8, 1)), setup, setData, block(0x01, Bytes(4, 2)), Bytes{0x66}}), o)).rep.ok());
    // 0x91 na pustym banku (wskaznik NULL) - bezpieczny
    CHECK(scan(makeVgm(cat({setup, setData, block(0x00, Bytes(4, 2)), Bytes{0x66}}), o)).rep.ok());
    // 0x91 bez wczesniejszego 0x90 - libvgm ignoruje
    CHECK(scan(makeVgm(cat({block(0x00, Bytes(8, 1)), setData, block(0x00, Bytes(4, 2)), Bytes{0x66}}), o)).rep.ok());
}

TEST_CASE("strumien: blok PCM w/za loopOfs wylacza petle (plik gra raz, ostrzezenie)")
{
    // cmds: 0x63 (1 B), blok (7 + 3 B), 0x66
    const Bytes blk = block(0x00, Bytes{1, 2, 3});
    Opt after;
    after.loopAt = 1;   // dokladnie na bloku
    auto a = scan(makeVgm(cat({Bytes{0x63}, blk, Bytes{0x66}}), after));
    REQUIRE(a.rep.ok());
    CHECK(a.rep.value().loopDisabled);
    const auto& aw = a.rep.value().warnings;
    CHECK(std::any_of(aw.begin(), aw.end(), [](const Warning& w) { return w.code == WarningCode::BlockAfterLoop; }));

    Opt before;
    before.loopAt = 1 + static_cast<std::int64_t>(blk.size());   // petla po bloku
    auto b = scan(makeVgm(cat({Bytes{0x63}, blk, Bytes{0x63, 0x66}}), before));
    REQUIRE(b.rep.ok());
    CHECK_FALSE(b.rep.value().loopDisabled);

    // blok ROM za petla nie jest pomijany przez libvgm - dozwolony
    Bytes rom;
    append32(rom, 100);
    append32(rom, 0);
    Opt rm;
    rm.loopAt = 0;
    rm.clocks = {{0x98, 1000000}};
    CHECK(scan(makeVgm(cat({block(0x8B, rom), Bytes{0x63, 0x66}}), rm)).rep.ok());
}

TEST_CASE("strumien: niespojne wywolanie -> InvalidArgument")
{
    const Bytes f = makeVgm(kSimple);
    auto h = parseVgmHeader(f);
    REQUIRE(h.ok());
    Bytes shorter(f.begin(), f.end() - 1);
    CHECK(validateCommandStream(shorter, h.value()).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("strumien: odporne na losowe bajty komend (bez crasha)")
{
    std::uint32_t seed = 987654321;
    for (int it = 0; it < 4000; ++it) {
        Bytes cmds;
        const int n = 1 + static_cast<int>((seed >> 5) % 64);
        for (int i = 0; i < n; ++i) {
            seed = seed * 1664525u + 1013904223u;
            cmds.push_back(static_cast<std::uint8_t>(seed >> 24));
        }
        if (it % 3 == 0) {   // wstrzykuj naglowek bloku z losowymi parametrami
            const Bytes b = {0x67, 0x66, static_cast<std::uint8_t>(seed >> 8), static_cast<std::uint8_t>(seed),
                             static_cast<std::uint8_t>(seed >> 16), 0, 0};
            cmds.insert(cmds.begin() + static_cast<std::ptrdiff_t>(seed % cmds.size()), b.begin(), b.end());
        }
        const Bytes f = makeVgm(cmds);
        auto h = parseVgmHeader(f);
        REQUIRE(h.ok());
        auto s = validateCommandStream(f, h.value());
        (void)s;
    }
}

// ===========================================================================
// Poprawki z recenzji bezpieczenstwa (ADR 0002 pkt 8.15-8.18, libvgm-parser-hazards.md H6-H9)
// ===========================================================================

namespace {

Result<HeaderInfo> hdrWith(const std::vector<std::pair<std::size_t, std::uint32_t>>& clocks,
                           const std::vector<std::pair<std::size_t, std::uint8_t>>& bytes = {},
                           const Limits& l = {})
{
    Opt o;
    o.clocks = clocks;
    Bytes f = makeVgm(kSimple, o);
    for (auto& [ofs, v] : bytes) f[ofs] = v;
    return parseVgmHeader(f, l);
}

} // namespace

TEST_CASE("naglowek: C352 z dzielnikiem 0xD6 == 0 odrzucony (libvgm dzieli przez bajt, #DE)")
{
    // C352 (indeks 0x27): zegar w 0xDC, dzielnik w 0xD6.
    auto bad = hdrWith({{0xDC, 24576000}}, {{0xD6, 0}});
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::BadHeader);
    CHECK(bad.error().byteOffset == 0xD6);

    auto ok = hdrWith({{0xDC, 24576000}}, {{0xD6, 72}});
    REQUIRE(ok.ok());
    REQUIRE(ok.value().chips.size() == 2);   // SN76496 + C352
    CHECK(ok.value().chips[1].type == ChipType::C352);
    // bez zegara C352 bajt 0xD6 nie ma znaczenia
    CHECK(hdrWith({}, {{0xD6, 0}}).ok());
}

TEST_CASE("naglowek: zegar po przeliczeniach libvgm (UINT32 z zawinieciem) jest sprawdzany, nie tylko surowy")
{
    // C352: clock * 72 / 1 zawija sie do ~2,9e9 (surowe 1e8 < limit 4e8).
    auto c352 = hdrWith({{0xDC, 100000000}}, {{0xD6, 1}});
    REQUIRE_FALSE(c352.ok());
    CHECK(c352.error().code == ErrorCode::LimitExceeded);

    // C352: 2^29 * 72 = 9 * 2^32 -> 0 po zawinieciu -> libvgm dostalby zegar 0.
    Limits wide;
    wide.maxChipClockHz = 0xFFFFFFFFu;
    auto zero = hdrWith({{0xDC, 1u << 29}}, {{0xD6, 72}}, wide);
    REQUIRE_FALSE(zero.ok());
    CHECK(zero.error().code == ErrorCode::BadHeader);

    // MultiPCM (0x88): clock * 224 / 180; 2^27 * 224 = 7 * 2^32 -> 0.
    auto mpcm = hdrWith({{0x88, 1u << 27}});
    REQUIRE_FALSE(mpcm.ok());
    CHECK(mpcm.error().code == ErrorCode::BadHeader);
    CHECK(hdrWith({{0x88, 10000000}}).ok());

    // QSound (0xB4): < 5 MHz to stary zegar 4 MHz (libvgm mnozy x15); 60 MHz bezposrednio tez w limicie.
    CHECK(hdrWith({{0xB4, 4000000}}).ok());
    CHECK(hdrWith({{0xB4, 60000000}}).ok());
}

TEST_CASE("naglowek: limity zegara ukladu (maxChipClockHz) i natywnej czestotliwosci probek")
{
    // POKEY (0xB0): rate = zegar.
    REQUIRE(hdrWith({{0xB0, 1789772}}).ok());
    CHECK(hdrWith({{0xB0, 20000000}}).ok());                                      // rate == limit
    auto fast = hdrWith({{0xB0, 20000001}});
    REQUIRE_FALSE(fast.ok());
    CHECK(fast.error().code == ErrorCode::LimitExceeded);

    // zegar ponad globalny limit (YMF278B, 0x60: dzielnik 768)
    auto huge = hdrWith({{0x60, 500000000}});
    REQUIRE_FALSE(huge.ok());
    CHECK(huge.error().code == ErrorCode::LimitExceeded);
    CHECK(huge.error().byteOffset == 0x60);
    // 400 MHz dokladnie (limit wlacznie) przy duzym dzielniku przechodzi
    CHECK(hdrWith({{0x60, 400000000}}).ok());
    // bity 30/31 nie wchodza do zegara
    CHECK(hdrWith({{0xB0, 1789772 | 0x80000000u}}).ok());

    // limity sa konfigurowalne; suma po instancjach
    Limits tight;
    tight.maxTotalChipSampleRate = 4'000'000;
    CHECK(hdrWith({}, {}, tight).ok());                                            // SN76496: 1,79 MHz
    auto sum = hdrWith({{0xB0, 3000000}}, {}, tight);                              // + POKEY 3 MHz = 4,79 MHz
    REQUIRE_FALSE(sum.ok());
    CHECK(sum.error().code == ErrorCode::LimitExceeded);
    Limits clk;
    clk.maxChipClockHz = 1'000'000;
    CHECK_FALSE(hdrWith({}, {}, clk).ok());                                        // SN76496 3,58 MHz > 1 MHz
}

TEST_CASE("naglowek: bajty konfiguracji powodujace przesuniecia o >= 32 bity (SN76496, SegaPCM) odrzucone")
{
    // SN76496: szerokosc rejestru przesuwnego 0x2A (>32 -> 1 << (width-1) w rdzeniu).
    CHECK(hdrWith({}, {{0x2A, 32}}).ok());
    auto sn = hdrWith({}, {{0x2A, 33}});
    REQUIRE_FALSE(sn.ok());
    CHECK(sn.error().code == ErrorCode::BadHeader);
    CHECK(sn.error().byteOffset == 0x2A);
    // bez ukladu bajt jest nieistotny
    Opt noSn;
    noSn.snClock = 0;
    Bytes f = makeVgm(kSimple, noSn);
    f[0x2A] = 200;
    CHECK(parseVgmHeader(f).ok());

    // SegaPCM (zegar 0x38, przesuniecie banku 0x3C)
    CHECK(hdrWith({{0x38, 4000000}}, {{0x3C, 31}}).ok());
    auto sp = hdrWith({{0x38, 4000000}}, {{0x3C, 32}});
    REQUIRE_FALSE(sp.ok());
    CHECK(sp.error().code == ErrorCode::BadHeader);
    CHECK(sp.error().byteOffset == 0x3C);
}

TEST_CASE("naglowek: instancja ukladu z zerowym zegarem (same bity flag) odrzucona - libvgm by ja uruchomil")
{
    Opt o;
    o.snClock = 0x40000000u;   // dual, zegar 0
    auto r = parseVgmHeader(makeVgm(kSimple, o));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::BadHeader);
    Opt o2;
    o2.snClock = 0x80000000u;  // tylko bit 31
    CHECK_FALSE(parseVgmHeader(makeVgm(kSimple, o2)).ok());
}

TEST_CASE("naglowek: eofOffset - eof uzyty przez parser (0x04 + rel albo rozmiar pliku)")
{
    const Bytes f = makeVgm(kSimple);
    auto ok = parseVgmHeader(f);
    REQUIRE(ok.ok());
    CHECK(ok.value().eofOffset == f.size());

    Bytes zero = f;
    put32(zero, 0x04, 0);
    CHECK(parseVgmHeader(zero).value().eofOffset == f.size());

    Bytes pad = f;
    put32(pad, 0x04, static_cast<std::uint32_t>(f.size() + 8 - 4));   // w padding libvgm
    auto rp = parseVgmHeader(pad);
    REQUIRE(rp.ok());
    CHECK(rp.value().eofOffset == f.size());
    CHECK(hasWarning(rp.value().warnings, WarningCode::EofOffsetMismatch));

    Bytes shorter = f;
    put32(shorter, 0x04, static_cast<std::uint32_t>(f.size() - 2 - 4));   // eof w srodku pliku
    auto rs = parseVgmHeader(shorter);
    REQUIRE(rs.ok());
    CHECK(rs.value().eofOffset == f.size() - 2);
    CHECK(rs.value().dataEnd == f.size() - 2);
}

TEST_CASE("gzip: domyslny wspolczynnik 2000:1 (deflate daje <= ~1032:1), limit twardy rozmiaru zostaje")
{
    CHECK(Limits{}.maxDecompressRatio == 2000);
    // 3 MiB zer -> wejscie ~20 KB (~160:1): sprawdzamy domyslna wartosc, limit twardy i zaostrzenie wspolczynnika.
    const Bytes gz = gzipZeros(3u << 20);
    auto r = decompressIfGzip(gz);
    REQUIRE(r.ok());
    CHECK(r.value().bytes.size() == (3u << 20));
    // twardy limit rozmiaru dalej dziala
    Limits small;
    small.maxDecompressedBytes = 1u << 20;
    auto t = decompressIfGzip(gz, small);
    REQUIRE_FALSE(t.ok());
    CHECK(t.error().code == ErrorCode::TooLarge);
    // wspolczynnik nadal mozna zaostrzyc
    Limits strict;
    strict.maxDecompressRatio = 10;
    auto s = decompressIfGzip(gz, strict);
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().code == ErrorCode::TooLarge);
}

TEST_CASE("GD3: znaki kierunkowe i niewidoczne Unicode sa usuwane (sanitized)")
{
    auto field = [](const std::u16string& title, bool& sanitized) {
        Opt o;
        o.gd3 = makeGd3({title});
        const Bytes f = makeVgm(kSimple, o);
        auto h = parseVgmHeader(f);
        REQUIRE(h.ok());
        auto g = parseGd3(f, h.value());
        REQUIRE(g.ok());
        sanitized = g.value().sanitized;
        return g.value().trackEn;
    };
    bool san = false;
    CHECK(field(u"a‮b​c‏d", san) == "abcd");   // RLO, ZWSP, RLM
    CHECK(san);
    CHECK(field(u"x⁦y⁩z‪w‬v‎", san) == "xyzwv");   // LRI, PDI, LRE, PDF, LRM
    CHECK(san);
    // sasiednie punkty kodowe poza przedzialami pozostaja: U+200A, U+2010, U+2065, U+206A
    CHECK(field(u" |‐|⁥|⁪", san) == "\xE2\x80\x8A|\xE2\x80\x90|\xE2\x81\xA5|\xE2\x81\xAA");
    CHECK_FALSE(san);
    CHECK(field(u"zwykly tytul", san) == "zwykly tytul");
    CHECK_FALSE(san);
}

TEST_CASE("strumien: pusty blok PCM w/za loopOfs nie wylacza petli (libvgm przeskakuje sam naglowek)")
{
    Opt o;
    o.loopAt = 1;
    auto r = scan(makeVgm(cat({Bytes{0x63}, block(0x00, Bytes{}), Bytes{0x63, 0x66}}), o));
    REQUIRE(r.rep.ok());
    CHECK_FALSE(r.rep.value().loopDisabled);
    CHECK_FALSE(hasWarning(r.rep.value().warnings, WarningCode::BlockAfterLoop));
    // niepusty - jak dotad
    auto n = scan(makeVgm(cat({Bytes{0x63}, block(0x00, Bytes{1}), Bytes{0x63, 0x66}}), o));
    REQUIRE(n.rep.ok());
    CHECK(n.rep.value().loopDisabled);
}

// --- Strumienie DAC (0x90..0x95): dzielenie przez zero w emu/dac_control.c ---

namespace {

Bytes dacSetup(std::uint8_t sid, std::uint8_t chip, std::uint16_t cmd)
{
    return {0x90, sid, chip, static_cast<std::uint8_t>(cmd >> 8), static_cast<std::uint8_t>(cmd & 0xFF)};
}
Bytes dacData(std::uint8_t sid, std::uint8_t bank, std::uint8_t step = 1, std::uint8_t base = 0)
{
    return {0x91, sid, bank, step, base};
}
Bytes dacFreq(std::uint8_t sid, std::uint32_t hz)
{
    Bytes b = {0x92, sid};
    append32(b, hz);
    return b;
}
Bytes dacPlay(std::uint8_t sid, std::uint8_t mode, std::uint32_t len = 100)
{
    Bytes b = {0x93, sid, 0, 0, 0, 0, mode};
    append32(b, len);
    return b;
}
Bytes dacPlayBlk(std::uint8_t sid, std::uint16_t id)
{
    return {0x95, sid, static_cast<std::uint8_t>(id & 0xFF), static_cast<std::uint8_t>(id >> 8), 0};
}

constexpr std::uint8_t kYM2612 = 0x02;
constexpr std::uint8_t kPWM = 0x11;
constexpr std::uint8_t kModeCmds = 0x01, kModeMsec = 0x02, kModeToEnd = 0x03, kModeBytes = 0x0F;

Opt dacOpt()
{
    Opt o;
    o.clocks = {{0x2C, 7670454}, {0x70, 23011361}};   // YM2612 + 32X PWM
    return o;
}

Scan dacScan(std::initializer_list<Bytes> cmds, const Opt& o = dacOpt())
{
    return scan(makeVgm(cat(cmds), o));
}

} // namespace

TEST_CASE("DAC: 0x93 w trybie MSEC przy czestotliwosci 0 odrzucone (1000 * len / Frequency)")
{
    const Bytes end = {0x66};
    // bez 0x92
    auto a = dacScan({dacSetup(0, kYM2612, 0x002A), dacData(0, 0), dacPlay(0, kModeMsec), end});
    REQUIRE_FALSE(a.rep.ok());
    CHECK(a.rep.error().code == ErrorCode::BadCommandStream);
    // 0x92 z zerem
    CHECK_FALSE(dacScan({dacSetup(0, kYM2612, 0x002A), dacFreq(0, 0), dacPlay(0, kModeMsec), end}).rep.ok());
    // poprawnie
    CHECK(dacScan({dacSetup(0, kYM2612, 0x002A), dacFreq(0, 8000), dacPlay(0, kModeMsec), end}).rep.ok());
    // 0x92 PRZED skutecznym 0x90 jest ignorowane przez libvgm (Running & 0x80) - czestotliwosc nadal 0
    CHECK_FALSE(dacScan({dacFreq(0, 8000), dacSetup(0, kYM2612, 0x002A), dacPlay(0, kModeMsec), end}).rep.ok());
    // inne tryby nie dziela przez freq
    CHECK(dacScan({dacSetup(0, kYM2612, 0x002A), dacData(0, 0), dacPlay(0, kModeCmds), end}).rep.ok());
}

TEST_CASE("DAC: 0x93 TOEND/BYTES i 0x95 przy DataStep == 0 odrzucone")
{
    const Bytes end = {0x66};
    const Bytes pcm = block(0x00, Bytes(16, 0x40));
    // 0x90 bez 0x91: StepSize = 0 => DataStep = 0
    for (std::uint8_t mode : {kModeToEnd, kModeBytes}) {
        auto r = dacScan({dacSetup(0, kYM2612, 0x002A), dacPlay(0, mode), end});
        REQUIRE_FALSE(r.rep.ok());
        CHECK(r.rep.error().code == ErrorCode::BadCommandStream);
        // z 0x91 (StepSize 0 -> 1): bezpieczne
        CHECK(dacScan({pcm, dacSetup(0, kYM2612, 0x002A), dacData(0, 0), dacPlay(0, mode), end}).rep.ok());
    }
    // CmdSize 2 (32X PWM) * StepSize 128 = 256 -> UINT8 DataStep == 0
    for (std::uint8_t mode : {kModeToEnd, kModeBytes}) {
        auto r = dacScan({pcm, dacSetup(0, kPWM, 0x0001), dacData(0, 0, 128), dacPlay(0, mode), end});
        REQUIRE_FALSE(r.rep.ok());
        CHECK(r.rep.error().code == ErrorCode::BadCommandStream);
        CHECK(dacScan({pcm, dacSetup(0, kPWM, 0x0001), dacData(0, 0, 127), dacPlay(0, mode), end}).rep.ok());
        // ten sam StepSize przy CmdSize 1 (YM2612) jest bezpieczny
        CHECK(dacScan({pcm, dacSetup(0, kYM2612, 0x002A), dacData(0, 0, 128), dacPlay(0, mode), end}).rep.ok());
    }
    // 0x95 (tryb BYTES): StepSize 128 na PWM
    auto b = dacScan({pcm, dacSetup(0, kPWM, 0x0001), dacData(0, 0, 128), dacPlayBlk(0, 0), end});
    REQUIRE_FALSE(b.rep.ok());
    CHECK(b.rep.error().code == ErrorCode::BadCommandStream);
    CHECK(dacScan({pcm, dacSetup(0, kPWM, 0x0001), dacData(0, 0, 2), dacPlayBlk(0, 0), end}).rep.ok());
    // SN76496 (typ 0): CmdSize zalezy od bitu 4 komendy - glosnosc 1, czestotliwosc 2
    CHECK_FALSE(dacScan({pcm, dacSetup(0, 0x00, 0x0000), dacData(0, 0, 128), dacPlayBlk(0, 0), end}).rep.ok());
    CHECK(dacScan({pcm, dacSetup(0, 0x00, 0x0010), dacData(0, 0, 128), dacPlayBlk(0, 0), end}).rep.ok());
}

TEST_CASE("DAC: komendy ignorowane przez libvgm dostaja ostrzezenie, a nie blad")
{
    const Bytes end = {0x66};
    const Bytes pcm = block(0x00, Bytes(16, 0x40));

    // uklad docelowy bez zegara w naglowku (SegaPCM 0x04): strumien powstaje, ale 0x91-0x93 sa ignorowane
    auto a = dacScan({dacSetup(0, 0x04, 0x0000), dacFreq(0, 0), dacPlay(0, kModeMsec), dacPlay(0, kModeBytes), end});
    REQUIRE(a.rep.ok());
    CHECK(hasWarning(a.rep.value().warnings, WarningCode::Other));
    // typ >= 48, instancja 1 bez dual-chip
    CHECK(dacScan({dacSetup(0, 0x30, 0x0000), dacPlay(0, kModeMsec), end}).rep.ok());
    CHECK(dacScan({dacSetup(0, static_cast<std::uint8_t>(0x80 | kYM2612), 0x002A), dacPlay(0, kModeMsec), end}).rep.ok());
    // 0x93 / 0x95 / 0x92 bez 0x90 w ogole
    CHECK(dacScan({dacPlay(3, kModeMsec), dacPlayBlk(3, 0), dacFreq(3, 1), end}).rep.ok());
    // strumien 0xFF: 0x90 ignorowane
    CHECK(dacScan({dacSetup(0xFF, kYM2612, 0x002A), dacPlay(0xFF, kModeMsec), end}).rep.ok());
    // 0x95 bez wybranego banku / z bankiem >= 0x40 / z ID poza bankiem
    CHECK(dacScan({pcm, dacSetup(0, kPWM, 0x0001), dacPlayBlk(0, 0), end}).rep.ok());
    CHECK(dacScan({pcm, dacSetup(0, kPWM, 0x0001), dacData(0, 0x40, 128), dacPlayBlk(0, 0), end}).rep.ok());
    CHECK(dacScan({pcm, dacSetup(0, kPWM, 0x0001), dacData(0, 0, 128), dacPlayBlk(0, 1), end}).rep.ok());
    // ostrzezenie o ignorowanej komendzie pojawia sie raz na rodzaj komendy
    auto w = dacScan({dacPlay(1, kModeMsec), dacPlay(1, kModeMsec), dacPlay(1, kModeMsec), end});
    REQUIRE(w.rep.ok());
    std::size_t ignored = 0;
    for (const Warning& x : w.rep.value().warnings)
        if (x.code == WarningCode::Other) ++ignored;
    CHECK(ignored == 1);
}

TEST_CASE("DAC: stan strumieni nie jest resetowany przy skoku petli - petla jest skanowana drugi raz")
{
    const Bytes setup = dacSetup(0, kYM2612, 0x002A);
    const Bytes end = {0x66};

    // Petla zaczyna sie od 0x93 (za 0x92 z 8000): 1. przebieg OK, a 0x92 z zerem na koncu petli
    // zostawia freq == 0 dla 0x93 w 2. przebiegu.
    Opt o = dacOpt();
    o.loopAt = static_cast<std::int64_t>(setup.size() + dacFreq(0, 8000).size());
    auto bad = dacScan({setup, dacFreq(0, 8000), dacPlay(0, kModeMsec), dacFreq(0, 0), Bytes{0x63}, end}, o);
    REQUIRE_FALSE(bad.rep.ok());
    CHECK(bad.rep.error().code == ErrorCode::BadCommandStream);
    // bez petli ten sam strumien jest bezpieczny
    CHECK(dacScan({setup, dacFreq(0, 8000), dacPlay(0, kModeMsec), dacFreq(0, 0), Bytes{0x63}, end}).rep.ok());
    // petla, w ktorej freq pozostaje niezerowe
    CHECK(dacScan({setup, dacFreq(0, 8000), dacPlay(0, kModeMsec), dacFreq(0, 4000), Bytes{0x63}, end}, o).rep.ok());

    // petla zaczyna sie od 0x92 (8000) - 2. przebieg ustawia freq przed 0x93, wiec jest bezpieczny
    Opt o2 = dacOpt();
    o2.loopAt = static_cast<std::int64_t>(setup.size());
    CHECK(dacScan({setup, dacFreq(0, 8000), dacPlay(0, kModeMsec), dacFreq(0, 0), Bytes{0x63}, end}, o2).rep.ok());
}

TEST_CASE("DAC: wskaznik na bank trzyma tylko skutecznie ustawiony strumien")
{
    const Bytes end = {0x66};
    const Bytes pcm0 = block(0x00, Bytes(8, 1));
    const Bytes more0 = block(0x00, Bytes(4, 2));
    const Bytes setup = dacSetup(0, kYM2612, 0x002A);

    // skuteczny 0x91 -> kolejny blok tego banku (resize) odrzucony
    auto bad = dacScan({pcm0, setup, dacData(0, 0), more0, end});
    REQUIRE_FALSE(bad.rep.ok());
    CHECK(bad.rep.error().code == ErrorCode::BadDataBlock);
    // strumien bez skutecznego 0x90 (uklad docelowy nie istnieje): 0x91 nie ustawia wskaznika
    CHECK(dacScan({pcm0, dacSetup(0, 0x04, 0), dacData(0, 0), more0, end}).rep.ok());
    // strumien przestawiony na inny bank - poprzedni bank nie jest juz trzymany
    CHECK(dacScan({pcm0, block(0x01, Bytes(8, 3)), setup, dacData(0, 0), dacData(0, 1), more0, end}).rep.ok());
    // przestawienie na pusty bank zeruje wskaznik
    CHECK(dacScan({pcm0, setup, dacData(0, 0), dacData(0, 5), more0, end}).rep.ok());
    // dwa strumienie na tym samym banku: zwolnienie jednego nie zwalnia drugiego
    auto two = dacScan({pcm0, setup, dacSetup(1, kYM2612, 0x002A), dacData(0, 0), dacData(1, 0), dacData(0, 7), more0, end});
    REQUIRE_FALSE(two.rep.ok());
    CHECK(two.rep.error().code == ErrorCode::BadDataBlock);
    // pusty blok (dataLen 0) nie powoduje resize
    CHECK(dacScan({pcm0, setup, dacData(0, 0), block(0x00, Bytes{}), end}).rep.ok());
}
