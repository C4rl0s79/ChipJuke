#include "file_dialog.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace vgmplay::app {
namespace {

std::atomic<bool> g_open{false};

enum class Kind { Files, Folder, Import, Export };
struct Target {
    AppController* controller;
    Kind kind;
};

std::filesystem::path toPath(const char* utf8)
{
    // Sciezka UTF-8 -> std::filesystem::path (bez strony kodowej ANSI).
    return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8));
}

void SDLCALL onSelected(void* userdata, const char* const* files, int /*filter*/)
{
    const Target* t = static_cast<const Target*>(userdata);
    if (!files) {
        std::fprintf(stderr, "SDL dialog: %s\n", SDL_GetError());
    } else if (files[0]) {
        switch (t->kind) {
        case Kind::Files:
        case Kind::Folder: {
            std::vector<std::filesystem::path> paths;
            for (int i = 0; files[i]; ++i) paths.push_back(toPath(files[i]));
            t->controller->submitPaths(std::move(paths));
            break;
        }
        case Kind::Import: t->controller->submitImportM3u(toPath(files[0])); break;
        case Kind::Export: t->controller->submitExportM3u(toPath(files[0])); break;
        }
    }
    g_open.store(false);
}

bool begin()
{
    bool expected = false;
    return g_open.compare_exchange_strong(expected, true);
}

} // namespace

void showOpenFileDialog(SDL_Window* window, AppController& controller)
{
    if (!begin()) return;
    static Target target;
    target = {&controller, Kind::Files};
    static const SDL_DialogFileFilter filters[] = {
        {"VGM (*.vgm, *.vgz)", "vgm;vgz"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(onSelected, &target, window, filters,
                           static_cast<int>(sizeof(filters) / sizeof(filters[0])), nullptr, true);
}

void showOpenFolderDialog(SDL_Window* window, AppController& controller)
{
    if (!begin()) return;
    static Target target;
    target = {&controller, Kind::Folder};
    SDL_ShowOpenFolderDialog(onSelected, &target, window, nullptr, false);
}

void showImportM3uDialog(SDL_Window* window, AppController& controller)
{
    if (!begin()) return;
    static Target target;
    target = {&controller, Kind::Import};
    static const SDL_DialogFileFilter filters[] = {
        {"M3U / M3U8", "m3u;m3u8"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(onSelected, &target, window, filters,
                           static_cast<int>(sizeof(filters) / sizeof(filters[0])), nullptr, false);
}

void showExportM3uDialog(SDL_Window* window, AppController& controller)
{
    if (!begin()) return;
    static Target target;
    target = {&controller, Kind::Export};
    static const SDL_DialogFileFilter filters[] = {
        {"M3U8 (UTF-8)", "m3u8"},
        {"M3U", "m3u"},
    };
    SDL_ShowSaveFileDialog(onSelected, &target, window, filters,
                           static_cast<int>(sizeof(filters) / sizeof(filters[0])), "playlist.m3u8");
}

} // namespace vgmplay::app
