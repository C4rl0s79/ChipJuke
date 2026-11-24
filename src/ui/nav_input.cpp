#include "vgmplay_ui/nav_input.h"

namespace vgmplay::ui {
namespace {

constexpr std::uint64_t kKeyboard = 1ull << 40;
constexpr std::uint64_t kPadButton = 2ull << 40;
constexpr std::uint64_t kPadStick = 3ull << 40;

std::uint64_t padKey(SDL_JoystickID id, unsigned code, std::uint64_t kind)
{
    return kind | (static_cast<std::uint64_t>(id) << 8) | code;
}

bool isRepeatable(Action a)
{
    switch (a) {
    case Action::Up: case Action::Down: case Action::Left: case Action::Right:
    case Action::PageUp: case Action::PageDown:
        return true;
    default:
        return false;
    }
}

// Przycisk pada -> akcja. Guide (i reszta) swiadomie niewiazane; LB/RB osobno.
bool mapPadButton(Uint8 b, Action& a)
{
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: a = Action::Up; return true;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: a = Action::Down; return true;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: a = Action::Left; return true;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: a = Action::Right; return true;
    case SDL_GAMEPAD_BUTTON_SOUTH: a = Action::Accept; return true;
    case SDL_GAMEPAD_BUTTON_EAST: a = Action::Back; return true;
    case SDL_GAMEPAD_BUTTON_START: a = Action::TogglePlay; return true;
    case SDL_GAMEPAD_BUTTON_WEST: a = Action::ToggleSolo; return true;      // X
    case SDL_GAMEPAD_BUTTON_NORTH: a = Action::ResetMix; return true;       // Y
    case SDL_GAMEPAD_BUTTON_BACK: a = Action::ToggleChannelsView; return true;
    default: return false;
    }
}

bool mapKey(SDL_Scancode sc, bool shift, Action& a, int& arg)
{
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) { // 1..9, 0 -> kanaly 0..9
        arg = static_cast<int>(sc - SDL_SCANCODE_1);
        a = shift ? Action::SoloChannel : Action::MuteChannel;
        return true;
    }
    switch (sc) {
    case SDL_SCANCODE_UP: a = Action::Up; return true;
    case SDL_SCANCODE_DOWN: a = Action::Down; return true;
    case SDL_SCANCODE_LEFT: a = Action::Left; return true;
    case SDL_SCANCODE_RIGHT: a = Action::Right; return true;
    case SDL_SCANCODE_PAGEUP: a = Action::PageUp; return true;
    case SDL_SCANCODE_PAGEDOWN: a = Action::PageDown; return true;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: a = Action::Accept; return true;
    case SDL_SCANCODE_ESCAPE: a = Action::Back; return true;
    case SDL_SCANCODE_SPACE: a = Action::TogglePlay; return true;
    case SDL_SCANCODE_F2: a = Action::ToggleLang; return true;
    case SDL_SCANCODE_F3: a = Action::ToggleGd3Lang; return true;
    case SDL_SCANCODE_F4: a = Action::ToggleChannelsView; return true;
    case SDL_SCANCODE_M: a = Action::ToggleMute; return true;
    case SDL_SCANCODE_S: a = Action::ToggleSolo; return true;
    case SDL_SCANCODE_U: a = Action::UnmuteAll; return true;
    case SDL_SCANCODE_C: a = Action::ClearSolo; return true;
    case SDL_SCANCODE_R: a = Action::ResetMix; return true;
    case SDL_SCANCODE_F12: a = Action::ToggleDiag; return true;
    default: return false; // Tab obslugiwany osobno
    }
}

} // namespace

void NavInput::setWindowFocused(bool focused)
{
    windowFocused_ = focused;
    // Zmiana fokusu okna: puszczenia moga nie dotrzec, wiec czyscimy caly stan.
    held_.clear();
    stick_.clear();
    events_.clear();
}

void NavInput::setFocus(Panel p)
{
    if (p == focus_) return;
    focus_ = p;
    for (auto& kv : held_) kv.second.stale = true;
}

void NavInput::cycleFocus(int dir)
{
    const int n = static_cast<int>(Panel::Count);
    setFocus(static_cast<Panel>((static_cast<int>(focus_) + dir + n) % n));
}

void NavInput::press(std::uint64_t key, Action a, std::uint64_t now, int arg)
{
    if (held_.count(key)) return; // powtorne "down" bez puszczenia
    held_[key] = Held{a, arg, isRepeatable(a), false, now + kRepeatDelayMs};
    events_.push_back({a, false, arg});
}

void NavInput::release(std::uint64_t key) { held_.erase(key); }

void NavInput::releaseAllForGamepad(SDL_JoystickID id)
{
    for (auto it = held_.begin(); it != held_.end();) {
        const std::uint64_t k = it->first;
        const bool pad = (k >> 40) == 2 || (k >> 40) == 3;
        if (pad && ((k >> 8) & 0xFFFFFFFFull) == id) it = held_.erase(it);
        else ++it;
    }
    for (auto it = stick_.begin(); it != stick_.end();) {
        if (((it->first >> 8) & 0xFFFFFFFFull) == id) it = stick_.erase(it);
        else ++it;
    }
}

void NavInput::handleStick(SDL_JoystickID id, Uint8 axis, Sint16 value, std::uint64_t now)
{
    if (axis != SDL_GAMEPAD_AXIS_LEFTX && axis != SDL_GAMEPAD_AXIS_LEFTY) return;
    const std::uint64_t sk = padKey(id, axis, kPadStick);
    int& state = stick_[sk]; // histereza: 16000 wlacza, 9000 wylacza
    int next = state;
    if (value > 16000) next = 1;
    else if (value < -16000) next = -1;
    else if (value > -9000 && value < 9000) next = 0;
    if (next == state) return;
    if (state != 0) release(sk);
    state = next;
    if (next != 0) {
        const bool horiz = axis == SDL_GAMEPAD_AXIS_LEFTX;
        const Action a = horiz ? (next > 0 ? Action::Right : Action::Left)
                               : (next > 0 ? Action::Down : Action::Up);
        press(sk, a, now);
    }
}

void NavInput::handleEvent(const SDL_Event& e, std::uint64_t now)
{
    switch (e.type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED: setWindowFocused(true); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: setWindowFocused(false); break;

    case SDL_EVENT_KEY_DOWN: {
        if (e.key.repeat || !windowFocused_) break; // powtorzenia robimy sami
        const std::uint64_t key = kKeyboard | e.key.scancode;
        if (e.key.scancode == SDL_SCANCODE_TAB) {
            if (held_.count(key)) break;
            cycleFocus((e.key.mod & SDL_KMOD_SHIFT) ? -1 : 1);
            held_[key] = Held{Action::Accept, 0, false, true, 0}; // wpis tylko do sledzenia puszczenia
            break;
        }
        Action a;
        int arg = 0;
        if (mapKey(e.key.scancode, (e.key.mod & SDL_KMOD_SHIFT) != 0, a, arg)) press(key, a, now, arg);
        break;
    }
    case SDL_EVENT_KEY_UP: // puszczenie zawsze, bez warunku na fokus
        release(kKeyboard | e.key.scancode);
        break;

    case SDL_EVENT_GAMEPAD_ADDED: // hotplug
        SDL_OpenGamepad(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        releaseAllForGamepad(e.gdevice.which);
        if (SDL_Gamepad* g = SDL_GetGamepadFromID(e.gdevice.which)) SDL_CloseGamepad(g);
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        if (!windowFocused_) break; // pad nie steruje aplikacja bez fokusu okna
        const Uint8 b = e.gbutton.button;
        const std::uint64_t key = padKey(e.gbutton.which, b, kPadButton);
        if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
            if (held_.count(key)) break;
            cycleFocus(b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ? -1 : 1);
            held_[key] = Held{Action::Accept, 0, false, true, 0};
            break;
        }
        Action a;
        if (mapPadButton(b, a)) press(key, a, now);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        release(padKey(e.gbutton.which, e.gbutton.button, kPadButton));
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (windowFocused_) handleStick(e.gaxis.which, e.gaxis.axis, e.gaxis.value, now);
        break;
    default: break;
    }
}

void NavInput::update(std::uint64_t now)
{
    if (!windowFocused_) return;
    for (auto& kv : held_) {
        Held& h = kv.second;
        if (h.stale || !h.repeatable || now < h.nextRepeat) continue;
        events_.push_back({h.action, true, h.arg});
        h.nextRepeat = now + kRepeatRateMs;
    }
}

} // namespace vgmplay::ui
