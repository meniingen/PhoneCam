#include "Mirror.h"

namespace
{
    struct FindCtx { DWORD pid; HWND found; };

    BOOL CALLBACK EnumProc(HWND h, LPARAM l)
    {
        FindCtx* c = (FindCtx*)l;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == c->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr)
        {
            RECT r;
            GetWindowRect(h, &r);
            if (r.right - r.left > 50 && r.bottom - r.top > 50)
            {
                c->found = h;
                return FALSE;
            }
        }
        return TRUE;
    }

    HWND FindWindowOfPid(DWORD pid)
    {
        FindCtx c = { pid, nullptr };
        EnumWindows(EnumProc, (LPARAM)&c);
        return c.found;
    }
}

bool Mirror::Start(const std::string& serial)
{
    if (running_) return true;
    if (watcher_.joinable()) watcher_.join();
    proc::Kill(child_);

    std::wstring dir = proc::ExeDir() + L"\\tools\\scrcpy";
    std::wstring exe = dir + L"\\scrcpy.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        log_(L"[Spegling] scrcpy.exe saknas: " + exe);
        return false;
    }

    std::wstring cmd = L"\"" + exe + L"\" --keyboard=uhid --mouse=uhid --no-audio --window-borderless --window-title=PhoneCamMirror";
    if (!serial.empty()) cmd += L" --serial=" + proc::ToWide(serial);

    if (!proc::Spawn(cmd, dir, false, false, child_))
    {
        log_(L"[Spegling] kunde inte starta scrcpy.exe");
        return false;
    }

    running_ = true;
    state_(true);
    log_(L"[Spegling] startad");

    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), child_.process, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DWORD pid = GetProcessId(child_.process);

    watcher_ = std::thread([this, h, pid]()
    {
        HWND found = nullptr;
        for (;;)
        {
            if (WaitForSingleObject(h, 100) == WAIT_OBJECT_0) break;
            if (!found)
            {
                found = FindWindowOfPid(pid);
                if (found && window_) window_(found);
            }
        }
        CloseHandle(h);
        if (running_.exchange(false))
        {
            log_(L"[Spegling] stoppad");
            state_(false);
        }
    });
    return true;
}

void Mirror::Stop()
{
    if (running_.exchange(false))
    {
        proc::Kill(child_);
        state_(false);
        log_(L"[Spegling] stoppad");
    }
    if (watcher_.joinable()) watcher_.join();
    proc::Kill(child_);
}
