
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace lva::tr {

enum class HomeButtonPress { Single, Double, Triple };

class HomeButton {
   public:
    using PressCallback = std::function<void(HomeButtonPress)>;

    struct Options {
        std::string input_device  = "/dev/input/event0";
    };

    HomeButton(const Options& opts, PressCallback on_press);
    ~HomeButton();

    HomeButton(const HomeButton&)            = delete;
    HomeButton& operator=(const HomeButton&) = delete;

    int Start();

    void OnMainLoopWake();

    void Stop();

    static HomeButtonPress ClassifyClicks(int clicks);

   private:
    void ThreadLoop();

    Options                 opts_;
    PressCallback           on_press_;

    int                     event_fd_       = -1;   // worker → main
    std::thread             thread_;
    std::atomic<bool>       stop_requested_{false};
    std::atomic<int>        pending_clicks_{0};      // 1/2/3
};

}  // namespace lva::tr
