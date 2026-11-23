// vgmplay - kontrakt: i18n.hpp
// Minimalne tlumaczenia interfejsu: klucz -> tekst (PL/EN). Bez zaleznosci od UI.
//
// WATEK: tylko watek UI (brak synchronizacji). Zwracane string_view sa wazne do nastepnego
// wywolania setLanguage/loadTable/loadJson na tym samym obiekcie.
// Reguly: nieznany klucz -> zwraca sam klucz (nigdy pusty, nigdy blad); brak tlumaczenia w
// biezacym jezyku -> fallback na angielski, potem klucz. Klucze: ASCII, "obszar.nazwa".
// Tytuly GD3 NIE podlegaja temu mechanizmowi (patrz Settings::ui.gd3Language).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "vgmplay/core.hpp"

namespace vgmplay::i18n {

enum class Language : std::uint8_t { Pl = 0, En = 1 };

// Kod do JSON/ustawien: "pl" / "en".
[[nodiscard]] std::string_view languageCode(Language l) noexcept;
[[nodiscard]] Result<Language> parseLanguageCode(std::string_view code);

struct Entry {
    std::string_view key;
    std::string_view text;
};

class Translator {
public:
    Translator();
    ~Translator();
    Translator(const Translator&) = delete;
    Translator& operator=(const Translator&) = delete;

    void setLanguage(Language l);
    [[nodiscard]] Language language() const noexcept;

    // Zastepuje tabele jezyka (dane kopiowane). Wbudowane tabele PL/EN laduje konstruktor.
    void loadTable(Language l, std::span<const Entry> entries);
    // JSON {"klucz":"tekst",...} (UTF-8); blad skladni -> ParseError, tabela bez zmian.
    [[nodiscard]] Result<void> loadJson(Language l, std::string_view json);

    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] std::string_view tr(std::string_view key) const;
    // Podstawianie {0}, {1}, ... argumentami; brakujacy argument -> zostawia znacznik.
    [[nodiscard]] std::string trf(std::string_view key, std::span<const std::string_view> args) const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace vgmplay::i18n
