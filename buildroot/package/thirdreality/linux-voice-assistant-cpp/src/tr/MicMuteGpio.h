
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace lva::tr {

enum class MuteChangeSource { InitialRead, Hardware };

class MicMuteGpio {
   public:
    using ChangeCallback =
        std::function<void(bool muted, MuteChangeSource source)>;

    explicit MicMuteGpio(ChangeCallback on_change,
                         std::string gpio_path = "/sys/class/gpio/gpio438/value");

    void Poll();

    bool ReadAndApplyOnce();

    void SyncToHardware(bool muted);

    bool Available() const noexcept { return available_; }

   private:
    bool ReadRaw(int* out_value);

    ChangeCallback            on_change_;
    std::string               gpio_path_;
    bool                      available_  = false;
    int                       last_value_ = -1;  // last GPIO digit seen
    std::chrono::steady_clock::time_point last_poll_{};
};

}  // namespace lva::tr
