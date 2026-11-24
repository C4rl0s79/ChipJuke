// Okno wyboru pliku SDL3 (Ctrl+O / przycisk "Otworz"). Wynik trafia do AppController::loadFile(),
// ktory jest bezpieczny wielowatkowo (callback SDL moze przyjsc z innego watku).
#pragma once

#include "app_controller.h"

struct SDL_Window;

namespace vgmplay::app {

// Nie blokuje. Jedno okno naraz (kolejne wywolania w trakcie sa ignorowane).
void showOpenFileDialog(SDL_Window* window, AppController& controller);

} // namespace vgmplay::app
