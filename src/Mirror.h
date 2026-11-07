#pragma once
#include "Process.h"
#include <thread>
#include <atomic>

// Skärmspegling + styrning via scrcpy.exe (inget kamerarelaterat här).
// Hittar scrcpy-fönstret så att huvudprogrammet kan bädda in det.
class Mirror
{
public:
    using StateFn = std::function<void(bool running)>;
    using WindowFn = std::function<void(HWND)>;

    Mirror(LogFn log, StateFn state, WindowFn window) : log_(log), state_(state), window_(window) {}
    ~Mirror() { Stop(); }

    bool Start(const std::string& serial);
    void Stop();
    bool Running() const { return running_; }

private:
    LogFn log_;
    StateFn state_;
    WindowFn window_;
    proc::Child child_;
    std::thread watcher_;
    std::atomic<bool> running_{ false };
};
