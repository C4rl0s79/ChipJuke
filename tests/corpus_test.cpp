// Test korpusu: dla kazdego wpisu tests/data/manifest.json (files[] i limitCases[]) laduje plik
// przez loader::loadTrackFromFile i lancuch format::* (jak w loaderze), po czym porownuje wynik
// z `expect`. Wpis z `expect.alternatives` przechodzi, gdy wynik jest wariantem podstawowym LUB
// alternatywnym; ktory zaszedl - wypisuje MESSAGE (lista do zamykania niejednoznacznosci kontraktu).
// Oczekiwania nie sa powtarzane na sztywno w kodzie - pochodza z manifestu (generator tools/vgmgen).
// Opis: docs/testing/e1-test-plan.md, tests/data/README.md.
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "test_util.hpp"
#include "vgmplay/format.hpp"
#include "vgmplay/loader.hpp"

namespace {

using nlohmann::json;
using namespace vgmplay;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Nazwy kodow <-> enumy (lista musi pokrywac include/vgmplay/core.hpp; nieznana nazwa
// w manifescie to blad testu, nie "po cichu pominiety" wpis).
// ---------------------------------------------------------------------------
#define VGMPLAY_ERRORS(X)                                                                          \
    X(InvalidArgument) X(Io) X(NotFound) X(AccessDenied) X(TooLarge) X(DecompressionFailed)        \
    X(Truncated) X(BadSignature) X(UnsupportedVersion) X(BadHeader) X(BadCommandStream)            \
    X(BadDataBlock) X(LimitExceeded) X(OutOfMemory) X(Cancelled) X(PathRejected) X(ParseError)     \
    X(UnsupportedSchema) X(DecoderFailure) X(Internal)
#define VGMPLAY_WARNINGS(X)                                                                        \
    X(UnknownVersion) X(EofOffsetMismatch) X(HeaderLengthMismatch) X(EndsEarly) X(UnknownCommand)  \
    X(ChipNotInHeader) X(TooManyChipInstances) X(BlockAfterLoop) X(BlockAfterSetData) X(Gd3Invalid)\
    X(TagTruncated) X(TagSanitized) X(EntrySkipped) X(PathRejected) X(ReparsePointSkipped)         \
    X(AccessDenied) X(LimitReached) X(Other)

std::optional<ErrorCode> errorFromName(const std::string& n)
{
#define X(name) if (n == #name) return ErrorCode::name;
    VGMPLAY_ERRORS(X)
#undef X
    return std::nullopt;
}
std::optional<WarningCode> warningFromName(const std::string& n)
{
#define X(name) if (n == #name) return WarningCode::name;
    VGMPLAY_WARNINGS(X)
#undef X
    return std::nullopt;
}
std::string errorName(ErrorCode c)
{
#define X(name) if (c == ErrorCode::name) return #name;
    VGMPLAY_ERRORS(X)
#undef X
    return "?";
}
std::string warningName(WarningCode c)
{
#define X(name) if (c == WarningCode::name) return #name;
    VGMPLAY_WARNINGS(X)
#undef X
    return "?";
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------
const json& manifest()
{
    static const json m = [] {
        const auto bytes = testutil::readAll(testutil::dataDir() / "manifest.json");
        REQUIRE_MESSAGE(!bytes.empty(), "brak tests/data/manifest.json");
        return json::parse(bytes.begin(), bytes.end());
    }();
    return m;
}

format::Limits limitsFrom(const json& j)
{
    format::Limits l;
    if (!j.is_object()) return l;
    auto u64 = [&](const char* k, std::uint64_t& dst) { if (j.contains(k)) dst = j[k].get<std::uint64_t>(); };
    u64("maxInputBytes", l.maxInputBytes);
    u64("maxDecompressedBytes", l.maxDecompressedBytes);
    u64("maxCommands", l.maxCommands);
    u64("maxPcmBudgetBytes", l.maxPcmBudgetBytes);
    u64("maxSingleBlockBytes", l.maxSingleBlockBytes);
    u64("maxTotalSamples", l.maxTotalSamples);
    if (j.contains("maxTagFieldBytes")) l.maxTagFieldBytes = j["maxTagFieldBytes"].get<std::uint32_t>();
    if (j.contains("maxInstancesPerChip")) l.maxInstancesPerChip = j["maxInstancesPerChip"].get<std::uint8_t>();
    if (j.contains("maxDecompressRatio")) l.maxDecompressRatio = j["maxDecompressRatio"].get<std::uint32_t>();
    return l;
}

// ---------------------------------------------------------------------------
// Warianty oczekiwan
// ---------------------------------------------------------------------------
struct Variant {
    bool ok = false;
    ErrorCode error = ErrorCode::Internal;
    std::set<WarningCode> warnings;   // dla ok: zbior MUSI byc rowny
    std::string label;                // "podstawowy" / "alternatywa #k"

    [[nodiscard]] std::string describe() const
    {
        std::string s = ok ? "ok" : "error " + errorName(error);
        if (ok) {
            s += " warnings={";
            for (auto w : warnings) s += warningName(w) + ",";
            s += "}";
        }
        return s;
    }
};

Variant parseVariant(const json& e, const std::string& label)
{
    Variant v;
    v.label = label;
    v.ok = e.at("result").get<std::string>() == "ok";
    if (!v.ok) {
        const auto name = e.at("error").get<std::string>();
        const auto c = errorFromName(name);
        REQUIRE_MESSAGE(c.has_value(), "nieznany ErrorCode w manifescie: " << name);
        v.error = *c;
    }
    if (e.contains("warnings"))
        for (const auto& w : e["warnings"]) {
            const auto name = w.get<std::string>();
            const auto c = warningFromName(name);
            REQUIRE_MESSAGE(c.has_value(), "nieznany WarningCode w manifescie: " << name);
            v.warnings.insert(*c);
        }
    return v;
}

std::vector<Variant> variantsOf(const json& expect)
{
    std::vector<Variant> out;
    out.push_back(parseVariant(expect, "podstawowy"));
    if (expect.contains("alternatives")) {
        int k = 1;
        for (const auto& a : expect["alternatives"]) out.push_back(parseVariant(a, "alternatywa #" + std::to_string(k++)));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Wynik lancucha format::* (kopia logiki loadera; sluzy do sprawdzenia pol szczegolowych)
// ---------------------------------------------------------------------------
struct Chain {
    std::optional<Error> error;
    format::HeaderInfo header;
    format::StreamReport stream;
    format::Gd3Tag gd3;
    bool gd3Failed = false;
    std::uint64_t rawSize = 0;
    std::set<WarningCode> warnings;   // jak w LoadedTrack::warnings
};

Chain runChain(std::span<const std::uint8_t> input, const format::Limits& lim)
{
    Chain c;
    format::Decompressed dec;
    if (input.size() >= 2 && input[0] == 0x1F && input[1] == 0x8B) {
        auto r = format::decompressIfGzip(input, lim);
        if (!r) { c.error = r.error(); return c; }
        dec = std::move(r).value();
    } else {
        if (input.size() > lim.maxInputBytes) {
            c.error = makeError(ErrorCode::TooLarge, "input exceeds size limit");
            return c;
        }
        dec.bytes.assign(input.begin(), input.end());
    }
    c.rawSize = dec.bytes.size();
    auto h = format::parseVgmHeader(dec.bytes, lim);
    if (!h) { c.error = h.error(); return c; }
    c.header = std::move(h).value();
    auto s = format::validateCommandStream(dec.bytes, c.header, lim);
    if (!s) { c.error = s.error(); return c; }
    c.stream = std::move(s).value();
    for (const auto& w : c.header.warnings) c.warnings.insert(w.code);
    for (const auto& w : c.stream.warnings) c.warnings.insert(w.code);
    auto g = format::parseGd3(dec.bytes, c.header, lim);
    if (g) {
        c.gd3 = std::move(g).value();
        if (c.gd3.truncated) c.warnings.insert(WarningCode::TagTruncated);
        if (c.gd3.sanitized) c.warnings.insert(WarningCode::TagSanitized);
    } else {
        c.gd3Failed = true;
        c.warnings.insert(WarningCode::Gd3Invalid);
    }
    return c;
}

// ---------------------------------------------------------------------------
// Pomocnicze: UTF-8
// ---------------------------------------------------------------------------
bool validUtf8(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n;
        std::uint32_t cp;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
        else return false;
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size()) return false;
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000)) return false;  // nadmiarowe
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;                                                   // surogaty
        if (cp > 0x10FFFF) return false;
        i += n + 1;
    }
    return true;
}

bool hasControl(std::string_view s)
{
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Rejestr pokrycia (macierz kodow zaobserwowanych w tescie)
// ---------------------------------------------------------------------------
struct Coverage {
    bool ran = false;
    std::set<ErrorCode> errors;
    std::set<WarningCode> warnings;
    std::vector<std::string> alternativesUsed;
    std::size_t entries = 0;
    std::size_t limitCases = 0;
};
Coverage& coverage()
{
    static Coverage c;
    return c;
}

// ---------------------------------------------------------------------------
// Porownanie jednego wpisu
// ---------------------------------------------------------------------------
void checkHeaderAgainst(const json& hj, const format::HeaderInfo& h)
{
    auto eq = [&](const char* k, std::uint64_t actual) {
        if (hj.contains(k)) CHECK_MESSAGE(actual == hj[k].get<std::uint64_t>(), "header." << k);
    };
    eq("versionBcd", h.versionBcd);
    eq("totalSamples", h.totalSamples);
    eq("loopSamples", h.loopSamples);
    eq("rate", h.rate);
    eq("dataOffset", h.dataOffset);
    eq("dataEnd", h.dataEnd);
    eq("gd3Offset", h.gd3Offset);
    eq("fileSize", h.fileSize);
    if (hj.contains("hasLoop")) CHECK_MESSAGE(h.hasLoop() == hj["hasLoop"].get<bool>(), "header.hasLoop");
    if (hj.contains("chips")) {
        CHECK_MESSAGE(h.chips.size() == hj["chips"].size(), "liczba ukladow w naglowku");
        for (const auto& cj : hj["chips"]) {
            const auto type = cj["type"].get<std::string>();
            const auto inst = cj["instance"].get<int>();
            const auto it = std::find_if(h.chips.begin(), h.chips.end(), [&](const format::ChipEntry& c) {
                return chipTypeName(c.type) == type && c.instance == inst;
            });
            REQUIRE_MESSAGE(it != h.chips.end(), "brak ukladu " << type << " instancja " << inst);
            CHECK_MESSAGE(it->clockHz == cj["clockHz"].get<std::uint32_t>(), type << " clockHz");
            if (cj.contains("volume")) CHECK_MESSAGE(it->volume == cj["volume"].get<std::uint32_t>(), type << " volume");
        }
    }
}

void checkStreamAgainst(const json& sj, const format::StreamReport& s)
{
    auto eq = [&](const char* k, std::uint64_t actual) {
        if (sj.contains(k)) CHECK_MESSAGE(actual == sj[k].get<std::uint64_t>(), "stream." << k);
    };
    eq("totalSamples", s.totalSamples);
    eq("loopStartSample", s.loopStartSample);
    eq("commandCount", s.commandCount);
    eq("pcmBytes", s.pcmBytes);
    eq("romWriteBytes", s.romWriteBytes);
    if (sj.contains("hasEndCommand")) CHECK_MESSAGE(s.hasEndCommand == sj["hasEndCommand"].get<bool>(), "stream.hasEndCommand");
}

void checkGd3Against(const json& gj, const format::Gd3Tag& g)
{
    if (gj.contains("present")) CHECK_MESSAGE(g.present == gj["present"].get<bool>(), "gd3.present");
    const std::map<std::string, const std::string*> fields = {
        {"trackEn", &g.trackEn}, {"trackJp", &g.trackJp}, {"gameEn", &g.gameEn}, {"gameJp", &g.gameJp},
        {"systemEn", &g.systemEn}, {"systemJp", &g.systemJp}, {"authorEn", &g.authorEn},
        {"authorJp", &g.authorJp}, {"releaseDate", &g.releaseDate}, {"converter", &g.converter},
        {"notes", &g.notes}};
    for (const auto& [name, ptr] : fields)
        if (gj.contains(name)) CHECK_MESSAGE(*ptr == gj[name].get<std::string>(), "gd3." << name);
    if (gj.contains("trackEnContains"))
        for (const auto& sub : gj["trackEnContains"])
            CHECK_MESSAGE(g.trackEn.find(sub.get<std::string>()) != std::string::npos, "trackEn zawiera " << sub);
    if (gj.contains("trackEnPrefix"))
        CHECK_MESSAGE(g.trackEn.rfind(gj["trackEnPrefix"].get<std::string>(), 0) == 0, "trackEn prefix");
    if (gj.contains("trackEnMaxBytes"))
        CHECK_MESSAGE(g.trackEn.size() <= gj["trackEnMaxBytes"].get<std::size_t>(), "trackEn rozmiar " << g.trackEn.size());
    for (const auto& [name, ptr] : fields) {
        if (gj.value("mustBeValidUtf8", false)) CHECK_MESSAGE(validUtf8(*ptr), "gd3." << name << " nie jest poprawnym UTF-8");
        if (gj.value("mustNotContainControl", false)) CHECK_MESSAGE(!hasControl(*ptr), "gd3." << name << " zawiera znaki sterujace");
    }
}

// Zwraca true, gdy wszystkie sprawdzenia przeszly (dla potrzeb zgrupowania w raporcie).
void checkEntry(const json& entry, const std::string& id, const fs::path& file, bool full)
{
    CAPTURE(id);
    const json& expect = entry.at("expect");
    const auto lim = limitsFrom(entry.value("limits", json::object()));
    const auto variants = variantsOf(expect);
    const auto bytes = testutil::readAll(file);

    // Plik na dysku: rozmiar i sha256 zgodne z manifestem (korpus zsynchronizowany z generatorem).
    if (entry.contains("sizeBytes")) CHECK_MESSAGE(bytes.size() == entry["sizeBytes"].get<std::size_t>(), "rozmiar pliku");

    auto loaded = loader::loadTrackFromFile(file, lim);
    const Chain chain = runChain(bytes, lim);

    // Spojnosc loader <-> lancuch format::*.
    CHECK_MESSAGE(loaded.ok() == !chain.error.has_value(), "loader i lancuch format roznia sie co do wyniku");

    // Ktory wariant pasuje?
    const Variant* matched = nullptr;
    for (const auto& v : variants) {
        if (v.ok != loaded.ok()) continue;
        if (!v.ok) {
            if (loaded.error().code == v.error) { matched = &v; break; }
        } else {
            std::set<WarningCode> actual;
            for (const auto& w : loaded.value().warnings) actual.insert(w.code);
            if (actual == v.warnings) { matched = &v; break; }
        }
    }
    std::string actualDesc;
    if (loaded.ok()) {
        actualDesc = "ok warnings={";
        for (const auto& w : loaded.value().warnings) actualDesc += warningName(w.code) + ",";
        actualDesc += "}";
    } else {
        actualDesc = "error " + errorName(loaded.error().code) + " (" + loaded.error().message + ")";
    }
    INFO("rzeczywisty wynik: " << actualDesc);
    {
        std::string exp;
        for (const auto& v : variants) exp += "\n    " + v.label + ": " + v.describe();
        INFO("oczekiwane warianty:" << exp);
    }
    if (matched == nullptr) {
        FAIL_CHECK(id << ": wynik nie pasuje do zadnego wariantu");
        return;
    }

    auto& cov = coverage();
    if (matched != &variants.front()) {
        const std::string line = id + " -> " + matched->label + " (" + matched->describe() + ")";
        cov.alternativesUsed.push_back(line);
        MESSAGE("[ALTERNATYWA] " << line);
    }
    if (loaded.ok()) {
        for (const auto& w : loaded.value().warnings) cov.warnings.insert(w.code);
    } else {
        cov.errors.insert(loaded.error().code);
        // Niezmienniki bledu: komunikat niepusty, offset (jesli ustawiony) w obrebie danych.
        CHECK_MESSAGE(!loaded.error().message.empty(), "pusty komunikat bledu");
        const auto off = loaded.error().byteOffset;
        if (off != kNoOffset) {
            const std::uint64_t limit = std::max<std::uint64_t>({bytes.size(), chain.rawSize,
                entry.value("rawSizeBytes", std::uint64_t{0})});
            CHECK_MESSAGE(off <= limit, "byteOffset " << off << " poza danymi (" << limit << ")");
        }
        // Ten sam kod z lancucha format.
        if (chain.error) CHECK(chain.error->code == loaded.error().code);
    }

    // Determinizm: drugie wywolanie daje ten sam wynik.
    {
        auto again = loader::loadTrackFromFile(file, lim);
        CHECK(again.ok() == loaded.ok());
        if (again.ok() && loaded.ok()) {
            CHECK(again.value().warnings.size() == loaded.value().warnings.size());
            CHECK(again.value().header.dataOffset == loaded.value().header.dataOffset);
            CHECK(*again.value().bytes == *loaded.value().bytes);
        } else if (!again.ok() && !loaded.ok()) {
            CHECK(again.error().code == loaded.error().code);
        }
    }

    if (!loaded.ok()) return;
    const auto& t = loaded.value();

    // Niezmienniki dla kazdego przyjetego pliku.
    CHECK(t.header.dataOffset <= t.header.dataEnd);
    CHECK(t.header.dataEnd <= t.header.fileSize);
    CHECK(t.header.gd3Offset <= t.header.fileSize);
    CHECK(t.header.loopOffset <= t.header.fileSize);
    REQUIRE(t.bytes);
    CHECK(t.bytes->size() == t.header.fileSize + format::kParserPadding);
    CHECK(t.raw().size() == t.header.fileSize);
    bool zeroPad = true;
    for (std::size_t i = t.header.fileSize; i < t.bytes->size(); ++i) zeroPad = zeroPad && (*t.bytes)[i] == 0;
    CHECK_MESSAGE(zeroPad, "padding za koncem pliku nie jest zerowy");
    // Ostrzezenia loadera = suma header + stream + gd3 (jako zbior kodow).
    {
        std::set<WarningCode> lw;
        for (const auto& w : t.warnings) lw.insert(w.code);
        CHECK_MESSAGE(lw == chain.warnings, "ostrzezenia loadera != ostrzezenia lancucha format");
    }
    // Petla wylaczona <=> BlockAfterLoop; loader zeruje pola petli (decyzja 2026-11-23).
    const bool hasBal = chain.warnings.count(WarningCode::BlockAfterLoop) > 0;
    CHECK_MESSAGE(chain.stream.loopDisabled == hasBal,
                  "loopDisabled <=> BlockAfterLoop");
    if (chain.stream.loopDisabled) {
        CHECK(t.stream.loopDisabled);
        CHECK_FALSE(t.header.hasLoop());
        CHECK(t.header.loopOffset == 0);
        CHECK(t.header.loopSamples == 0);
    }

    // Pola szczegolowe tylko dla wariantu podstawowego (alternatywy nie opisuja header/stream/gd3).
    if (full && matched == &variants.front() && !chain.error) {
        // Zakres: manifest opisuje HeaderInfo z parseVgmHeader (przed zerowaniem petli w loaderze).
        if (entry.contains("header")) checkHeaderAgainst(entry["header"], chain.header);
        if (entry.contains("stream")) checkStreamAgainst(entry["stream"], chain.stream);
        if (entry.contains("gd3")) {
            checkGd3Against(entry["gd3"], chain.gd3);
            // To samo musi byc w wyniku loadera.
            checkGd3Against(entry["gd3"], t.gd3);
        }
    }
}

// Wpisy z potwierdzonym bledem IMPLEMENTACJI (patrz raport/dok.): osobny test z may_fail,
// zeby nie maskowac reszty. Klucz: sciezka z manifestu (lub id limitCase). Wypelniane po przegladzie.
const std::set<std::string>& knownImplementationBugs()
{
    static const std::set<std::string> s = {
    };
    return s;
}

fs::path pathOf(const json& entry) { return testutil::dataDir() / fs::path(entry.at("path").get<std::string>()); }

} // namespace

TEST_CASE("korpus: manifest jest wczytywalny i zgodny ze schematem")
{
    const auto& m = manifest();
    CHECK(m.at("schemaVersion").get<int>() == 1);
    CHECK(m.at("files").size() >= 81);
    const bool noUnverified = !m.contains("unverifiedNames") || m["unverifiedNames"].empty();
    CHECK_MESSAGE(noUnverified, "manifest uzywa nazw spoza kontraktu");
}

TEST_CASE("korpus: brak plikow-sierot - katalogi valid/ i malformed/ pokrywaja sie z manifestem")
{
    std::set<std::string> onDisk, inManifest;
    for (const char* sub : {"valid", "malformed"})
        for (const auto& e : fs::directory_iterator(testutil::dataDir() / sub))
            if (e.is_regular_file()) onDisk.insert(std::string(sub) + "/" + e.path().filename().string());
    for (const auto& f : manifest().at("files")) inManifest.insert(f.at("path").get<std::string>());
    for (const auto& p : onDisk) CHECK_MESSAGE(inManifest.count(p), "plik bez wpisu w manifescie: " << p);
    for (const auto& p : inManifest) CHECK_MESSAGE(onDisk.count(p), "wpis manifestu bez pliku: " << p);
    // Hash pliku zgodny z manifestem (korpus nie zostal zmieniony recznie).
    for (const auto& f : manifest().at("files")) {
        const auto bytes = testutil::readAll(pathOf(f));
        CHECK_MESSAGE(testutil::sha256Hex(bytes.data(), bytes.size()) == f.at("sha256").get<std::string>(),
                      "sha256 pliku " << f["path"]);
    }
}

TEST_CASE("korpus: files[] - wynik loadera i pola szczegolowe zgodne z manifestem")
{
    auto& cov = coverage();
    cov.ran = true;
    for (const auto& entry : manifest().at("files")) {
        const std::string id = entry.at("path").get<std::string>();
        if (knownImplementationBugs().count(id)) continue;
        SUBCASE(id.c_str()) {
            checkEntry(entry, id, pathOf(entry), true);
            ++cov.entries;
        }
    }
}

TEST_CASE("korpus: limitCases[] - te same pliki z nizszymi limitami")
{
    auto& cov = coverage();
    cov.ran = true;
    for (const auto& entry : manifest().at("limitCases")) {
        const std::string id = "limitCase:" + entry.at("id").get<std::string>();
        if (knownImplementationBugs().count(id)) continue;
        SUBCASE(id.c_str()) {
            checkEntry(entry, id, pathOf(entry), false);
            ++cov.limitCases;
        }
    }
}

TEST_CASE("korpus: znane bledy implementacji (oczekiwane niepowodzenia - patrz raport)" * doctest::may_fail())
{
    for (const auto& entry : manifest().at("files")) {
        const std::string id = entry.at("path").get<std::string>();
        if (!knownImplementationBugs().count(id)) continue;
        checkEntry(entry, id, pathOf(entry), true);
    }
    for (const auto& entry : manifest().at("limitCases")) {
        const std::string id = "limitCase:" + entry.at("id").get<std::string>();
        if (!knownImplementationBugs().count(id)) continue;
        checkEntry(entry, id, pathOf(entry), false);
    }
}

TEST_CASE("korpus: wartosci z .vgz - rawSha256 tresci zgodny z manifestem (niezalezny od zlib)")
{
    for (const auto& f : manifest().at("files")) {
        if (!f.contains("rawSha256")) continue;
        const auto bytes = testutil::readAll(pathOf(f));
        auto d = format::decompressIfGzip(bytes);
        if (!d) continue;   // pliki-bomby: obslugiwane w tescie wpisow
        CHECK_MESSAGE(d.value().wasGzip, f["path"]);
        CHECK_MESSAGE(testutil::sha256Hex(d.value().bytes.data(), d.value().bytes.size()) == f["rawSha256"].get<std::string>(),
                      "rawSha256 " << f["path"]);
        CHECK_MESSAGE(d.value().bytes.size() == f["rawSizeBytes"].get<std::size_t>(), "rawSizeBytes " << f["path"]);
    }
}

TEST_CASE("korpus: macierz pokrycia kodow bledow i ostrzezen z manifestu")
{
    auto& cov = coverage();
    if (!cov.ran) { MESSAGE("pominieto: test wpisow nie byl uruchomiony w tym przebiegu (filtr)"); return; }

    // Kody wymienione w manifescie (podstawowe i alternatywne).
    std::set<ErrorCode> wantE;
    std::set<WarningCode> wantW;
    auto scan = [&](const json& expect) {
        for (const auto& v : variantsOf(expect)) {
            if (!v.ok) wantE.insert(v.error);
            wantW.insert(v.warnings.begin(), v.warnings.end());
        }
    };
    for (const auto& f : manifest()["files"]) scan(f["expect"]);
    for (const auto& f : manifest()["limitCases"]) scan(f["expect"]);

    std::ostringstream os;
    os << "\n--- Macierz pokrycia ---\nwpisy files[]: " << cov.entries << ", limitCases[]: " << cov.limitCases
       << "\nbledy zaobserwowane:";
    for (auto e : cov.errors) os << " " << errorName(e);
    os << "\nostrzezenia zaobserwowane:";
    for (auto w : cov.warnings) os << " " << warningName(w);
    os << "\nalternatywy uzyte (" << cov.alternativesUsed.size() << "):";
    for (const auto& a : cov.alternativesUsed) os << "\n  " << a;
    MESSAGE(os.str());

    // Kod wymieniony jako wynik PODSTAWOWY w manifescie, a nigdy nie zaobserwowany, to luka testu
    // (alternatywy moga nie zajsc - to nie jest luka).
    std::set<ErrorCode> primaryE;
    std::set<WarningCode> primaryW;
    auto scanPrimary = [&](const json& expect) {
        const auto v = parseVariant(expect, "p");
        if (!v.ok) primaryE.insert(v.error);
        primaryW.insert(v.warnings.begin(), v.warnings.end());
    };
    for (const auto& f : manifest()["files"]) scanPrimary(f["expect"]);
    for (const auto& f : manifest()["limitCases"]) scanPrimary(f["expect"]);
    for (auto e : primaryE) CHECK_MESSAGE(cov.errors.count(e), "ErrorCode z manifestu nie zaobserwowany: " << errorName(e));
    for (auto w : primaryW) CHECK_MESSAGE(cov.warnings.count(w), "WarningCode z manifestu nie zaobserwowany: " << warningName(w));
    (void)wantE; (void)wantW;
}
