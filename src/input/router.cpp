// InputRouter: zrodla fizyczne -> akcje, z wlasnoscia nacisniec, zatrzaskiem przy zmianie ekranu i
// auto-repeatem kierunkowych. Zasady 1-6: include/vgmplay/input.hpp.
#include "vgmplay/input.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace vgmplay::input {

bool isDirectional(Action a) noexcept
{
    switch (a) {
    case Action::Up: case Action::Down: case Action::Left: case Action::Right:
    case Action::PageUp: case Action::PageDown:
        return true;
    default:
        return false;
    }
}

void purgePresses(std::vector<ActionEvent>& queue)
{
    std::erase_if(queue, [](const ActionEvent& e) { return e.phase != Phase::Released; });
}

namespace {

constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count_);

struct ActionState {
    int sources = 0;             // ile zrodel tej akcji jest trzymanych
    ScreenId owner = ScreenId::Player;
    bool latched = false;        // ekran zmienil sie podczas trzymania (zasada 2)
    std::uint64_t nextRepeat = 0;
};

struct HeldSource {
    BindingSource source;
    std::vector<Action> actions; // akcje przypisane w chwili wcisniecia (puszczenie jest spojne mimo zmiany wiazan)
};

} // namespace

struct InputRouter::Impl {
    std::vector<Binding> bindings;
    std::vector<HeldSource> held;
    std::array<ActionState, kActionCount> state{};
    ScreenId screen = ScreenId::Player;
    std::uint32_t delayMs = 400;
    std::uint32_t rateMs = 60;

    HeldSource* findHeld(const BindingSource& s)
    {
        for (HeldSource& h : held)
            if (h.source == s) return &h;
        return nullptr;
    }

    void releaseAction(Action a, std::uint64_t t, std::vector<ActionEvent>& out)
    {
        ActionState& st = state[static_cast<std::size_t>(a)];
        if (st.sources <= 0) return;
        if (--st.sources > 0) return;
        out.push_back({a, Phase::Released, st.owner, t});
        st = ActionState{};
    }
};

InputRouter::InputRouter() : impl_(new Impl) {}
InputRouter::~InputRouter() { delete impl_; }

void InputRouter::setBindings(std::span<const Binding> bindings)
{
    impl_->bindings.assign(bindings.begin(), bindings.end());
}

void InputRouter::setRepeat(std::uint32_t delayMs, std::uint32_t rateMs)
{
    impl_->delayMs = delayMs;
    impl_->rateMs = std::max<std::uint32_t>(1, rateMs);
}

void InputRouter::setContext(ScreenId screen)
{
    Impl& m = *impl_;
    if (screen == m.screen) return;
    m.screen = screen;
    for (ActionState& st : m.state)
        if (st.sources > 0) st.latched = true;   // zasada 2: nowy ekran czeka na puszczenie
}

ScreenId InputRouter::context() const noexcept { return impl_->screen; }

bool InputRouter::anyHeld() const noexcept { return !impl_->held.empty(); }

void InputRouter::consume(const RawInput& in, std::vector<ActionEvent>& out)
{
    Impl& m = *impl_;
    if (in.down) {
        if (m.findHeld(in.source)) return;   // powtorne "down" bez puszczenia (np. repeat systemowy)
        HeldSource h;
        h.source = in.source;
        for (const Binding& b : m.bindings) {
            if (b.source != in.source || b.action == Action::None || b.action >= Action::Count_) continue;
            if (std::find(h.actions.begin(), h.actions.end(), b.action) == h.actions.end()) h.actions.push_back(b.action);
        }
        if (h.actions.empty()) return;       // nieprzypisane zrodlo: nic nie sledzimy
        for (Action a : h.actions) {
            ActionState& st = m.state[static_cast<std::size_t>(a)];
            if (st.sources++ == 0) {         // pierwsze zrodlo akcji = nowe nacisniecie nalezace do biezacego ekranu
                st.owner = m.screen;
                st.latched = false;
                st.nextRepeat = in.timeMs + m.delayMs;
                out.push_back({a, Phase::Pressed, m.screen, in.timeMs});
            }
        }
        m.held.push_back(std::move(h));
    } else {
        const auto it = std::find_if(m.held.begin(), m.held.end(),
                                     [&](const HeldSource& h) { return h.source == in.source; });
        if (it == m.held.end()) return;      // puszczenie nieznanego zrodla (np. po onFocusLost)
        const std::vector<Action> actions = std::move(it->actions);
        m.held.erase(it);
        for (Action a : actions) m.releaseAction(a, in.timeMs, out);
    }
}

void InputRouter::tick(std::uint64_t nowMs, std::vector<ActionEvent>& out)
{
    Impl& m = *impl_;
    for (std::size_t i = 1; i < kActionCount; ++i) {
        const Action a = static_cast<Action>(i);
        ActionState& st = m.state[i];
        if (st.sources <= 0 || st.latched || !isDirectional(a) || nowMs < st.nextRepeat) continue;
        out.push_back({a, Phase::Repeat, st.owner, nowMs});
        st.nextRepeat = nowMs + m.rateMs;
    }
}

void InputRouter::onFocusLost(std::uint64_t nowMs, std::vector<ActionEvent>& out)
{
    Impl& m = *impl_;
    m.held.clear();
    for (std::size_t i = 1; i < kActionCount; ++i) {
        ActionState& st = m.state[i];
        if (st.sources > 0) out.push_back({static_cast<Action>(i), Phase::Released, st.owner, nowMs});
        st = ActionState{};
    }
}

} // namespace vgmplay::input
