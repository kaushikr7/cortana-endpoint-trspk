#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>

namespace lva::tr {

enum class LedState {
    Idle,
    Ready,
    Booting,
    Reconnecting,
    Listening,
    Thinking,
    Speaking,
    Error,
    Updating,
    Blocked,
    Volume,
};

struct LedPresentation {
    std::string_view animation;
    bool return_to_idle;
};

class LedController {
public:
    using Clock = std::chrono::steady_clock;
    using RenderFn = std::function<void(const LedPresentation&)>;

    explicit LedController(RenderFn render = {});

    void SetBase(LedState state);
    void SetConnection(LedState state);
    void ClearConnection();
    void SetTurn(LedState state);
    void ClearTurn();
    void SetSystem(LedState state);
    void ClearSystem();
    void SetBlocked(bool blocked);

    void ShowVolumeChanged(
        std::chrono::milliseconds duration = std::chrono::milliseconds(1200),
        Clock::time_point now = Clock::now());
    void Poll(Clock::time_point now = Clock::now());

    std::optional<LedState> EffectiveState() const;

    static LedPresentation PresentationFor(LedState state);

private:
    enum class Layer : std::size_t {
        Base = 0,
        Overlay,
        Connection,
        Turn,
        System,
        Blocked,
        Count,
    };

    void SetLayer(Layer layer, LedState state);
    void ClearLayer(Layer layer);
    std::optional<LedState> EffectiveStateLocked() const;
    void RenderIfChangedLocked();

    RenderFn render_;
    mutable std::mutex mutex_;
    std::array<std::optional<LedState>,
               static_cast<std::size_t>(Layer::Count)> layers_{};
    std::optional<LedState> last_rendered_;
    std::optional<Clock::time_point> overlay_expires_at_;
};

}  // namespace lva::tr
