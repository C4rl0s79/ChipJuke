// Edycja wiazan (czysta logika, bez SDL/ImGui): konflikty, zamiana, minimalny zestaw akcji krytycznych.
// Uzywa ekran "Przypisania" (src/ui); testowana jednostkowo.
#pragma once

#include <span>
#include <vector>

#include "vgmplay/input.hpp"

namespace vgmplay::input {

enum class SourceClass : std::uint8_t { Keyboard = 0, Gamepad };

[[nodiscard]] SourceClass classOf(SourceKind k) noexcept;

// Akcje, bez ktorych aplikacja jest nieobslugiwalna padem: nawigacja, potwierdz, wstecz, zmiana panelu.
// Kazda z nich musi miec co najmniej jedno zrodlo pada.
[[nodiscard]] bool isCritical(Action a) noexcept;

// Zrodla danej klasy przypisane do akcji (w kolejnosci wiazan).
[[nodiscard]] std::vector<BindingSource> sourcesOf(std::span<const Binding> b, Action a, SourceClass cls);

// Akcje krytyczne bez zrodla pada.
[[nodiscard]] std::vector<Action> missingCritical(std::span<const Binding> b);

// Akcja (inna niz `except`) juz uzywajaca tego zrodla.
[[nodiscard]] std::optional<Action> findConflict(std::span<const Binding> b, const BindingSource& src, Action except);

// Wiazania do uzycia: pusty zestaw -> domyslne; inaczej kopia, w ktorej akcjom krytycznym bez zrodla pada
// dopisano domyslne wiazania pada (chroni przed "zablokowanym" paddem po recznej edycji pliku).
[[nodiscard]] std::vector<Binding> effectiveBindings(std::span<const Binding> stored);

struct AssignResult {
    bool ok = false;                       // false: nic nie zmieniono (np. zostawiloby akcje krytyczna bez pada)
    std::optional<Action> swappedWith;     // akcja, ktora dotad miala to zrodlo (dostala poprzednie zrodlo edytowanej)
};

// Ustawia JEDNO zrodlo danej klasy dla akcji (zastepuje dotychczasowe zrodla tej klasy). Gdy `src` uzywa inna
// akcja: zamiana - tamta dostaje poprzednie (pierwsze) zrodlo edytowanej akcji (o ile bylo), a traci `src`.
AssignResult assign(std::vector<Binding>& b, Action a, const BindingSource& src);

// Usuwa zrodla danej klasy z akcji. false (bez zmian), gdy akcja krytyczna zostalaby bez pada.
bool clearClass(std::vector<Binding>& b, Action a, SourceClass cls);

// Akcje pokazywane na ekranie przypisan, w stalej kolejnosci.
[[nodiscard]] std::span<const Action> rebindableActions() noexcept;

} // namespace vgmplay::input
