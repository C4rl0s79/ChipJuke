// vgmplay - kontrakt: core.hpp
// Typy bazowe wspolne dla wszystkich modulow: Result<T>, Error, Warning, SampleTime,
// CancelToken, SpscRing<T,N> oraz male enumy wspoldzielone (NonLoopEnd).
//
// ZAMROZONY KONTRAKT (E0). Zmiany wymagaja uzgodnienia ze wszystkimi wlascicielami modulow.
// Naglowek nie zalezy od libvgm, SDL, ImGui ani miniaudio.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vgmplay {

// Pozycja/czas w KLATKACH wyjscia (1 klatka = 1 probka L+R) przy biezacej czestotliwosci
// wyjscia silnika. Nie mylic z "tickami VGM" (stale 44100/s, patrz format.hpp::kVgmSampleRate).
using SampleTime = std::uint64_t;

// Bufor bajtow pliku. Po zaladowaniu przechodzi do std::shared_ptr<const ByteBuffer>
// (niemutowalny, patrz loader.hpp).
using ByteBuffer = std::vector<std::uint8_t>;

// ---------------------------------------------------------------------------
// Bledy i ostrzezenia
// ---------------------------------------------------------------------------

enum class ErrorCode : std::uint16_t {
    InvalidArgument = 1,    // zly argument wywolania (blad programisty / zly indeks)
    Io,                     // blad systemu plikow / odczytu / zapisu
    NotFound,               // brak pliku, urzadzenia, klucza
    AccessDenied,           // brak uprawnien
    TooLarge,               // przekroczono limit rozmiaru (wejscie lub po dekompresji)
    DecompressionFailed,    // uszkodzony lub uciety gzip, brak Z_STREAM_END
    Truncated,              // dane urwane przed koncem struktury
    BadSignature,           // zla sygnatura pliku ("Vgm ", "Gd3 ")
    UnsupportedVersion,     // wersja formatu spoza obslugiwanego zakresu
    BadHeader,              // niespojny/niepoprawny naglowek (offsety, rozmiary)
    BadCommandStream,       // niepoprawny strumien komend (pre-skan)
    BadDataBlock,           // niepoprawny blok danych 0x67
    LimitExceeded,          // limit liczby (komend, plikow, glebokosci, pamieci)
    OutOfMemory,            // std::bad_alloc zlapany na granicy modulu
    Cancelled,              // przerwane przez CancelToken
    PathRejected,           // sciezka odrzucona (URL, wyjscie poza dozwolony katalog, ...)
    ParseError,             // blad skladni JSON / M3U / tlumaczen
    UnsupportedSchema,      // schemaVersion nowszy niz znany
    DecoderFailure,         // blad dekodera/emulatora (silnik)
    Internal,               // naruszenie niezmiennika; blad w kodzie
};

inline constexpr std::uint64_t kNoOffset = (std::numeric_limits<std::uint64_t>::max)();

struct Error {
    ErrorCode code = ErrorCode::Internal;
    // Offset w bajtach (od poczatku danych wejsciowych) powodujacy blad; kNoOffset = nie dotyczy.
    std::uint64_t byteOffset = kNoOffset;
    // Komunikat diagnostyczny dla logu/dewelopera, po angielsku, bez danych osobowych.
    // NIE jest tekstem dla uzytkownika (UI tlumaczy po `code` przez i18n).
    std::string message;
};

enum class WarningCode : std::uint16_t {
    // format / loader
    UnknownVersion = 1,     // wersja VGM nowsza niz znana
    EofOffsetMismatch,      // eofOfs niezgodny z rozmiarem pliku (skorygowano)
    HeaderLengthMismatch,
    EndsEarly,              // brak 0x66 przed koncem danych
    UnknownCommand,         // nieznana, ale pomijalna komenda
    ChipNotInHeader,        // uklad uzyty w strumieniu bez zegara w naglowku
    TooManyChipInstances,   // >2 instancje typu - nadmiarowe zignorowane
    BlockAfterLoop,         // blok 0x67 po loopOfs
    BlockAfterSetData,      // blok PCM po 0x91 dla tego samego banku
    Gd3Invalid,             // GD3 uszkodzony - tagi puste
    TagTruncated,           // pole GD3 przyciete do limitu
    TagSanitized,           // usunieto znaki sterujace / niesparowane surogaty
    // playlist / library
    EntrySkipped,           // pozycja pominieta (np. odrzucona sciezka)
    PathRejected,
    ReparsePointSkipped,
    AccessDenied,
    LimitReached,           // maxFiles / maxDepth osiagniete
    // ogolne
    Other,
};

struct Warning {
    WarningCode code = WarningCode::Other;
    std::uint64_t byteOffset = kNoOffset;
    std::string message;    // jw. jak Error::message
};

// Wynik operacji, ktora moze sie nie udac. Kontrakt nie rzuca wyjatkow (std::bad_alloc jest
// lapany na granicy modulu i zwracany jako ErrorCode::OutOfMemory).
// value()/error() w niewlasciwym stanie: jak std::get (std::bad_variant_access);
// zawsze sprawdzaj ok() najpierw.
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return std::get<0>(v_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(v_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(v_)); }
    [[nodiscard]] const Error& error() const { return std::get<1>(v_); }

private:
    std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : err_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return !err_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const { return err_.value(); }

private:
    std::optional<Error> err_;
};

[[nodiscard]] inline Error makeError(ErrorCode code, std::string message,
                                     std::uint64_t byteOffset = kNoOffset)
{
    return Error{code, byteOffset, std::move(message)};
}

// ---------------------------------------------------------------------------
// Anulowanie dlugich operacji (loader, skan biblioteki, import M3U)
// ---------------------------------------------------------------------------

// Wspoldzielony znacznik anulowania; kopie wskazuja na ten sam stan.
// requestCancel() wolno wolac z dowolnego watku; worker sprawdza go co najmniej raz na plik /
// porcje danych. Jednorazowy (raz ustawiony nie wraca).
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    void requestCancel() noexcept { flag_->store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool isCancelled() const noexcept { return flag_->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// ---------------------------------------------------------------------------
// Wspolne enumy zachowania odtwarzania
// ---------------------------------------------------------------------------

// Co robi odtwarzacz na koncu utworu BEZ petli.
//  Stop    - utwor konczy sie po komendzie 0x66 z krotka cisza ogonowa
//            (DecoderConfig::endSilenceMs); BEZ wyciszania.
//  FadeOut - wyciszenie o dlugosci fadeMs (opcja zachowana na przyszlosc).
// DECYZJA UZYTKOWNIKA Z 2026-11-23: utwory bez petli koncza sie bez wyciszania (Stop; wartosc
// domyslna). Wyciszenie (domyslnie 1 s) dziala tylko po przejsciach petli i nie zalezy od
// tego ustawienia.
enum class NonLoopEnd : std::uint8_t { Stop = 0, FadeOut = 1 };

// ---------------------------------------------------------------------------
// SpscRing - pierscien lock-free, JEDEN producent i JEDEN konsument
// ---------------------------------------------------------------------------
//
// Kontrakt:
//  * N: potega dwojki, N >= 2. Pojemnosc uzyteczna = N (liczniki 64-bitowe).
//  * Pamiec alokowana RAZ w konstruktorze; push/pop NIE alokuja (o ile przypisanie T nie
//    alokuje; dla T = variant ze std::string/std::vector moze - takich T uzywaj tylko poza
//    sciezka audio).
//  * tryPush wolno wolac z JEDNEGO watku (producent), tryPop z JEDNEGO (konsument). Dwa watki
//    po tej samej stronie = wyscig. Producent i konsument moga byc jednym watkiem.
//  * Wait-free po obu stronach, brak blokad. FIFO. Zapis happens-before odczyt (release/acquire).
//  * tryPush zwraca false przy pelnym pierscieniu (element NIE zapisany; polityka po stronie
//    producenta). tryPop zwraca false przy pustym.
//  * T: domyslnie konstruowalny, noexcept przypisanie move.
//  * Nie kopiowalny ani przenaszalny. Duzy T (ScopeBlock) i tak laduje na stercie.
//  * sizeApprox()/empty() dokladne tylko dla jednego watku; z innego - wskazowka.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // padding przez alignas - zamierzony (false sharing)
#endif
template <class T, std::size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N musi byc potega dwojki >= 2");
    static_assert(std::is_default_constructible_v<T>, "T musi byc domyslnie konstruowalny");
    static_assert(std::is_nothrow_move_assignable_v<T>, "T musi miec noexcept przypisanie move");

public:
    SpscRing() : buf_(std::make_unique<T[]>(N)) {}
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

    // Producent.
    [[nodiscard]] bool tryPush(const T& v) { return pushImpl(v); }
    [[nodiscard]] bool tryPush(T&& v) { return pushImpl(std::move(v)); }

    // Konsument.
    [[nodiscard]] bool tryPop(T& out)
    {
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        const std::uint64_t w = write_.load(std::memory_order_acquire);
        if (r == w) return false;
        out = std::move(buf_[static_cast<std::size_t>(r & kMask)]);
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t sizeApprox() const noexcept
    {
        const std::uint64_t w = write_.load(std::memory_order_acquire);
        const std::uint64_t r = read_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(w - r);
    }
    [[nodiscard]] bool empty() const noexcept { return sizeApprox() == 0; }

private:
    static constexpr std::uint64_t kMask = N - 1;

    template <class U>
    bool pushImpl(U&& v)
    {
        const std::uint64_t w = write_.load(std::memory_order_relaxed);
        const std::uint64_t r = read_.load(std::memory_order_acquire);
        if (w - r >= N) return false;
        buf_[static_cast<std::size_t>(w & kMask)] = std::forward<U>(v);
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    std::unique_ptr<T[]> buf_;
    alignas(64) std::atomic<std::uint64_t> write_{0}; // pisze producent
    alignas(64) std::atomic<std::uint64_t> read_{0};  // pisze konsument
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace vgmplay
