// vgmplay - okno aplikacji: SDL3 + Dear ImGui (backend SDL3 + DX11) + AppController.
// main.cpp zajmuje sie oknem, zdarzeniami SDL i petla klatek; logika (silnik, audio, ladowanie)
// jest w AppController, a ekrany (PlayerScreen) widza tylko PlayerView i PlayerActions.
#include "app_controller.h"
#include "app_resources.h"
#include "file_dialog.h"

#include "vgmplay_ui/nav_input.h"
#include "vgmplay_ui/player_screen.h"
#include "vgmplay_ui/strings.h"
#include "vgmplay_ui/theme.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_sdl3.h>

#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

struct D3D {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
    bool occluded = false;
};

void createRenderTarget(D3D& d)
{
    ComPtr<ID3D11Texture2D> back;
    if (SUCCEEDED(d.swapChain->GetBuffer(0, IID_PPV_ARGS(&back))))
        d.device->CreateRenderTargetView(back.Get(), nullptr, &d.rtv);
}

bool createDevice(D3D& d, HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    UINT flags = 0;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    auto tryCreate = [&](D3D_DRIVER_TYPE type, UINT f) {
        return D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, f, levels, 2, D3D11_SDK_VERSION,
                                             &sd, &d.swapChain, &d.device, &got, &d.context);
    };
    HRESULT hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, flags);
    if (FAILED(hr) && flags) hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, 0); // brak warstwy debug
    if (FAILED(hr)) hr = tryCreate(D3D_DRIVER_TYPE_WARP, 0);
    if (FAILED(hr)) return false;
    // Bez obslugi Alt+Enter przez DXGI (okno zarzadzane przez SDL).
    ComPtr<IDXGIFactory> factory;
    if (SUCCEEDED(d.swapChain->GetParent(IID_PPV_ARGS(&factory))))
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    createRenderTarget(d);
    return true;
}

void resizeSwapChain(D3D& d, UINT w, UINT h)
{
    if (!d.swapChain || w == 0 || h == 0) return;
    d.rtv.Reset();
    d.swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget(d);
}

// Font osadzony jako zasob RCDATA (jeden przenosny plik exe).
const void* loadFontResource(int& size)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_FONT_UI), reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!r) return nullptr;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return nullptr;
    size = static_cast<int>(SizeofResource(nullptr, r));
    return LockResource(g);
}

struct CommandLine {
    std::optional<std::filesystem::path> file;
    bool pl = false;
    bool nullAudio = false;
    bool noAutoplay = false;
    bool diag = false;
    bool fakeChannelScopes = false;
    bool fakePattern = false;
    int scopeStrip = 0;              // 0 = maly, 1 = duzy, 2 = ukryty
    unsigned rowTicks = 2205;
};

// Argumenty z GetCommandLineW (UTF-16): nazwy plikow spoza strony kodowej ANSI dzialaja poprawnie.
CommandLine parseCommandLine()
{
    CommandLine cl;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return cl;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--pl") cl.pl = true;
        else if (a == L"--null-audio") cl.nullAudio = true;       // bez dzwieku (testy/CI)
        else if (a == L"--no-autoplay") cl.noAutoplay = true;     // po zaladowaniu: Stopped
        else if (a == L"--diag") cl.diag = true;                  // start z diagnostyka (jak F12)
        else if (a == L"--fake-channel-scopes") cl.fakeChannelScopes = true; // DIAGNOSTYKA: syntetyczne przebiegi kanalow
        else if (a == L"--fake-pattern") cl.fakePattern = true;   // DIAGNOSTYKA: sztuczne nuty zamiast chipstate
        else if (a == L"--scopes-large") cl.scopeStrip = 1;       // start z duzym paskiem oscyloskopow
        else if (a == L"--scopes-hidden") cl.scopeStrip = 2;      // start bez paska oscyloskopow
        else if (a.rfind(L"--row-ticks=", 0) == 0) cl.rowTicks = static_cast<unsigned>(std::max(1, _wtoi(a.c_str() + 12)));
        else if (a.rfind(L"--", 0) != 0 && !cl.file) cl.file = std::filesystem::path(a);
    }
    LocalFree(argv);
    return cl;
}

} // namespace

int main()
{
    const CommandLine cl = parseCommandLine();
    if (cl.pl) vgmplay::ui::setLang(vgmplay::ui::Lang::Pl);

    // Pad nie ma sterowac aplikacja, gdy okno nie ma fokusu (decyzja projektowa).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    const float scale = [] {
        const float s = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        return s > 0.0f ? s : 1.0f;
    }();
    SDL_Window* window = SDL_CreateWindow("vgmplay", static_cast<int>(1280 * scale),
                                          static_cast<int>(720 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(window, static_cast<int>(800 * scale), static_cast<int>(480 * scale));

    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                         SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    D3D d3d;
    if (!hwnd || !createDevice(d3d, hwnd)) {
        std::fprintf(stderr, "Nie udalo sie utworzyc urzadzenia D3D11\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // bez zapisu ukladu okien (ustawienia: E3)

    const float dpi = SDL_GetWindowDisplayScale(window);
    int fontSize = 0;
    const void* fontData = loadFontResource(fontSize);
    vgmplay::ui::installFont(io, fontData, fontSize);
    vgmplay::ui::applyTheme(dpi > 0.0f ? dpi : 1.0f);

    ImGui_ImplSDL3_InitForOther(window);
    ImGui_ImplDX11_Init(d3d.device.Get(), d3d.context.Get());

    // Zdarzenie budzace petle UI (np. koniec ladowania na watku workera).
    const Uint32 wakeEvent = SDL_RegisterEvents(1);

    vgmplay::app::ControllerOptions options;
    options.nullAudio = cl.nullAudio;
    options.autoplay = !cl.noAutoplay;
    options.fakeChannelScopes = cl.fakeChannelScopes;
    options.fakePattern = cl.fakePattern;
    options.rowTicks = cl.rowTicks;
    auto controller = std::make_unique<vgmplay::app::AppController>(options);
    controller->setWakeCallback([wakeEvent] {
        SDL_Event e{};
        e.type = wakeEvent;
        SDL_PushEvent(&e);
    });
    controller->setOpenDialogHandler(
        [&] { vgmplay::app::showOpenFileDialog(window, *controller); });
    controller->start();
    if (!controller->view().audioError.empty())
        std::fprintf(stderr, "audio: %s\n", controller->view().audioError.c_str());
    if (cl.file) controller->loadFile(*cl.file);

    vgmplay::ui::NavInput nav;
    vgmplay::ui::PlayerScreen screen;
    screen.setDiagnosticsVisible(cl.diag);
    screen.setScopeStrip(cl.scopeStrip == 1 ? vgmplay::ui::ScopeStrip::Large
                         : cl.scopeStrip == 2 ? vgmplay::ui::ScopeStrip::Hidden
                                              : vgmplay::ui::ScopeStrip::Small);

    bool running = true;
    bool minimized = false;
    int settle = 12; // po zdarzeniu rysujemy jeszcze kilka klatek (hover, stany ImGui, odpowiedz silnika)
    std::uint64_t fpsStart = SDL_GetTicks();
    int fpsFrames = 0;
    float fps = 0.0f;

    auto handle = [&](SDL_Event& e) {
        ImGui_ImplSDL3_ProcessEvent(&e);
        nav.handleEvent(e, SDL_GetTicks());
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running = false; break;
        case SDL_EVENT_WINDOW_MINIMIZED: minimized = true; break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN: minimized = false; break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            resizeSwapChain(d3d, static_cast<UINT>(w), static_cast<UINT>(h));
            break;
        }
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            vgmplay::ui::applyTheme(SDL_GetWindowDisplayScale(window));
            break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data) {
                // UTF-8 -> sciezka (bez strony kodowej ANSI).
                controller->loadFile(std::filesystem::path(reinterpret_cast<const char8_t*>(e.drop.data)));
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.scancode == SDL_SCANCODE_O && (e.key.mod & SDL_KMOD_CTRL) && !e.key.repeat)
                controller->requestOpenFile();
            break;
        default: break;
        }
        settle = 12;
    };

    while (running) {
        const bool animating = screen.wantsAnimation(controller->view()) || nav.anyHeld() || settle > 0;
        SDL_Event e;
        if (minimized) {
            if (SDL_WaitEventTimeout(&e, 250)) handle(e);
        } else if (!animating) {
            // Tryb bezczynnosci: spimy do zdarzenia lub 500 ms (ok. 2 kl/s).
            if (SDL_WaitEventTimeout(&e, 500)) handle(e);
        }
        while (SDL_PollEvent(&e)) handle(e);
        if (!running) break;

        controller->pump(); // takze w minimalizacji: zdarzenia silnika i wyniki ladowania
        if (minimized) {
            controller->setChannelScopesVisible(false); // ukryte okno: silnik nie liczy strumieni kanalow
            continue;
        }

        const std::uint64_t now = SDL_GetTicks();
        nav.update(now);

        const bool idle = !(screen.wantsAnimation(controller->view()) || nav.anyHeld() || settle > 0);
        if (settle > 0) --settle;

        if (d3d.occluded) {
            if (d3d.swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                SDL_Delay(100);
                continue;
            }
            d3d.occluded = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        screen.frame(nav, controller->view(), *controller, {fps, idle});
        ImGui::Render();

        const ImVec4& bg = vgmplay::ui::palette().window;
        const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
        ID3D11RenderTargetView* rtv = d3d.rtv.Get();
        d3d.context->OMSetRenderTargets(1, &rtv, nullptr);
        d3d.context->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT hr = d3d.swapChain->Present(1, 0); // vsync
        d3d.occluded = (hr == DXGI_STATUS_OCCLUDED);

        ++fpsFrames;
        const std::uint64_t t = SDL_GetTicks();
        if (t - fpsStart >= 1000) {
            fps = static_cast<float>(fpsFrames) * 1000.0f / static_cast<float>(t - fpsStart);
            fpsFrames = 0;
            fpsStart = t;
        }
    }

    controller.reset(); // zatrzymuje watki silnika i workera oraz urzadzenie audio
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    d3d = D3D{};
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
