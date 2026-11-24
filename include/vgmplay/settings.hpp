// vgmplay - kontrakt: settings.hpp
// Ustawienia aplikacji (JSON), lokalizacja pliku (tryb przenosny) i zapis atomowy.
//
// WATEK: Settings to zwykla struktura (kopiuj do/od watkow wlasnorecznie). Funkcje plikowe sa
// BLOKUJACE - wolac z workera, nie z petli UI. toJson/fromJson sa czyste.
//
// Tryb przenosny: ustawienia leza OBOK .exe (settings.json), gdy ten katalog jest zapisywalny
// (proba utworzenia i usuniecia pliku tymczasowego); w przeciwnym razie %APPDATA%\vgmplay.
// STALA DECYZJA (nie ustawienie): pad nie steruje aplikacja, gdy okno nie ma fokusu
// (input::kGamepadWorksInBackground == false).
//
// Wartosci domyslne oznaczone [robocze] to propozycje do zmiany bez zmiany kontraktu.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "vgmplay/chips.hpp"
#include "vgmplay/core.hpp"
#include "vgmplay/format.hpp"   // Gd3Language
#include "vgmplay/i18n.hpp"     // Language
#include "vgmplay/input.hpp"    // Binding

namespace vgmplay::settings {

inline constexpr std::uint32_t kSettingsSchemaVersion = 1;

enum class Theme : std::uint8_t { Dark = 0, Light = 1 };
enum class ScopeMode : std::uint8_t { Master = 0, PerDevice = 1, PerChannel = 2 };  // pierwsze wydanie: tylko Master
enum class TrackerView : std::uint8_t { Notes = 0, Scopes = 1 };

struct AudioSettings {
    std::string device;                 // identyfikator urzadzenia; pusty = domyslne systemowe
    std::uint32_t bufferMs = 50;        // [robocze]
    std::uint32_t sampleRate = 48000;   // 0 = czestotliwosc urzadzenia
};

struct PlaybackSettings {
    std::uint32_t loopCount = 2;        // 0 = bez konca
    std::uint32_t fadeMs = 1000;
    float volume = 0.8f;                // liniowo 0..1 [robocze]
    // Decyzja uzytkownika 2026-11-23: utwory bez petli koncza sie bez wyciszania (Stop).
    NonLoopEnd nonLoopEnd = NonLoopEnd::Stop;
    std::vector<CoreChoice> cores;      // wybor rdzeni; brak = domyslne
};

struct UiSettings {
    i18n::Language language = i18n::Language::Pl;        // [robocze: pozniej wg jezyka systemu]
    format::Gd3Language gd3Language = format::Gd3Language::En;
    Theme theme = Theme::Dark;
    float fontScale = 1.0f;             // 0.5 .. 3.0
    ScopeMode scopeMode = ScopeMode::Master;
    TrackerView trackerView = TrackerView::Notes;
    // Dlugosc wiersza wzorca w tickach VGM (44100 Hz); 2205 = 1/20 s (decyzja uzytkownika 2026-11-24).
    // Zakres po walidacji: 64 .. 44100.  [dodatek E2]
    std::uint32_t rowTicks = 2205;
};

struct InputSettings {
    std::vector<input::Binding> bindings;   // pusty = domyslne wiazania aplikacji
    std::uint32_t repeatDelayMs = 400;      // [robocze]
    std::uint32_t repeatRateMs = 60;        // [robocze]
};

// Stan sesji do wznowienia przy starcie. [dodatek E2]
// Aktywna playlista i kursor zapisuje plik playlist (PlaylistModel::activeIndex + Playlist::cursor);
// tutaj tylko pozycja w utworze. trackPath sluzy weryfikacji: UI przywraca positionMs tylko wtedy,
// gdy sciezka biezacego wpisu kursora == trackPath (inaczej pozycja = 0).
struct SessionSettings {
    std::string trackPath;              // UTF-8, '/' jako separator; pusty = brak
    std::uint64_t positionMs = 0;
};

struct Settings {
    std::uint32_t schemaVersion = kSettingsSchemaVersion;
    AudioSettings audio;
    PlaybackSettings playback;
    UiSettings ui;
    InputSettings input;
    SessionSettings session;            // [dodatek E2]
};

// Zwraca ustawienia zwalidowane (clamp zakresow: fontScale, volume, bufferMs, repeat*).
// Nieznane pola ignoruje; brakujace = domyslne. schemaVersion > znany -> UnsupportedSchema;
// starszy -> migracja w gore. Zla skladnia -> ParseError.
// `warnings` (opcjonalne, dodatek E2): przyciete zakresy, pola o zlym typie, pominiete wiazania.
// `allowNetworkPaths` [dodatek E2-sec]: false (domyslnie) = session.trackPath w formie UNC jest
// odrzucane z Warning; URL i przestrzen urzadzen odrzucane zawsze. Parser ma limity: plik <= 4 MiB,
// zagniezdzenie JSON <= 8, <= 50000 wartosci (LimitExceeded).
[[nodiscard]] Result<Settings> fromJson(std::string_view json, std::vector<Warning>* warnings = nullptr,
                                        bool allowNetworkPaths = false);
[[nodiscard]] std::string toJson(const Settings& settings);   // UTF-8, deterministyczne

struct SettingsLocation {
    std::filesystem::path file;         // pelna sciezka settings.json
    bool portable = false;              // true = obok .exe
};
// exeDir: katalog pliku wykonywalnego. appDataDir: %APPDATA% (wstrzykiwane dla testow).
// appDataDir zaczynajacy sie od dwoch separatorow (UNC, \?\) jest ignorowany [E2-sec]: wynik jak dla pustego
// (settings.json obok exe, portable=false).
[[nodiscard]] SettingsLocation resolveSettingsLocation(const std::filesystem::path& exeDir,
                                                       const std::filesystem::path& appDataDir);
[[nodiscard]] bool isDirectoryWritable(const std::filesystem::path& dir);

// Brak pliku -> domyslne ustawienia (NIE blad). Uszkodzony plik -> Error (wywolujacy
// zachowuje kopie .bad i uzywa domyslnych). Zapis atomowy: temp + rename, tworzy katalogi.
// Przy uszkodzonym pliku loadSettings sam zachowuje kopie `<plik>.bad` (oryginal zostaje).
[[nodiscard]] Result<Settings> loadSettings(const std::filesystem::path& file,
                                            std::vector<Warning>* warnings = nullptr,
                                            bool allowNetworkPaths = false);
[[nodiscard]] Result<void> saveSettings(const std::filesystem::path& file, const Settings& settings);

} // namespace vgmplay::settings
