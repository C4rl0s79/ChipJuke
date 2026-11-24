// Test regresyjny fuzzingu: odtwarza male korpusy fuzz/corpus/<cel>/ na zwyklym buildzie MSVC
// (bez ASan/libFuzzera) przez TE SAME ciala celow co libFuzzer (fuzz/fuzz_targets.hpp).
// Naruszenie niezmiennika celu (funkcja zwraca false) albo crash = regresja. Korpusy powstaja z kampanii
// fuzzingu (zminimalizowane przez -merge=1); procedura w docs/testing/fuzzing.md.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../fuzz/fuzz_targets.hpp"

namespace {

namespace fs = std::filesystem;

fs::path corpusRoot()
{
    // VGMPLAY_TEST_DATA_DIR = <repo>/tests/data
    return fs::path(VGMPLAY_TEST_DATA_DIR).parent_path().parent_path() / "fuzz" / "corpus";
}

std::vector<std::uint8_t> readBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

using Target = bool (*)(vgmplay::fuzz::Bytes);

// Odtwarza wszystkie pliki korpusu celu; zwraca liczbe odtworzonych plikow.
std::size_t replay(const char* dir, Target fn)
{
    const fs::path d = corpusRoot() / dir;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file()) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        const auto data = readBytes(f);
        INFO("plik korpusu: " << f.string());
        CHECK(fn({data.data(), data.size()}));
    }
    return files.size();
}

struct Case {
    const char* dir;
    Target fn;
};

} // namespace

TEST_CASE("fuzz regress: korpusy odtwarzaja sie bez naruszenia niezmiennikow")
{
    using namespace vgmplay::fuzz;
    const Case cases[] = {
        {"vgm_header", runVgmHeader},       {"gd3", runGd3},
        {"gzip", runGzip},                  {"stream", runStream},
        {"decode", runDecode},              {"pattern", runPattern},
        {"settings", runSettings},          {"playlist_json", runPlaylistJson},
        {"m3u", runM3u},                    {"library_cache", runLibraryCache},
    };
    for (const Case& c : cases) {
        SUBCASE(c.dir)
        {
            // Pusty korpus oznaczalby zgubiony katalog (zle sciezki) - test nic by nie sprawdzal.
            CHECK(replay(c.dir, c.fn) > 0);
        }
    }
}

TEST_CASE("fuzz regress: puste i skrajne wejscia nie przewracaja celow")
{
    using namespace vgmplay::fuzz;
    const std::vector<std::uint8_t> empty;
    const std::vector<std::uint8_t> one{0x00};
    const std::vector<std::uint8_t> ff(4096, 0xFF);
    for (const auto* in : {&empty, &one, &ff}) {
        const Bytes b{in->data(), in->size()};
        CHECK(runVgmHeader(b));
        CHECK(runGd3(b));
        CHECK(runGzip(b));
        CHECK(runStream(b));
        CHECK(runDecode(b));
        CHECK(runPattern(b));
        CHECK(runSettings(b));
        CHECK(runPlaylistJson(b));
        CHECK(runM3u(b));
        CHECK(runLibraryCache(b));
    }
}
