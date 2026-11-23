// vgmplay - kontrakt: loader.hpp
// Zaladowanie i pelna walidacja pliku VGM/VGZ do niemutowalnego LoadedTrack.
//
// WATEK: funkcje sa BLOKUJACE (I/O + dekompresja + pre-skan). Wolno je wolac WYLACZNIE z
// watku workera (nigdy z UI ani z watku silnika/audio). Sa bezpieczne wielowatkowo
// (brak stanu globalnego); wiele workerow naraz jest dozwolone.
//
// Bezpieczenstwo plikow: otwarcie jako zwykly plik (nie katalog/urzadzenie), rozmiar
// sprawdzany na otwartym uchwycie (bez TOCTOU) przed alokacja, czytanie z limitem
// Limits::maxInputBytes, brak podazania za URL; bledy I/O -> Error, nie wyjatki.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "vgmplay/core.hpp"
#include "vgmplay/format.hpp"

namespace vgmplay::loader {

// Niemutowalny wynik ladowania. Tworzony przez loader, potem wspoldzielony przez
// std::shared_ptr<const LoadedTrack> (UI, playlista, silnik). Nigdy nie zmieniany po utworzeniu;
// wolno czytac z dowolnego watku bez synchronizacji.
struct LoadedTrack {
    // Surowe (rozpakowane) bajty + kParserPadding zer na koncu. Rozmiar kontenera =
    // header.fileSize + format::kParserPadding. Wlasciciel: shared_ptr; dekoder kopiuje
    // dane do libvgm, wiec nie musi trzymac wskaznika po open().
    std::shared_ptr<const ByteBuffer> bytes;
    format::HeaderInfo header;
    format::Gd3Tag gd3;
    format::StreamReport stream;       // PRAWDZIWA dlugosc w tickach (stream.totalSamples)
    std::filesystem::path path;        // sciezka zrodlowa (pusta dla ladowania z pamieci)
    bool wasGzip = false;
    std::vector<Warning> warnings;     // zsumowane: header + stream + gd3 + loader

    // Widok surowych bajtow BEZ paddingu (header.fileSize).
    [[nodiscard]] std::span<const std::uint8_t> raw() const noexcept
    {
        if (!bytes) return {};
        return std::span<const std::uint8_t>(bytes->data(),
                                              static_cast<std::size_t>(header.fileSize));
    }
};

// Wczytuje plik, rozpakowuje gzip, waliduje (header, stream, GD3) i dopelnia bufor.
// Kolejnosc kontroli i kody bledow: ADR 0002 pkt 8. Niekrytyczny blad GD3 = ostrzezenie.
// cancel sprawdzany miedzy etapami (Cancelled).
[[nodiscard]] Result<LoadedTrack> loadTrackFromFile(const std::filesystem::path& path,
                                                    const format::Limits& limits = {},
                                                    const CancelToken& cancel = {});

// To samo z bufora w pamieci (testy, przyszle archiwa ZIP); path tylko informacyjnie.
[[nodiscard]] Result<LoadedTrack> loadTrackFromMemory(std::span<const std::uint8_t> input,
                                                      const std::filesystem::path& path = {},
                                                      const format::Limits& limits = {},
                                                      const CancelToken& cancel = {});

} // namespace vgmplay::loader
