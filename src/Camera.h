#pragma once
#include "Process.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <winsock2.h>
#include <cstdint>

struct CameraStatus
{
    int64_t exposureNs = 0;
    int32_t iso = 0, focusMilli = 0, ev = 0, zoomTenths = 10;
    int32_t aeState = -1, afState = -1, awbState = -1;
    int32_t aeMode = -1, afMode = -1, awbMode = -1, whiteBalanceKelvin = 5000;
};

struct CameraConfig
{
    std::string serial;
    std::string facing = "back";   // back | front
    int width = 1280;              // måste vara multipel av 4
    int height = 720;
    int fps = 30;
    int rotation = 0;              // 0 | 90 | 180 | 270
    bool mirror = false;
    bool flip = false;
    bool beautify = false;
    int shutterSpeed = 300, iso = 6400, focus = 16, zoom = 6;
    bool exposureAuto = false, focusAuto = false;
    bool whiteBalanceAuto = false, highSpeed = false, hardwareDecoding = false;
    int ev = 0, whiteBalanceKelvin = 5000;
    std::wstring decoder = L"Default";
};

// Livebild från mobilkameran -> softcam (virtuell webbkamera).
// Kedja: scrcpy-server (video_source=camera, raw H.264) -> adb forward -> ffmpeg (avkodning) -> scSendFrame.
class Camera
{
public:
    using StateFn = std::function<void(bool running)>;
    using FrameFn = std::function<void(const unsigned char* bgr, int width, int height)>;
    using StatusFn = std::function<void(const CameraStatus&)>;

    Camera(LogFn log, StateFn state, FrameFn frame = nullptr, StatusFn status = nullptr) : log_(log), state_(state), frame_(frame), status_(status) {}
    ~Camera() { Stop(); }

    bool Start(const CameraConfig& cfg);
    void Stop();
    bool SetCameraParam(unsigned char parameter, int value);
    bool SetTorch(bool enabled);
    bool AdjustZoom(bool zoomIn, int steps = 1);
    bool Running() const { return running_; }

private:
    void Run(CameraConfig cfg);
    std::wstring DetectVersion(const std::wstring& scrcpyExe, const std::wstring& dir);

    LogFn log_;
    StateFn state_;
    FrameFn frame_;
    StatusFn status_;
    std::thread worker_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<SOCKET> sock_{ INVALID_SOCKET };
    std::atomic<SOCKET> controlSock_{ INVALID_SOCKET };
    std::mutex controlMx_;
    std::atomic<HANDLE> ffmpegProc_{ nullptr };
    std::atomic<HANDLE> serverProc_{ nullptr };
};
