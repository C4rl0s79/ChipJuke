// vgmplay - audio_out: wyjscie audio (miniaudio, WASAPI shared)
// Callback urzadzenia TYLKO czyta z PcmRing::consume() (bez alokacji, blokad i logowania).
// Zarzadzanie urzadzeniem (open/start/stop/reopen) - wylacznie z jednego watku (UI/app); callback
// i notyfikacje urzadzenia komunikuja sie z nim przez atomiki.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vgmplay/core.hpp"
#include "vgmplay/pcm_ring.hpp"

namespace vgmplay::audio_out {

struct Config {
    enum class Backend : std::uint8_t {
        Default,    // WASAPI (shared) na Windows
        Null,       // urzadzenie "null" miniaudio: bez dzwieku, ale z prawdziwym zegarem (testy/CI)
    };
    Backend backend = Backend::Default;
    // 0 = natywna czestotliwosc urzadzenia (zalecane; silnik renderuje w tej czestotliwosci).
    // Wartosc != 0 wymusza ja, a konwersje robi WASAPI/miniaudio.
    std::uint32_t sampleRate = 0;
    std::uint32_t periodMs = 10;     // cel dla okresu urzadzenia
    std::uint32_t periods = 3;
    // [dodatek E3] Urzadzenie wyjsciowe: DeviceEntry::id z listDevices(); pusty = domyslne systemowe
    // (i podazanie za zmiana domyslnego). Nieznane id -> open() otwiera domyslne i ustawia
    // DeviceInfo::fellBackToDefault = true (nie blad).
    std::string deviceId;
};

// [dodatek E3] Urzadzenie odtwarzania widziane przez backend (WASAPI shared).
struct DeviceEntry {
    std::string id;          // stabilny miedzy uruchomieniami identyfikator (UTF-8, z ma_device_id)
    std::string name;        // nazwa do wyswietlenia (UTF-8, oczyszczona ze znakow sterujacych)
    bool isDefault = false;  // obecne domyslne systemowe
};

// [dodatek E3] Lista urzadzen odtwarzania. Watek: UI/app (moze trwac do ~100 ms - nie co klatke).
// Backend Null: jedna pozycja {id "null", name "Null", isDefault true}.
[[nodiscard]] Result<std::vector<DeviceEntry>> listDevices(Config::Backend backend = Config::Backend::Default);

struct DeviceInfo {
    std::string name;
    std::string backend;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 2;
    std::uint32_t periodFrames = 0;   // faktyczny okres urzadzenia
    std::uint32_t periods = 0;
    // Bufor po stronie urzadzenia (okres x liczba okresow) w ms; opoznienie calkowite =
    // to + zawartosc PcmRing (queuedFrames / sampleRate).
    double bufferMs = 0.0;
    std::string deviceId;            // [dodatek E3] id otwartego urzadzenia (pusty = domyslne)
    bool fellBackToDefault = false;  // [dodatek E3] zadanego deviceId nie znaleziono
};

// Zdarzenia urzadzenia zgloszone przez notyfikacje miniaudio (odczyt kasuje flagi).
struct DeviceEvents {
    bool lost = false;        // urzadzenie zatrzymalo sie bez stop() - np. odlaczone; wymaga reopen()
    bool rerouted = false;    // miniaudio przelaczylo strumien (zmiana urzadzenia domyslnego)
    bool interrupted = false; // przerwanie sesji (np. inna aplikacja)
};

class AudioOut {
public:
    explicit AudioOut(PcmRing& ring);
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // Otwiera urzadzenie (nie startuje). Ponowne open() zamyka poprzednie.
    [[nodiscard]] Result<DeviceInfo> open(const Config& config = {});
    [[nodiscard]] Result<void> start();
    void stop();
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool isStarted() const noexcept;
    [[nodiscard]] std::uint32_t sampleRate() const noexcept;   // 0 gdy zamkniete
    [[nodiscard]] const DeviceInfo& info() const noexcept;

    // Konsumuje flagi zdarzen (watek UI, np. co klatke).
    [[nodiscard]] DeviceEvents pollEvents() noexcept;
    // Po `lost`: zamyka i otwiera urzadzenie z ta sama konfiguracja (i startuje, jesli grano).
    // Zwraca nowe info; czestotliwosc moze sie zmienic - wtedy silnik trzeba skonfigurowac od nowa.
    [[nodiscard]] Result<DeviceInfo> reopen();

    // Glosnosc liniowa 0..1 (skrot do PcmRing::setVolume).
    void setVolume(float linear) noexcept { ring_.setVolume(linear); }

private:
    struct Impl;
    PcmRing& ring_;
    std::unique_ptr<Impl> impl_;
};

} // namespace vgmplay::audio_out
