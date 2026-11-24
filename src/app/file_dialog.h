// Okna wyboru plikow/folderow SDL3 (Ctrl+O, Ctrl+Shift+O, Ctrl+I, Ctrl+E i przyciski playlisty).
// Wyniki trafiaja do AppController::submit*(), ktore jest bezpieczne wielowatkowo (callback SDL moze
// przyjsc z innego watku). Okna systemowe nie sa sterowalne testami automatycznymi.
#pragma once

#include "app_controller.h"

struct SDL_Window;

namespace vgmplay::app {

// Nie blokuja. Jedno okno naraz (kolejne wywolania w trakcie sa ignorowane).
void showOpenFileDialog(SDL_Window* window, AppController& controller);     // wiele plikow -> playlista
void showOpenFolderDialog(SDL_Window* window, AppController& controller);   // folder -> skan w tle
void showImportM3uDialog(SDL_Window* window, AppController& controller);
void showExportM3uDialog(SDL_Window* window, AppController& controller);

} // namespace vgmplay::app
