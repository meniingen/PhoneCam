#include "Camera.h"
#include <ws2tcpip.h>
#include <softcam/softcam.h>
#include <vector>
#include <set>

namespace
{
    const wchar_t* kScid = L"2a2b3c4d";
    const int kPort = 27283;
    const wchar_t* kRemoteJar = L"/data/local/tmp/phonecam-server.jar";

    bool SocketReadExact(SOCKET socket, char* data, size_t length)
    {
        size_t received = 0;
        while (received < length)
        {
            int n = recv(socket, data + received, (int)(length - received), 0);
            if (n <= 0) return false;
            received += (size_t)n;
        }
        return true;
    }

    uint32_t ReadBe32(const unsigned char* data)
    {
        return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    }

    uint64_t ReadBe64(const unsigned char* data)
    {
        return ((uint64_t)ReadBe32(data) << 32) | ReadBe32(data + 4);
    }

    std::wstring RotationFilter(int rot)
    {
        switch (rot)
        {
        case 90:  return L"transpose=1,";
        case 180: return L"transpose=1,transpose=1,";
        case 270: return L"transpose=2,";
        default:  return L"";
        }
    }
}

bool Camera::Start(const CameraConfig& cfg)
{
    if (running_) return true;
    if (worker_.joinable()) worker_.join();
    stop_ = false;
    running_ = true;
    state_(true);
    worker_ = std::thread([this, cfg]() { Run(cfg); });
    return true;
}

bool Camera::SetCameraParam(unsigned char parameter, int value)
{
    std::lock_guard<std::mutex> lock(controlMx_);
    SOCKET s = controlSock_.load();
    if (s == INVALID_SOCKET) return false;
    unsigned char packet[6] = { 23, parameter, (unsigned char)(value >> 24), (unsigned char)(value >> 16), (unsigned char)(value >> 8), (unsigned char)value };
    size_t sent = 0;
    while (sent < sizeof(packet))
    {
        int n = send(s, (const char*)packet + sent, (int)(sizeof(packet) - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

bool Camera::SetTorch(bool enabled)
{
    std::lock_guard<std::mutex> lock(controlMx_);
    SOCKET s = controlSock_.load();
    if (s == INVALID_SOCKET) return false;
    const unsigned char packet[2] = { 18, (unsigned char)(enabled ? 1 : 0) };
    return send(s, (const char*)packet, sizeof(packet), 0) == sizeof(packet);
}

bool Camera::AdjustZoom(bool zoomIn, int steps)
{
    std::lock_guard<std::mutex> lock(controlMx_);
    SOCKET s = controlSock_.load();
    if (s == INVALID_SOCKET) return false;
    unsigned char type = (unsigned char)(zoomIn ? 19 : 20);
    for (int i = 0; i < steps; ++i) if (send(s, (const char*)&type, 1, 0) != 1) return false;
    return true;
}

void Camera::Stop()
{
    stop_ = true;
    SOCKET s = sock_.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) closesocket(s);
    { std::lock_guard<std::mutex> lock(controlMx_); SOCKET c = controlSock_.exchange(INVALID_SOCKET); if (c != INVALID_SOCKET) closesocket(c); }
    if (HANDLE h = ffmpegProc_.load()) TerminateProcess(h, 0);
    if (HANDLE h = serverProc_.load()) TerminateProcess(h, 0);
    if (worker_.joinable()) worker_.join();
}

std::wstring Camera::DetectVersion(const std::wstring& scrcpyExe, const std::wstring& dir)
{
    std::string out;
    if (!proc::RunCapture(L"\"" + scrcpyExe + L"\" --version", dir, out)) return L"";
    // "scrcpy 5.0.1 <https://...>"
    size_t p = out.find("scrcpy ");
    if (p == std::string::npos) return L"";
    p += 7;
    size_t e = p;
    while (e < out.size() && (isdigit((unsigned char)out[e]) || out[e] == '.')) ++e;
    return proc::ToWide(out.substr(p, e - p));
}

void Camera::Run(CameraConfig cfg)
{
    // Cache mellan körningar: version och redan pushad server (sparar tid vid omstart av kameran).
    static std::wstring cachedVersion;
    static std::set<std::string> pushedTo;

    const std::wstring root = proc::ExeDir() + L"\\tools";
    const std::wstring scDir = root + L"\\scrcpy";
    const std::wstring adb = scDir + L"\\adb.exe";
    const std::wstring scrcpyExe = scDir + L"\\scrcpy.exe";
    const std::wstring serverFile = scDir + L"\\scrcpy-server-camera-controls";
    const std::wstring ffmpeg = root + L"\\ffmpeg\\ffmpeg.exe";

    scCamera cam = nullptr;
    proc::Child srv, ff;
    std::thread reader;
    std::thread controlReader;
    std::thread serverLogReader;
    bool forwarded = false;
    const std::wstring serialArg = cfg.serial.empty() ? L"" : L" -s " + proc::ToWide(cfg.serial);
    const std::wstring adbBase = L"\"" + adb + L"\"" + serialArg;

    auto finish = [&]()
    {
        stop_ = true;
        SOCKET s = sock_.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) closesocket(s);
        { std::lock_guard<std::mutex> lock(controlMx_); SOCKET c = controlSock_.exchange(INVALID_SOCKET); if (c != INVALID_SOCKET) closesocket(c); }
        if (controlReader.joinable()) controlReader.join();
        ffmpegProc_ = nullptr;
        serverProc_ = nullptr;
        if (ff.process) TerminateProcess(ff.process, 0);
        if (srv.process) TerminateProcess(srv.process, 0);
        if (serverLogReader.joinable()) serverLogReader.join();
        if (reader.joinable()) reader.join();
        proc::Kill(ff);
        proc::Kill(srv);
        if (cam) scDeleteCamera(cam);
        if (forwarded)
        {
            std::string o;
            proc::RunCapture(adbBase + L" forward --remove tcp:" + std::to_wstring(kPort), scDir, o, 5000);
        }
        WSACleanup();
        running_ = false;
        state_(false);
        log_(L"[Kamera] stoppad");
    };

    for (const std::wstring& f : { adb, scrcpyExe, serverFile, ffmpeg })
    {
        if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            log_(L"[Kamera] saknas: " + f);
            finish();
            return;
        }
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (cachedVersion.empty()) cachedVersion = DetectVersion(scrcpyExe, scDir);
    const std::wstring version = cachedVersion;
    if (version.empty())
    {
        log_(L"[Kamera] kunde inte lasa scrcpy-version");
        finish();
        return;
    }

    // --- Starta mottagarsidan (virtuell kamera + ffmpeg) direkt, så den är klar när telefonen svarar.
    cam = scCreateCamera(cfg.width, cfg.height, 0);
    if (!cam)
    {
        log_(L"[Kamera] scCreateCamera misslyckades (instans finns redan eller softcam.dll ej registrerad)");
        finish();
        return;
    }

    const std::wstring W = std::to_wstring(cfg.width), H = std::to_wstring(cfg.height);
    const bool useHevc = cfg.width >= 7680 || cfg.height >= 4320;
    const std::wstring inputCodec = useHevc ? L"hevc" : L"h264";
    const std::wstring androidCodec = useHevc ? L"h265" : L"h264";
    std::wstring ffCmd =
        L"\"" + ffmpeg + L"\" -hide_banner -loglevel error -fflags nobuffer -flags low_delay";
    if (cfg.hardwareDecoding)
    {
        if (cfg.decoder == L"D3D11") ffCmd += L" -hwaccel d3d11va";
        else if (cfg.decoder == L"DXVA2") ffCmd += L" -hwaccel dxva2";
        else if (cfg.decoder == L"CUDA") ffCmd += L" -hwaccel cuda";
        else if (cfg.decoder == L"Intel QuickSync") ffCmd += L" -hwaccel qsv";
        else ffCmd += L" -hwaccel auto";
    }
    ffCmd +=
        L" -probesize 32 -analyzeduration 0 -use_wallclock_as_timestamps 1 -f " + inputCodec + L" -i pipe:0"
        L" -vf \"" + RotationFilter(cfg.rotation) + (cfg.mirror ? L"hflip," : L"") + (cfg.flip ? L"vflip," : L"") +
        (cfg.beautify ? L"hqdn3d=1.5:1.5:6:6," : L"") +
        L"scale=" + W + L":" + H + L":force_original_aspect_ratio=decrease,"
        L"pad=" + W + L":" + H + L":(ow-iw)/2:(oh-ih)/2:black,format=bgr24\""
        L" -fps_mode passthrough -flush_packets 1 -f rawvideo pipe:1";
    if (!proc::Spawn(ffCmd, root + L"\\ffmpeg", true, true, ff))
    {
        log_(L"[Kamera] kunde inte starta ffmpeg");
        finish();
        return;
    }
    ffmpegProc_ = ff.process;

    HANDLE ffOut = ff.out;
    const size_t frameSize = (size_t)cfg.width * cfg.height * 3;
    reader = std::thread([this, ffOut, frameSize, cam, fw = cfg.width, fh = cfg.height]()
    {
        std::vector<unsigned char> frame(frameSize);
        ULONGLONG lastPreview = 0;
        for (;;)
        {
            size_t got = 0;
            while (got < frameSize)
            {
                DWORD n = 0;
                if (!ReadFile(ffOut, frame.data() + got, (DWORD)(frameSize - got), &n, nullptr) || n == 0)
                    return;
                got += n;
            }
            scSendFrame(cam, frame.data());
            if (frame_)
            {
                ULONGLONG now = GetTickCount64();
                if (now - lastPreview >= 33) { lastPreview = now; frame_(frame.data(), fw, fh); }
            }
        }
    });

    // --- Telefonsidan
    std::string out;
    if (!pushedTo.count(cfg.serial))
    {
        if (!proc::RunCapture(adbBase + L" push \"" + serverFile + L"\" " + kRemoteJar, scDir, out))
        {
            log_(L"[Kamera] adb push misslyckades: " + proc::ToWide(out));
            finish();
            return;
        }
        pushedTo.insert(cfg.serial);
    }

    if (!proc::RunCapture(adbBase + L" forward tcp:" + std::to_wstring(kPort) +
                          L" localabstract:scrcpy_" + kScid, scDir, out))
    {
        log_(L"[Kamera] adb forward misslyckades: " + proc::ToWide(out));
        finish();
        return;
    }
    forwarded = true;

    std::wstring serverCmd =
        L"CLASSPATH=" + std::wstring(kRemoteJar) +
        L" app_process / com.genymobile.scrcpy.Server " + version +
        L" scid=" + kScid +
        L" log_level=error tunnel_forward=true raw_stream=true cleanup=false"
        L" video=true audio=false control=true max_fps=" + std::to_wstring(cfg.fps) +
        L" video_source=camera video_codec=" + androidCodec +
        L" camera_facing=" + proc::ToWide(cfg.facing) +
        L" camera_size=" + std::to_wstring(cfg.width) + L"x" + std::to_wstring(cfg.height) +
        L" camera_fps=" + std::to_wstring(cfg.fps) + L" camera_zoom=" + std::to_wstring(cfg.zoom / 10.0).substr(0, 4) +
        (cfg.highSpeed ? L" camera_high_speed=true" : L"");

    if (!proc::Spawn(adbBase + L" shell \"" + serverCmd + L"\"", scDir, false, true, srv, true))
    {
        log_(L"[Kamera] kunde inte starta servern");
        finish();
        return;
    }
    serverProc_ = srv.process;
    serverLogReader = std::thread([this, output = srv.out]()
    {
        char buffer[2048];
        std::string pending;
        DWORD count = 0;
        while (ReadFile(output, buffer, sizeof(buffer), &count, nullptr) && count)
        {
            pending.append(buffer, count);
            size_t newline;
            while ((newline = pending.find_first_of("\r\n")) != std::string::npos)
            {
                std::string line = pending.substr(0, newline);
                size_t skip = newline + 1;
                while (skip < pending.size() && (pending[skip] == '\r' || pending[skip] == '\n')) ++skip;
                pending.erase(0, skip);
                if (!line.empty()) log_(L"[Android] " + proc::ToWide(line));
            }
            if (pending.size() > 8192) { log_(L"[Android] " + proc::ToWide(pending.substr(0, 8192))); pending.erase(0, 8192); }
        }
        if (!pending.empty()) log_(L"[Android] " + proc::ToWide(pending));
    });

    // Anslut till servern (adb forward accepterar alltid, så vi provar tätt tills data kommer).
    SOCKET s = INVALID_SOCKET, control = INVALID_SOCKET;
    std::vector<char> buf(1 << 16);
    int first = 0;
    for (int attempt = 0; attempt < 300 && !stop_; ++attempt)
    {
        SOCKET t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)kPort);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(t, (sockaddr*)&addr, sizeof(addr)) == 0)
        {
            DWORD to = 3000;
            setsockopt(t, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
            SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (connect(c, (sockaddr*)&addr, sizeof(addr)) == 0)
            {
                int n = recv(t, buf.data(), (int)buf.size(), 0);
                if (n > 0) { s = t; control = c; first = n; break; }
            }
            closesocket(c);
        }
        closesocket(t);
        if (!proc::Alive(srv)) break;
        Sleep(40);
    }
    if (s == INVALID_SOCKET)
    {
        if (!stop_) log_(L"[Kamera] fick ingen video fran telefonen (Android 12+ kravs, tillat kamera/USB-felsokning)");
        finish();
        return;
    }
    DWORD noTimeout = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&noTimeout, sizeof(noTimeout));
    sock_ = s;
    controlSock_ = control;

    controlReader = std::thread([this, control]()
    {
        for (;;)
        {
            unsigned char header[3] = {};
            if (!SocketReadExact(control, (char*)header, sizeof(header))) return;
            const uint16_t size = (uint16_t)((header[1] << 8) | header[2]);
            if (header[0] != 3 || size != 52) return;
            unsigned char data[52] = {};
            if (!SocketReadExact(control, (char*)data, sizeof(data))) return;
            CameraStatus status;
            status.exposureNs = (int64_t)ReadBe64(data);
            int32_t* fields[] = { &status.iso, &status.focusMilli, &status.ev, &status.zoomTenths,
                &status.aeState, &status.afState, &status.awbState, &status.aeMode, &status.afMode,
                &status.awbMode, &status.whiteBalanceKelvin };
            for (int i = 0; i < 11; ++i) *fields[i] = (int32_t)ReadBe32(data + 8 + i * 4);
            if (status_) status_(status);
        }
    });

    // Initialize camera values from the advanced Settings page.
    // Camera2 servers may clamp or reject values outside the selected lens capability.
    SetCameraParam(0, cfg.focusAuto ? 3 : 0);
    if (cfg.focusAuto) SetCameraParam(5, 1); // start one autofocus pass on camera start
    if (!cfg.focusAuto) SetCameraParam(1, cfg.focus * 1000);
    SetCameraParam(2, cfg.exposureAuto ? 1 : 0);
    if (!cfg.exposureAuto) { SetCameraParam(3, 1000000000 / (cfg.shutterSpeed > 0 ? cfg.shutterSpeed : 1)); SetCameraParam(4, cfg.iso); }

    log_(L"[Kamera] igang - valj \"Softcam\" som kamera i appen");

    // Pump: socket -> ffmpeg stdin
    DWORD written = 0;
    if (!WriteFile(ff.in, buf.data(), first, &written, nullptr)) first = -1;
    while (first >= 0 && !stop_)
    {
        int n = recv(s, buf.data(), (int)buf.size(), 0);
        if (n <= 0) break;
        if (!WriteFile(ff.in, buf.data(), n, &written, nullptr)) break;
    }

    finish();
}
