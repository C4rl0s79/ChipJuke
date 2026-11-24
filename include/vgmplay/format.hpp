// vgmplay - kontrakt: format.hpp
// CZYSTE funkcje bez I/O i bez libvgm: dekompresja gzip, parsowanie naglowka VGM, GD3 i
// pre-skan strumienia komend (lista walidacji: ADR 0002 pkt 8, libvgm-parser-hazards.md).
// Wszystkie funkcje: bez stanu globalnego, bezpieczne wielowatkowo, odporne na dowolne
// bajty (cel fuzzingu w fuzz/), nie rzucaja (bad_alloc -> ErrorCode::OutOfMemory).
// Offsety w wynikach sa BEZWZGLEDNE (od poczatku pliku po dekompresji); wszystkie
// obliczenia offsetow w UINT64 (bez zawijania UINT32).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "vgmplay/chips.hpp"
#include "vgmplay/core.hpp"

namespace vgmplay::format {

// Czestotliwosc "tickow" czasu VGM - stala w formacie.
inline constexpr std::uint32_t kVgmSampleRate = 44100;

// Zera doklejane za koncem pliku w buforze przekazywanym parserowi libvgm (pkt 8.7 ADR):
// pokrywa memcpy naglowka 0x100 B i operandy ostatniej komendy (H1/H2).
inline constexpr std::size_t kParserPadding = 0x110;

// Limity bezpieczenstwa. Wartosci domyslne to PROPOZYCJE (nie zmierzone na korpusie).
struct Limits {
    std::uint64_t maxInputBytes        = 64ull << 20;  // plik wejsciowy (skompresowany lub surowy)
    std::uint64_t maxDecompressedBytes = 64ull << 20;  // wynik gzip; NIE ufamy ISIZE
    std::uint64_t maxCommands          = 50'000'000;   // liczba komend w pre-skanie
    std::uint64_t maxPcmBudgetBytes    = 256ull << 20; // suma bankow PCM + zapisow ROM/RAM
    std::uint64_t maxSingleBlockBytes  = 64ull << 20;  // pojedynczy blok 0x67 / memSize ROM
    std::uint32_t maxTagFieldBytes     = 4096;         // pole GD3 (UTF-8, po konwersji)
    std::uint8_t  maxInstancesPerChip  = 2;            // libvgm: max 2 instancje typu
    std::uint64_t maxTotalSamples      = 44100ull * 60 * 60 * 24; // sanity: 24 h (ticki 44100)
    std::uint32_t maxDecompressRatio   = 2000;         // gzip: wyjscie/wejscie (stosowane powyzej 1 MiB wyjscia);
                                                       // deflate daje teoretycznie maks. ~1032:1 (bloki zer), wiec
                                                       // 2000 nie odrzuca zadnego poprawnego pliku; realna ochrone
                                                       // przed bomba daje twardy limit maxDecompressedBytes
    // Zegary ukladow (pkt 8 ADR, H6/H7): libvgm tworzy bufor probek o dlugosci 1 s przy natywnej
    // czestotliwosci ukladu (emu/Resampler.c:103) i abort()-uje przy braku pamieci.
    std::uint32_t maxChipClockHz       = 400'000'000;  // zegar po przeliczeniach libvgm (np. C352 x72/div); wiekszy = odrzucenie
    std::uint32_t maxChipSampleRate    = 20'000'000;   // natywna czestotliwosc probek jednego ukladu (zegar / dzielnik rdzenia)
    std::uint64_t maxTotalChipSampleRate = 64'000'000; // suma po wszystkich instancjach (8 B/probke => <= ~512 MiB)
};

// Wpis ukladu z naglowka VGM.
struct ChipEntry {
    ChipType type = ChipType::Unknown;
    std::uint8_t instance = 0;      // 0 lub 1 (bit 30 zegara = "dual chip")
    std::uint32_t clockHz = 0;      // zegar z zamaskowanymi bitami flag (bit 30/31)
    std::uint32_t flags = 0;        // surowe pola dodatkowe naglowka tego ukladu (np. feedback/
                                    // shift SN76496, typ AY8910, flagi); znaczenie zalezy od type.
                                    // Bit 31 = bit 31 surowego zegara (znaczenie zalezy od ukladu,
                                    // np. T6W28 dla SN76496); uklady z dodatkowymi bajtami naglowka
                                    // pakuja je w bitach 0..30 (opis: src/format/header.cpp).
    std::uint32_t volume = 0;       // z naglowka rozszerzonego (0 = brak); bit 15 = wzgledna
};

struct HeaderInfo {
    std::uint32_t versionBcd = 0;       // np. 0x171
    std::uint32_t totalSamples = 0;     // z naglowka, ticki 44100 Hz - NIEZAUFANE (patrz StreamReport)
    std::uint32_t loopSamples = 0;      // dlugosc petli w tickach; 0 = brak petli
    std::uint64_t loopOffset = 0;       // offset BEZWZGLEDNY poczatku petli; 0 = brak
    std::uint32_t rate = 0;             // 0x24: 50/60 Hz, 0 = nieokreslona
    std::vector<ChipEntry> chips;
    std::uint64_t dataOffset = 0;       // poczatek strumienia komend
    std::uint64_t dataEnd = 0;          // koniec strumienia (po korekcie eof i GD3), <= fileSize
    std::uint64_t eofOffset = 0;        // BEZWZGLEDNY eof uzyty przez parser (0x04 + rel, albo fileSize gdy pole
                                        // bylo nieprawidlowe); <= fileSize. Loader ustawia 0x04 w swojej kopii
                                        // tak, by libvgm (liczacy rozmiar Z paddingiem) widzial ten sam eof.
    std::uint64_t gd3Offset = 0;        // 0 = brak
    std::uint64_t fileSize = 0;         // rozmiar surowych bajtow (bez paddingu)
    std::int8_t  volumeModifier = 0;    // surowy bajt 0x7C
    std::int8_t  loopBase = 0;          // 0x7E
    std::uint8_t loopModifier = 0;      // 0x7F (0 = domyslne 0x10)
    std::vector<Warning> warnings;

    [[nodiscard]] bool hasLoop() const noexcept { return loopOffset != 0 && loopSamples != 0; }
};

// Jezyk wyboru pol GD3.
enum class Gd3Language : std::uint8_t { En = 0, Jp = 1 };

// 11 pol GD3 (UTF-8, zsanityzowane: bez NUL/znakow sterujacych/niesparowanych surogatow,
// przyciete do Limits::maxTagFieldBytes). Puste = brak.
struct Gd3Tag {
    std::string trackEn, trackJp;
    std::string gameEn, gameJp;
    std::string systemEn, systemJp;
    std::string authorEn, authorJp;
    std::string releaseDate;
    std::string converter;      // "VGM by"
    std::string notes;
    bool present = false;       // true, gdy GD3 poprawnie sparsowany
    bool truncated = false;     // jakies pole przyciete do Limits::maxTagFieldBytes (-> TagTruncated)
    bool sanitized = false;     // usunieto znaki sterujace / zamieniono niesparowane surogaty (-> TagSanitized)

    // Wybor pola wg jezyka z fallbackiem na drugi jezyk, gdy puste.
    // Implementacja inline w pliku .cpp modulu format lub u wywolujacego; tu tylko deklaracja.
    [[nodiscard]] const std::string& track(Gd3Language l) const noexcept;
    [[nodiscard]] const std::string& game(Gd3Language l) const noexcept;
    [[nodiscard]] const std::string& system(Gd3Language l) const noexcept;
    [[nodiscard]] const std::string& author(Gd3Language l) const noexcept;
};

struct Decompressed {
    ByteBuffer bytes;
    bool wasGzip = false;
};

// Jesli wejscie zaczyna sie od 1F 8B: rozpakowanie WLASNYM zlib (inflateInit2(15|32) lub
// 16+15), z limitem wyjscia (Limits::maxDecompressedBytes; ISIZE ignorowane), wymaga
// Z_STREAM_END (uciety/uszkodzony = DecompressionFailed). Wynik zaczynajacy sie znow od
// 1F 8B (gzip w gzip) = BadSignature. Wejscie > maxInputBytes = TooLarge.
// Jesli to nie gzip: kopia wejscia (wasGzip=false).
[[nodiscard]] Result<Decompressed> decompressIfGzip(std::span<const std::uint8_t> input,
                                                    const Limits& limits = {});

// Parsuje i waliduje naglowek surowego (juz rozpakowanego) pliku VGM: punkty 3-6, 11, 14 z ADR 0002.
// Bledy krytyczne -> Error; niespojnosci naprawialne -> HeaderInfo::warnings.
[[nodiscard]] Result<HeaderInfo> parseVgmHeader(std::span<const std::uint8_t> file,
                                                const Limits& limits = {});

// Parsuje GD3 wskazany przez header.gd3Offset (UTF-16LE -> UTF-8 WLASNYM dekoderem, bez WinAPI).
// Brak GD3 (gd3Offset == 0) -> Gd3Tag{present=false}, NIE blad. Uszkodzony GD3 -> Error
// (wywolujacy traktuje jako niekrytyczny; loader dodaje Warning Gd3Invalid).
[[nodiscard]] Result<Gd3Tag> parseGd3(std::span<const std::uint8_t> file, const HeaderInfo& header,
                                      const Limits& limits = {});

struct StreamReport {
    std::uint64_t commandCount = 0;
    std::uint64_t totalSamples = 0;     // PRAWDZIWA dlugosc: suma opoznien do 0x66 (ticki 44100)
    std::uint64_t loopStartSample = 0;  // pozycja petli w tickach; wazne gdy header.hasLoop()
    std::uint64_t pcmBytes = 0;         // suma bankow PCM
    std::uint64_t romWriteBytes = 0;    // suma memSize zapisow ROM
    bool hasEndCommand = false;         // znaleziono 0x66
    // Blok PCM (0x00..0x7F) lezy w punkcie petli albo za nim: libvgm w 2. przejsciu petli
    // wykonalby jego dane jako komendy. Plik jest przyjmowany, ale gra RAZ, bez petli
    // (decyzja uzytkownika 2026-11-23); loader zeruje pola petli w swojej kopii bufora.
    bool loopDisabled = false;
    std::vector<Warning> warnings;
};

// Pre-skan strumienia komend od dataOffset do dataEnd (ADR 0002 pkt 8.8-8.10): kazda komenda
// miesci sie w dataEnd, nieznane komendy, liczba komend, bloki danych 0x67 (rozmiary,
// kompresja, tabele, budzety), kolejnosc blok/0x91/petla. Sukces = bufor jest bezpieczny dla
// libvgm (po dopelnieniu kParserPadding). Funkcja czysta; zlozonosc O(rozmiar).
[[nodiscard]] Result<StreamReport> validateCommandStream(std::span<const std::uint8_t> file,
                                                         const HeaderInfo& header,
                                                         const Limits& limits = {});

// Lacki bufora przekazywanego do libvgm (WSPOLNE dla loadera i VgmDecoder::open). `raw` = surowe bajty pliku
// (rozmiar == header.fileSize, jeszcze BEZ paddingu; wywolac przed dopelnieniem). Modyfikuje WYLACZNIE kopie:
//  * 0x04 (eofOfs) := header.eofOffset - 4, zeby libvgm (liczacy rozmiar z paddingiem) widzial ten sam eof,
//    a tym samym dataEnd/loopOfs/GD3 co pre-skan;
//  * 0x1C/0x20 := 0 gdy stream.loopDisabled (blok PCM w/za punktem petli).
// Nie zmienia HeaderInfo - wywolujacy sam zeruje loopOffset/loopSamples, jesli ich uzywa.
void applyLibvgmPatches(std::span<std::uint8_t> raw, const HeaderInfo& header, const StreamReport& stream) noexcept;

} // namespace vgmplay::format
