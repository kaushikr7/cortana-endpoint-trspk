#include "tr/LedController.h"

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <utility>

#include "util/Log.h"

extern char** environ;

namespace lva::tr {

namespace {

constexpr const char* kTag = "led";
constexpr const char* kAnimationDirectory =
    "/usr/share/thirdreality/animation/";

void SpawnAnimation(const LedPresentation& presentation) {
    const std::string animation_path =
        std::string(kAnimationDirectory) + std::string(presentation.animation);
    const std::string array_argument =
        std::string("array:string:") + animation_path;

    char program[] = "/usr/bin/dbus-send";
    char system_argument[] = "--system";
    char type_argument[] = "--type=signal";
    char object_path[] = "/com/3r/EventBus";
    char interface_name[] = "com._3reality.EventBus.LedShow";
    char return_to_idle[] = "boolean:false";
    if (presentation.return_to_idle) {
        std::strcpy(return_to_idle, "boolean:true");
    }
    std::string mutable_array_argument = array_argument;

    char* arguments[] = {
        program,
        system_argument,
        type_argument,
        object_path,
        interface_name,
        return_to_idle,
        mutable_array_argument.data(),
        nullptr,
    };

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(
        &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(
        &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = 0;
    const int result = ::posix_spawn(
        &pid, program, &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        LVA_LOGW(kTag, "posix_spawn failed: %s", std::strerror(result));
        return;
    }
    LVA_LOGD(kTag, "show '%.*s' (pid=%d)",
             static_cast<int>(presentation.animation.size()),
             presentation.animation.data(), static_cast<int>(pid));
}

}  // namespace

LedController::LedController(RenderFn render)
    : render_(render ? std::move(render) : RenderFn(SpawnAnimation)) {}

LedPresentation LedController::PresentationFor(LedState state) {
    switch (state) {
        case LedState::Idle:
            return {"active-ending.animation", true};
        case LedState::Ready:
            return {"none.animation", true};
        case LedState::Booting:
            return {"active-thinking.animation", false};
        case LedState::Reconnecting:
            return {"alert-short.animation", false};
        case LedState::Listening:
            return {"active-waking.animation", false};
        case LedState::Thinking:
            return {"active-thinking.animation", false};
        case LedState::Speaking:
            return {"active-talking.animation", false};
        case LedState::Cancelling:
            return {"active-ending.animation", true};
        case LedState::Error:
            return {"error.animation", true};
        case LedState::Updating:
            return {"ntf_queued.animation", false};
        case LedState::Blocked:
            return {"red.animation", false};
        case LedState::Volume:
            return {"volume-changed.animation", false};
    }
    return {"error.animation", true};
}

void LedController::SetBase(LedState state) {
    SetLayer(Layer::Base, state);
}

void LedController::SetConnection(LedState state) {
    SetLayer(Layer::Connection, state);
}

void LedController::ClearConnection() {
    ClearLayer(Layer::Connection);
}

void LedController::SetTurn(LedState state) {
    SetLayer(Layer::Turn, state);
}

void LedController::ClearTurn() {
    ClearLayer(Layer::Turn);
}

void LedController::SetSystem(LedState state) {
    SetLayer(Layer::System, state);
}

void LedController::ClearSystem() {
    ClearLayer(Layer::System);
}

void LedController::SetBlocked(bool blocked) {
    if (blocked) {
        SetLayer(Layer::Blocked, LedState::Blocked);
    } else {
        ClearLayer(Layer::Blocked);
    }
}

void LedController::ShowVolumeChanged(std::chrono::milliseconds duration,
                                      Clock::time_point now) {
    std::lock_guard lock(mutex_);
    layers_[static_cast<std::size_t>(Layer::Overlay)] = LedState::Volume;
    overlay_expires_at_ = now + duration;
    RenderIfChangedLocked();
}

void LedController::Poll(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (!overlay_expires_at_.has_value() || now < *overlay_expires_at_) {
        return;
    }
    overlay_expires_at_.reset();
    layers_[static_cast<std::size_t>(Layer::Overlay)].reset();
    RenderIfChangedLocked();
}

std::optional<LedState> LedController::EffectiveState() const {
    std::lock_guard lock(mutex_);
    return EffectiveStateLocked();
}

void LedController::SetLayer(Layer layer, LedState state) {
    std::lock_guard lock(mutex_);
    layers_[static_cast<std::size_t>(layer)] = state;
    RenderIfChangedLocked();
}

void LedController::ClearLayer(Layer layer) {
    std::lock_guard lock(mutex_);
    layers_[static_cast<std::size_t>(layer)].reset();
    if (layer == Layer::Overlay) overlay_expires_at_.reset();
    RenderIfChangedLocked();
}

std::optional<LedState> LedController::EffectiveStateLocked() const {
    for (std::size_t index = layers_.size(); index > 0; --index) {
        if (layers_[index - 1].has_value()) return layers_[index - 1];
    }
    return std::nullopt;
}

void LedController::RenderIfChangedLocked() {
    const auto effective = EffectiveStateLocked();
    if (effective == last_rendered_) return;
    last_rendered_ = effective;
    if (effective.has_value()) render_(PresentationFor(*effective));
}

}  // namespace lva::tr
