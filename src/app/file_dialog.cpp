#include "file_dialog.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <filesystem>

namespace vgmplay::app {
namespace {

std::atomic<bool> g_open{false};

void SDLCALL onSelected(void* userdata, const char* const* files, int /*filter*/)
{
    auto* controller = static_cast<AppController*>(userdata);
    if (!files) {
        std::fprintf(stderr, "SDL_ShowOpenFileDialog: %s\n", SDL_GetError());
    } else if (files[0]) {
        // Sciezka UTF-8 -> std::filesystem::path (bez strony kodowej ANSI).
        const std::filesystem::path p(reinterpret_cast<const char8_t*>(files[0]));
        controller->loadFile(p);
    }
    g_open.store(false);
}

} // namespace

void showOpenFileDialog(SDL_Window* window, AppController& controller)
{
    bool expected = false;
    if (!g_open.compare_exchange_strong(expected, true)) return;
    static const SDL_DialogFileFilter filters[] = {
        {"VGM (*.vgm, *.vgz)", "vgm;vgz"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(onSelected, &controller, window, filters,
                           static_cast<int>(sizeof(filters) / sizeof(filters[0])), nullptr, false);
}

} // namespace vgmplay::app
