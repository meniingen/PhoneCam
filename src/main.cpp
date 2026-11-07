#include <algorithm>
#include <cmath>
using std::min;
using std::max;
#include "Process.h"
#include "Mirror.h"
#include "Camera.h"
#include <windowsx.h>
#include <sal.h>
#include <commctrl.h>
#include <objidl.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <dxgi.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "dxgi.lib")

using namespace Gdiplus;

enum { WM_LOG = WM_APP + 1, WM_MIRROR_STATE, WM_CAMERA_STATE, WM_MIRROR_WND, WM_DEVICE, WM_CAMERA_STATUS, WM_TRAYICON };

// ---------------------------------------------------------------- state
static HWND g_wnd = nullptr;
static HWND g_scr = nullptr;            // inbäddat scrcpy-fönster
static int g_dpi = 96;
static int g_cw = 0, g_ch = 0;
static int g_hover = 0;                 // 1-3 combos, 4 start, 5 stop
static int g_sel[3] = { 0, 1, 1 };      // kamera, upplösning, fps
static bool g_connected = false;
static bool g_camRunning = false;
static bool g_cameraReady = false;
static bool g_torch = false, g_mirrorImage = false, g_flipImage = false, g_beautify = false;
static bool g_rotationLocked = false;
static int g_zoomValue = 6;
static int g_shutter = 300, g_iso = 6400, g_ev = 0, g_whiteBalance = 5000, g_focus = 16;
static bool g_exposureAuto = true, g_whiteBalanceAuto = true, g_focusAuto = true;
static bool g_updatingSettings = false;
static int g_videoWidth = 1280, g_videoHeight = 720, g_videoFps = 30;
static bool g_videoHighSpeed = false, g_hardwareDecoding = false;
static CameraConfig g_activeCameraConfig;
static bool g_hasActiveCameraConfig = false;
static std::wstring g_decoder = L"Default", g_mediaSavePath = L"C:\\Users\\Linus\\Videos\\PhoneCam";
static int g_gpuIndex = 0, g_videoOrientation = 0;
static std::atomic_bool g_generateLogs{ false };
static bool g_runAtStartup = false, g_minimizeToTray = true;
static bool g_miscChecks[5] = { false, true, true, false, true };
static bool g_connectionAnimation = true;
static bool g_thisSizeOnly = true;
static bool g_trayIconAdded = false;
static unsigned g_waitAnimationFrame = 0;
static std::mutex g_fileLogMutex;
static std::string g_originalAccelerometer = "1";
static std::string g_serial;
static HWND g_settings = nullptr;
static HWND g_settingTabs = nullptr;
static HWND g_settingTabButtons[4] = {};
static HWND g_settingPages[4] = {};
static int g_selectedSettingsTab = 3;
static std::unique_ptr<Camera> g_camera;
static CameraConfig ReadConfig();
static void RestartCamera();
static void ToggleRotationLock();
static void SaveSettings(HWND h);
static void LoadSettings(HWND h);
static void SetRunAtStartup(bool enabled);
static int MainFpsChoices(const wchar_t**& choices);
static int MainFpsValue(int index);
static bool MainFpsIsHighSpeed(int fps);
static std::wstring BrowseFolder(HWND owner, const std::wstring& initial);
static void PopulateVideoFps(HWND page, bool restoreCurrent);
static HBRUSH g_settingsBrush = nullptr;
static HFONT g_settingsFont = nullptr;
// PhoneCam-tema (samma färger som huvudfönstret)
static const COLORREF kDlgBg = RGB(30, 30, 36), kCard = RGB(46, 46, 54), kField = RGB(36, 36, 43), kBorder = RGB(72, 72, 84);
static const COLORREF kText = RGB(235, 235, 242), kDim = RGB(130, 130, 142), kBlue = RGB(88, 172, 255), kGreen = RGB(61, 200, 110);
static const COLORREF kBtn = RGB(60, 60, 70), kBtnHi = RGB(80, 80, 94);
static HBRUSH g_brDlg = nullptr, g_brCard = nullptr, g_brField = nullptr;
static void EnsureSettingsBrushes()
{
    if (!g_brDlg) g_brDlg = CreateSolidBrush(kDlgBg);
    if (!g_brCard) g_brCard = CreateSolidBrush(kCard);
    if (!g_brField) g_brField = CreateSolidBrush(kField);
    if (!g_settingsBrush) g_settingsBrush = g_brCard;
}
static void FillSolid(HDC dc, const RECT& r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
static void RoundBox(HDC dc, const RECT& r, COLORREF fill, COLORREF line, int rad)
{
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
static void DrawLabel(HDC dc, RECT r, const wchar_t* t, COLORREF c, UINT fmt)
{
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c);
    DrawTextW(dc, t, -1, &r, fmt | DT_SINGLELINE | DT_VCENTER);
}
static double SettingsScaleX(HWND page) { return page == g_settingPages[3] ? 2.10 : 1.80; }
static double SettingsScaleY(HWND page) { return page == g_settingPages[3] ? 1.68 : 1.80; }
static int SettingsPxX(HWND page, int value) { return (int)(value * SettingsScaleX(page) + 0.5); }
static int SettingsPxY(HWND page, int value) { return (int)(value * SettingsScaleY(page) + 0.5); }
static void ApplyManualCameraValue(int row, int value);
static void SetCameraReadback(int row, int value);
static void Log(const std::wstring& msg);
static LRESULT CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    EnsureSettingsBrushes();
    if (m == WM_NOTIFY && ((NMHDR*)l)->code == NM_CUSTOMDRAW)
    {
        NMCUSTOMDRAW* cd = (NMCUSTOMDRAW*)l;
        const int cid = GetDlgCtrlID(cd->hdr.hwndFrom);
        if (cid >= 1400 && cid <= 1406)
        {
            const bool on = IsWindowEnabled(cd->hdr.hwndFrom) != FALSE;
            if (cd->dwDrawStage == CDDS_PREPAINT) { FillSolid(cd->hdc, cd->rc, kCard); return CDRF_NOTIFYITEMDRAW; }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
            {
                if (cd->dwItemSpec == TBCD_CHANNEL)
                {
                    RECT ch = cd->rc; const int cy = (ch.top + ch.bottom) / 2;
                    RECT track = { ch.left, cy - 2, ch.right, cy + 2 };
                    FillSolid(cd->hdc, track, RGB(96, 96, 110));
                    RECT th = {}; SendMessageW(cd->hdr.hwndFrom, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
                    RECT part = { ch.left, cy - 2, (th.left + th.right) / 2, cy + 2 };
                    FillSolid(cd->hdc, part, on ? kBlue : RGB(96, 96, 110));
                    return CDRF_SKIPDEFAULT;
                }
                if (cd->dwItemSpec == TBCD_THUMB)
                {
                    RECT t = cd->rc; const int d = min(t.right - t.left, t.bottom - t.top);
                    const int cx = (t.left + t.right) / 2, cy = (t.top + t.bottom) / 2;
                    HBRUSH b = CreateSolidBrush(on ? kBlue : RGB(120, 120, 134)); HPEN pn = CreatePen(PS_SOLID, 2, on ? RGB(225, 238, 255) : RGB(150, 150, 164));
                    HGDIOBJ ob = SelectObject(cd->hdc, b), op = SelectObject(cd->hdc, pn);
                    Ellipse(cd->hdc, cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2);
                    SelectObject(cd->hdc, ob); SelectObject(cd->hdc, op); DeleteObject(b); DeleteObject(pn);
                    return CDRF_SKIPDEFAULT;
                }
            }
        }
    }
    if (m == WM_NOTIFY && ((NMHDR*)l)->hwndFrom == g_settingTabs && ((NMHDR*)l)->code == TCN_SELCHANGE)
    {
        int selected = TabCtrl_GetCurSel(g_settingTabs);
        for (int i = 0; i < 4; ++i) ShowWindow(g_settingPages[i], i == selected ? SW_SHOW : SW_HIDE);
        if (selected == 3)
        {
            SetWindowPos(h, nullptr, 0, 0, 1408, 741, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(g_settingTabs, nullptr, 24, 50, 1360, 64, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, IDOK), nullptr, 980, 670, 120, 40, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, IDCANCEL), nullptr, 1110, 670, 120, 40, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, 1001), nullptr, 1240, 670, 130, 40, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else
        {
            SetWindowPos(h, nullptr, 0, 0, 1110, 980, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(g_settingTabs, nullptr, 14, 8, 1080, 84, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, IDOK), nullptr, 680, 900, 110, 42, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, IDCANCEL), nullptr, 800, 900, 110, 42, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, 1001), nullptr, 920, 900, 140, 42, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    if (m == WM_COMMAND && HIWORD(w) == BN_CLICKED && LOWORD(w) >= 1600 && LOWORD(w) <= 1603)
    {
        const int selected = LOWORD(w) - 1600;
        g_selectedSettingsTab = selected;
        for (int i = 0; i < 4; ++i)
        {
            ShowWindow(g_settingPages[i], i == selected ? SW_SHOW : SW_HIDE);
            InvalidateRect(g_settingTabButtons[i], nullptr, TRUE);
        }
        if (selected == 3)
        {
            SetWindowPos(h, nullptr, 0, 0, 1408, 741, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, IDOK), nullptr, 980, 660, 120, 40, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, IDCANCEL), nullptr, 1110, 660, 120, 40, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, 1001), nullptr, 1240, 660, 130, 40, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else
        {
            SetWindowPos(h, nullptr, 0, 0, 1110, 980, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, IDOK), nullptr, 680, 900, 110, 42, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, IDCANCEL), nullptr, 800, 900, 110, 42, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(h, 1001), nullptr, 920, 900, 140, 42, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    if (m == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == 1458)) { SaveSettings(h); DestroyWindow(h); return 0; }
    if (m == WM_COMMAND && LOWORD(w) == IDCANCEL) { LoadSettings(h); DestroyWindow(h); return 0; }
    if (m == WM_COMMAND && LOWORD(w) == 1001) { SaveSettings(h); return 0; }
    if (m == WM_COMMAND && HIWORD(w) == EN_CHANGE)
    {
        int id = LOWORD(w);
        if (id >= 1410 && id <= 1416 && !g_updatingSettings)
        {
            wchar_t valueText[32] = {};
            GetWindowTextW((HWND)l, valueText, 32);
            int value = _wtoi(valueText);
            static const int mins[] = { 1, 50, -4, 2000, 0, 0, 0 };
            static const int maxs[] = { 1000, 6400, 4, 10000, 16, 12, 100 };
            const int row = id - 1410;
            value = max(mins[row], min(maxs[row], value));
            HWND page = GetParent((HWND)l);
            HWND slider = GetDlgItem(page, 1400 + row);
            if ((int)SendMessageW(slider, TBM_GETPOS, 0, 0) != value) SendMessageW(slider, TBM_SETPOS, TRUE, value);
            wchar_t normalized[24]; _itow_s(value, normalized, 10);
            if (wcscmp(valueText, normalized) != 0) SetWindowTextW((HWND)l, normalized);
            ApplyManualCameraValue(row, value);
        }
        return 0;
    }
    if (m == WM_HSCROLL)
    {
        HWND slider = (HWND)l;
        int id = GetDlgCtrlID(slider);
        if (id >= 1400 && id <= 1406)
        {
            int value = (int)SendMessageW(slider, TBM_GETPOS, 0, 0);
            wchar_t number[24]; _itow_s(value, number, 10);
            SetWindowTextW(GetDlgItem(GetParent(slider), 1410 + id - 1400), number);
            ApplyManualCameraValue(id - 1400, value);
        }
        return 0;
    }
    if (m == WM_COMMAND && HIWORD(w) == BN_CLICKED)
    {
        int id = LOWORD(w);
        HWND page = GetParent((HWND)l);
        if (id >= 1300 && id <= 1304)
        {
            const int index = id - 1300;
            g_miscChecks[index] = !g_miscChecks[index];
            if (index == 0) g_runAtStartup = g_miscChecks[index];
            else if (index == 1) g_minimizeToTray = g_miscChecks[index];
            else if (index == 2) g_connectionAnimation = g_miscChecks[index];
            else if (index == 3) g_generateLogs = g_miscChecks[index];
            else if (index == 4) g_hardwareDecoding = g_miscChecks[index];
            InvalidateRect((HWND)l, nullptr, TRUE);
            if (index == 4) EnableWindow(GetDlgItem(g_settingPages[2], 1315), g_hardwareDecoding);
            return 0;
        }
        if (id == 1108)
        {
            g_thisSizeOnly = !g_thisSizeOnly;
            InvalidateRect((HWND)l, nullptr, TRUE);
            return 0;
        }
        if (id == 1313)
        {
            std::wstring selected = BrowseFolder(h, g_mediaSavePath);
            if (!selected.empty()) SetWindowTextW(GetDlgItem(g_settingPages[2], 1312), selected.c_str());
            return 0;
        }
        if (id >= 1430 && id <= 1436)
        {
            const bool automatic = id == 1430 ? !g_exposureAuto : (id == 1433 ? !g_whiteBalanceAuto : (id == 1434 ? !g_focusAuto : false));
            if (id == 1430) g_exposureAuto = automatic;
            else if (id == 1433) g_whiteBalanceAuto = automatic;
            else if (id == 1434) g_focusAuto = automatic;
            if (g_camera && g_camRunning)
            {
                if (id == 1430)
                {
                    g_camera->SetCameraParam(2, automatic ? 1 : 0);
                    if (!automatic)
                    {
                        g_camera->SetCameraParam(3, 1000000000 / max(g_shutter, 1));
                        g_camera->SetCameraParam(4, g_iso);
                    }
                }
                else if (id == 1434)
                {
                    g_camera->SetCameraParam(0, automatic ? 3 : 0);
                    if (automatic) g_camera->SetCameraParam(5, 1); // start one AF pass immediately
                    else g_camera->SetCameraParam(1, g_focus * 1000);
                }
                else if (id == 1433)
                {
                    // The bundled camera server has no white-balance control message.
                    // Keep the UI state, but do not send unsupported parameter IDs.
                    Log(L"[Kamera] Automatisk vitbalans stöds inte av den medföljande Android-servern.");
                }
            }
            InvalidateRect((HWND)l, nullptr, TRUE);
            return 0;
        }
        if (id >= 1450 && id <= 1457)
        {
            if (id == 1450)
            {
                const std::wstring adb = L"\"" + proc::ExeDir() + L"\\tools\\scrcpy\\adb.exe\" -s " + proc::ToWide(g_serial) + L" shell dumpsys battery";
                std::string output;
                std::wstring battery = L"N/A";
                if (proc::RunCapture(adb, proc::ExeDir(), output, 4000))
                {
                    size_t pos = output.find("level:");
                    if (pos != std::string::npos) battery = proc::ToWide(output.substr(pos + 6, output.find_first_of("\r\n", pos) - (pos + 6))) + L"%";
                }
                SetWindowTextW((HWND)l, battery.c_str());
            }
            else if (id == 1451 && g_camera) { g_torch = !g_torch; g_camera->SetTorch(g_torch); }
            else if (id == 1452) { g_beautify = !g_beautify; RestartCamera(); }
            else if (id == 1453) { g_mirrorImage = !g_mirrorImage; RestartCamera(); }
            else if (id == 1454) { g_flipImage = !g_flipImage; RestartCamera(); }
            else if (id == 1455) { g_sel[0] = 1 - g_sel[0]; RestartCamera(); }
            else if (id == 1456) ToggleRotationLock();
            else if (id == 1457) { DestroyWindow(h); return 0; }
        }
    }
    if (m == WM_DRAWITEM)
    {
        auto* item = (DRAWITEMSTRUCT*)l;
        const int id = (int)item->CtlID;
        HDC dc = item->hDC; RECT r = item->rcItem;
        HGDIOBJ oldFont = g_settingsFont ? SelectObject(dc, g_settingsFont) : nullptr;
        const bool down = (item->itemState & ODS_SELECTED) != 0;
        wchar_t label[128] = {}; GetWindowTextW(item->hwndItem, label, 128);
        bool handled = true;
        if (id == 1700) // grupprutor (fieldset-stil, mörkt kort)
        {
            SIZE ts = {}; if (label[0]) GetTextExtentPoint32W(dc, label, (int)wcslen(label), &ts);
            FillSolid(dc, r, kDlgBg);
            RECT box = r; if (label[0]) box.top += ts.cy / 2;
            RoundBox(dc, box, kCard, kBorder, 14);
            if (label[0])
            {
                RECT gap = { r.left + 14, box.top - 1, r.left + 14 + ts.cx + 16, box.top + 2 };
                FillSolid(dc, gap, kCard);
                RECT tr = { r.left + 22, r.top, r.right, r.top + ts.cy };
                DrawLabel(dc, tr, label, kText, DT_LEFT);
            }
        }
        else if (id == 1702) FillSolid(dc, r, kBorder);
        else if (id == 1430 || id == 1433 || id == 1434) // Auto-reglage
        {
            FillSolid(dc, r, kCard);
            const bool enabled = id == 1430 ? g_exposureAuto : (id == 1433 ? g_whiteBalanceAuto : g_focusAuto);
            RECT pill = { r.left + 2, r.top + 3, r.left + 52, r.bottom - 3 };
            COLORREF track = enabled ? kGreen : RGB(88, 88, 100);
            HBRUSH tb = CreateSolidBrush(track); HPEN tp = CreatePen(PS_SOLID, 1, track);
            HGDIOBJ ob = SelectObject(dc, tb), op = SelectObject(dc, tp);
            RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, pill.bottom - pill.top, pill.bottom - pill.top);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(tb); DeleteObject(tp);
            const int d = pill.bottom - pill.top - 6; const int cx = enabled ? pill.right - d / 2 - 4 : pill.left + d / 2 + 4; const int cy = (pill.top + pill.bottom) / 2;
            COLORREF kn = enabled ? kDlgBg : RGB(235, 235, 242);
            HBRUSH kb = CreateSolidBrush(kn); HPEN kp = CreatePen(PS_SOLID, 1, kn);
            ob = SelectObject(dc, kb); op = SelectObject(dc, kp);
            Ellipse(dc, cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(kb); DeleteObject(kp);
            RECT text = r; text.left += 60; DrawLabel(dc, text, L"Auto", kText, DT_LEFT);
        }
        else if ((id >= 1300 && id <= 1304) || id == 1108) // kryssrutor
        {
            FillSolid(dc, r, kCard);
            const bool on = (id >= 1300 && id <= 1304) ? g_miscChecks[id - 1300] : g_thisSizeOnly;
            const int d = min(24, (int)(r.bottom - r.top) - 4);
            const int bx = r.left + 2, by = (r.top + r.bottom - d) / 2;
            RECT box = { bx, by, bx + d, by + d };
            RoundBox(dc, box, on ? kBlue : kField, on ? kBlue : RGB(130, 130, 145), 8);
            if (on)
            {
                HPEN cp = CreatePen(PS_SOLID, 3, kDlgBg); HGDIOBJ op = SelectObject(dc, cp);
                MoveToEx(dc, bx + d * 22 / 100, by + d / 2, nullptr); LineTo(dc, bx + d * 42 / 100, by + d * 70 / 100); LineTo(dc, bx + d * 78 / 100, by + d * 28 / 100);
                SelectObject(dc, op); DeleteObject(cp);
            }
            RECT text = r; text.left = bx + d + 12; DrawLabel(dc, text, label, kText, DT_LEFT);
        }
        else if (id >= 1600 && id <= 1603) // flikar
        {
            FillSolid(dc, r, kDlgBg);
            const bool sel = id - 1600 == g_selectedSettingsTab;
            DrawLabel(dc, r, label, sel ? RGB(245, 245, 250) : RGB(160, 160, 174), DT_CENTER);
            if (sel)
            {
                const int x0 = r.left + 10, x1 = r.right - 10;
                for (int x = x0; x < x1; ++x)
                {
                    const double t = (double)(x - x0) / max(1, x1 - x0);
                    RECT px = { x, r.bottom - 4, x + 1, r.bottom - 1 };
                    FillSolid(dc, px, RGB((int)(150 + (70 - 150) * t), (int)(90 + (140 - 90) * t), 255));
                }
            }
        }
        else if ((id >= 1450 && id <= 1458) || id == IDOK || id == IDCANCEL || id == 1001 || id == 1313) // knappar
        {
            const bool primary = (id == 1458 || id == IDOK);
            FillSolid(dc, r, id >= 1450 && id <= 1458 ? kCard : kDlgBg);
            RoundBox(dc, r, primary ? (down ? RGB(70, 150, 230) : kBlue) : (down ? kBtnHi : kBtn), primary ? kBlue : kBorder, 12);
            DrawLabel(dc, r, label, primary ? kDlgBg : kText, DT_CENTER);
        }
        else handled = false;
        if (oldFont) SelectObject(dc, oldFont);
        if (handled) return TRUE;
    }
    if (m == WM_COMMAND && HIWORD(w) == CBN_SELCHANGE)
    {
        if (LOWORD(w) == 1102) { PopulateVideoFps(g_settingPages[0], false); return 0; }
        if (LOWORD(w) == 1103)
        {
            HWND combo = GetDlgItem(g_settingPages[0], 1103); int at = ComboBox_GetCurSel(combo);
            if (at != CB_ERR) { DWORD_PTR mode = ComboBox_GetItemData(combo, at); g_videoFps = (int)(mode & 0xffff); g_videoHighSpeed = (mode & 0x10000) != 0; }
            return 0;
        }
        if (LOWORD(w) == 1101) { g_videoOrientation = ComboBox_GetCurSel(GetDlgItem(g_settingPages[0], 1101)); return 0; }
    }
    if (m == WM_CTLCOLORDLG) return (LRESULT)g_brDlg;
    if (m == WM_CTLCOLORSTATIC)
    {
        HDC dc = (HDC)w; wchar_t cls[16] = {}; GetClassNameW((HWND)l, cls, 16);
        if (!_wcsicmp(cls, L"Edit")) { SetTextColor(dc, kDim); SetBkColor(dc, kField); return (LRESULT)g_brField; }
        SetTextColor(dc, kText); SetBkColor(dc, kCard); SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_brCard;
    }
    if (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX)
    {
        HDC dc = (HDC)w; SetTextColor(dc, kText); SetBkColor(dc, kField);
        return (LRESULT)g_brField;
    }
    if (m == WM_CTLCOLORBTN)
    {
        HDC dc = (HDC)w; SetTextColor(dc, kText); SetBkColor(dc, kCard);
        return (LRESULT)g_brCard;
    }
    if (m == WM_NCDESTROY && g_settings == h) g_settings = nullptr;
    return DefWindowProcW(h, m, w, l);
}
static std::wstring g_msg;
static std::atomic<bool> g_quit{ false };
static std::thread g_poll;

static std::unique_ptr<Mirror> g_mirror;

static std::mutex g_frameMx;
static std::vector<unsigned char> g_frame;
static int g_fw = 0, g_fh = 0;

static const wchar_t* kFacing[] = { L"Bakre kamera", L"Fr\u00e4mre kamera" };
static const wchar_t* kRes[] = { L"480p", L"720p", L"1080p", L"4K", L"8K" };
static const wchar_t* kFrontRes[] = { L"720p", L"1080p" };
static const wchar_t* kRear480Fps[] = { L"15 fps", L"24 fps", L"30 fps", L"60 fps", L"120 fps", L"240 fps", L"480 fps", L"960 fps", L"1920 fps" };
static const wchar_t* kRear720Fps[] = { L"15 fps", L"24 fps", L"30 fps", L"60 fps", L"120 fps", L"240 fps", L"480 fps", L"960 fps", L"1920 fps" };
static const wchar_t* kRear1080Fps[] = { L"15 fps", L"24 fps", L"30 fps", L"60 fps", L"120 fps", L"240 fps", L"480 fps", L"960 fps" };
static const wchar_t* kRear4kFps[] = { L"30 fps", L"60 fps" };
static const wchar_t* kFront1080Fps[] = { L"60 fps" };
static const wchar_t* kFront720Fps[] = { L"120 fps" };
static const wchar_t* k8kFps[] = { L"24 fps" };
static const wchar_t** kItems[] = { kFacing, kRes, kRear720Fps };
static const int kCount[] = { 2, 5, 9 };
static int MainFpsChoices(const wchar_t**& choices)
{
    if (g_sel[0] == 1 && g_sel[1] == 2) { choices = kFront1080Fps; return 1; }
    if (g_sel[0] == 1) { choices = kFront720Fps; return 1; }
    if (g_sel[1] == 4) { choices = k8kFps; return 1; }
    if (g_sel[1] == 3) { choices = kRear4kFps; return 2; }
    if (g_sel[1] == 2) { choices = kRear1080Fps; return 8; }
    if (g_sel[1] == 0) { choices = kRear480Fps; return 9; }
    choices = kRear720Fps; return 9;
}
static int MainFpsValue(int index)
{
    if (g_sel[0] == 1 && g_sel[1] == 2) return 60;
    if (g_sel[0] == 1) return 120;
    if (g_sel[1] == 4) return 24;
    if (g_sel[1] == 3) { static const int values[] = { 30, 60 }; return values[max(0, min(1, index))]; }
    if (g_sel[1] == 2) { static const int values[] = { 15, 24, 30, 60, 120, 240, 480, 960 }; return values[max(0, min(7, index))]; }
    static const int values[] = { 15, 24, 30, 60, 120, 240, 480, 960, 1920 };
    return values[max(0, min(8, index))];
}
static bool MainFpsIsHighSpeed(int fps)
{
    if (g_sel[0] == 1) return g_sel[1] == 1 && fps >= 120;
    return g_sel[1] <= 2 && fps >= 120;
}
static const wchar_t* kLabel[] = { L"Kamera", L"Uppl\u00f6sning", L"FPS" };

// ---------------------------------------------------------------- layout (96-dpi koordinater skalas med S)
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static RECT Rc(int x, int y, int w, int h) { RECT r = { S(x), S(y), S(x + w), S(y + h) }; return r; }
static RECT CardR() { return Rc(32, 140, 350, 374); }
static RECT ComboR(int i) { return Rc(56, 198 + i * 92, 302, 42); }
static RECT LabelR(int i) { return Rc(58, 170 + i * 92, 300, 22); }
static RECT StartR() { return Rc(56, 450, 144, 40); }
static RECT StopR() { return Rc(214, 450, 144, 40); }
static RECT SettingsR() { return Rc(400, 450, 144, 40); }
static RECT PanelR() { return Rc(32, 534, 576, 352); }
static RECT ImgR() { return Rc(48, 550, 544, 306); }
static RECT CaptionR() { return Rc(48, 858, 544, 22); }
static RECT TitleR() { return Rc(32, 26, 500, 52); }
static RECT StatusR() { return Rc(58, 88, 400, 24); }
static RECT MsgR() { return Rc(32, 114, 580, 20); }
static RECT RightR() { RECT r = { S(640), 0, g_cw, g_ch }; return r; }

static RectF F(const RECT& r) { return RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)); }
static bool In(const RECT& r, int x, int y) { POINT p = { x, y }; return PtInRect(&r, p) != FALSE; }

static bool Enabled(int id)
{
    if (id >= 1 && id <= 3) return !g_camRunning;
    if (id == 4) return g_connected && !g_camRunning;
    if (id == 5) return g_camRunning;
    if (id == 6) return g_cameraReady;
    return false;
}

static int HitTest(int x, int y)
{
    for (int i = 0; i < 3; ++i) if (In(ComboR(i), x, y)) return 1 + i;
    if (In(StartR(), x, y)) return 4;
    if (In(StopR(), x, y)) return 5;
    if (g_cameraReady && In(SettingsR(), x, y)) return 6;
    return 0;
}

// ---------------------------------------------------------------- tema
struct Fonts
{
    std::unique_ptr<Font> title, label, text, tiny, btn;
    int dpi = 0;
};
static Fonts g_f;

static void EnsureFonts()
{
    if (g_f.dpi == g_dpi) return;
    FontFamily ff(L"Segoe UI");
    FontFamily fs(L"Segoe UI Semibold");
    if (fs.IsAvailable())
    {
        g_f.title.reset(new Font(&fs, (REAL)S(32), FontStyleRegular, UnitPixel));
        g_f.btn.reset(new Font(&fs, (REAL)S(16), FontStyleRegular, UnitPixel));
    }
    else
    {
        g_f.title.reset(new Font(&ff, (REAL)S(32), FontStyleBold, UnitPixel));
        g_f.btn.reset(new Font(&ff, (REAL)S(16), FontStyleBold, UnitPixel));
    }
    g_f.label.reset(new Font(&ff, (REAL)S(15), FontStyleRegular, UnitPixel));
    g_f.text.reset(new Font(&ff, (REAL)S(16), FontStyleRegular, UnitPixel));
    g_f.tiny.reset(new Font(&ff, (REAL)S(13), FontStyleRegular, UnitPixel));
    g_f.dpi = g_dpi;
}

static void RoundPath(GraphicsPath& p, const RectF& r, float rad)
{
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

static void FillRound(Graphics& g, const RECT& r, int rad, const Color& fill, const Color* border)
{
    GraphicsPath p;
    RectF f = F(r);
    f.Width -= 1; f.Height -= 1;
    RoundPath(p, f, (REAL)rad);
    SolidBrush b(fill);
    g.FillPath(&b, &p);
    if (border)
    {
        Pen pen(*border, 1.0f);
        g.DrawPath(&pen, &p);
    }
}

static void DrawTxt(Graphics& g, const wchar_t* s, Font* f, const RECT& r, const Color& c,
    StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter)
{
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s, -1, f, F(r), &sf, &b);
}

static void DrawCombo(Graphics& g, int i)
{
    RECT r = ComboR(i);
    bool dis = !Enabled(1 + i);
    bool hov = g_hover == 1 + i;
    Color border(255, 110, 110, 124);
    FillRound(g, r, S(8), dis ? Color(255, 44, 44, 50) : (hov ? Color(255, 62, 62, 72) : Color(255, 50, 50, 58)), &border);
    RECT t = { r.left + S(14), r.top, r.right - S(36), r.bottom };
    const wchar_t* label = kItems[i][g_sel[i]];
    if (i == 1 && g_sel[0] == 1)
        label = kFrontRes[max(0, min(1, g_sel[1] - 1))];
    else if (i == 2)
    {
        const wchar_t** choices = nullptr;
        const int count = MainFpsChoices(choices);
        if (choices && count > 0) label = choices[max(0, min(count - 1, g_sel[i]))];
    }
    DrawTxt(g, label, g_f.text.get(), t, dis ? Color(255, 150, 150, 158) : Color(255, 240, 240, 245));
    float cx = (float)(r.right - S(22)), cy = (r.top + r.bottom) / 2.0f;
    Pen pen(dis ? Color(255, 140, 140, 150) : Color(255, 220, 220, 230), (REAL)max(1.6, S(2) * 0.9));
    pen.SetLineJoin(LineJoinRound);
    PointF pts[3] = { PointF(cx - S(5), cy - S(2)), PointF(cx, cy + S(3)), PointF(cx + S(5), cy - S(2)) };
    g.DrawLines(&pen, pts, 3);
}

static void DrawButton(Graphics& g, const RECT& r, int id, const Color& base, bool camera, const wchar_t* text)
{
    bool en = Enabled(id), hov = g_hover == id && en;
    Color c = base;
    if (!en) c = Color(110, base.GetR(), base.GetG(), base.GetB());
    else if (hov) c = Color(255, (BYTE)min(255, base.GetR() + 18), (BYTE)min(255, base.GetG() + 18), (BYTE)min(255, base.GetB() + 18));
    FillRound(g, r, S(8), c, nullptr);

    Color ink(en ? 255 : 170, 22, 26, 36);
    RectF m;
    g.MeasureString(text, -1, g_f.btn.get(), PointF(0, 0), &m);
    float iconW = (float)S(20), gap = (float)S(8);
    float total = iconW + gap + m.Width;
    float x = (r.left + r.right) / 2.0f - total / 2.0f;
    float cy = (r.top + r.bottom) / 2.0f;

    Pen pen(ink, (REAL)max(1.5, S(2) * 0.85));
    SolidBrush ib(ink);
    if (camera)
    {
        GraphicsPath body;
        RoundPath(body, RectF(x, cy - S(7), (REAL)S(20), (REAL)S(15)), (REAL)S(3));
        g.DrawPath(&pen, &body);
        g.DrawRectangle(&pen, RectF(x + S(6), cy - S(10), (REAL)S(8), (REAL)S(3)));
        g.DrawEllipse(&pen, RectF(x + S(10) - S(7) / 2.0f, cy - S(3), (REAL)S(7), (REAL)S(7)));
    }
    else
    {
        GraphicsPath sq;
        RoundPath(sq, RectF(x + S(3), cy - S(7), (REAL)S(14), (REAL)S(14)), (REAL)S(3));
        g.FillPath(&ib, &sq);
    }
    RECT tr = { (LONG)(x + iconW + gap), r.top, r.right, r.bottom };
    DrawTxt(g, text, g_f.btn.get(), tr, ink);
}

static void Paint(Graphics& g, const RECT& clip)
{
    EnsureFonts();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(InterpolationModeBilinear);
    g.SetClip(Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));

    // bakgrund
    {
        LinearGradientBrush lb(PointF(0, 0), PointF(0, (REAL)max(g_ch, 1)), Color(255, 42, 42, 50), Color(255, 26, 26, 31));
        g.FillRectangle(&lb, RectF(0, 0, (REAL)S(640), (REAL)g_ch));
        SolidBrush rb(Color(255, 14, 14, 17));
        RECT rr = RightR();
        g.FillRectangle(&rb, F(rr));
        Pen div(Color(255, 60, 60, 70), 1.0f);
        g.DrawLine(&div, (REAL)S(640), 0.0f, (REAL)S(640), (REAL)g_ch);
    }

    // titel + status
    DrawTxt(g, L"PhoneCam", g_f.title.get(), TitleR(), Color(255, 245, 245, 250));
    {
        RECT sr = StatusR();
        float cy = (sr.top + sr.bottom) / 2.0f, cx = (float)S(44);
        Color dot = g_connected ? Color(255, 61, 220, 132) : Color(255, 140, 140, 150);
        if (g_connected)
        {
            SolidBrush glow(Color(60, 61, 220, 132));
            g.FillEllipse(&glow, cx - S(10), cy - S(10), (REAL)S(20), (REAL)S(20));
        }
        SolidBrush db(dot);
        g.FillEllipse(&db, cx - S(5), cy - S(5), (REAL)S(10), (REAL)S(10));
        wchar_t statusText[64] = L"Ansluten";
        if (!g_connected)
        {
            if (g_connectionAnimation)
            {
                const int dots = (int)(g_waitAnimationFrame % 4);
                wcscpy_s(statusText, L"Söker efter telefon");
                for (int i = 0; i < dots; ++i) wcscat_s(statusText, L".");
            }
            else wcscpy_s(statusText, L"Ingen enhet ansluten");
        }
        DrawTxt(g, statusText, g_f.text.get(), sr, Color(255, 205, 205, 215));
    }
    if (!g_msg.empty())
        DrawTxt(g, g_msg.c_str(), g_f.tiny.get(), MsgR(), Color(255, 150, 150, 162));

    // kontrollkort
    {
        RECT cr = CardR();
        Color border(255, 72, 72, 84);
        GraphicsPath p;
        RectF cf = F(cr); cf.Width -= 1; cf.Height -= 1;
        RoundPath(p, cf, (REAL)S(16));
        LinearGradientBrush lb(PointF(0, (REAL)cr.top), PointF(0, (REAL)cr.bottom), Color(255, 52, 52, 60), Color(255, 40, 40, 47));
        g.FillPath(&lb, &p);
        // accentlinje överst
        GraphicsPath saved;
        g.SetClip(&p, CombineModeIntersect);
        LinearGradientBrush ac(PointF((REAL)cr.left, 0), PointF((REAL)cr.right, 0), Color(255, 150, 90, 255), Color(255, 70, 140, 255));
        g.FillRectangle(&ac, RectF((REAL)cr.left, (REAL)cr.top, (REAL)(cr.right - cr.left), (REAL)max(2, S(3))));
        g.SetClip(Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));
        Pen bp(border, 1.0f);
        g.DrawPath(&bp, &p);

        for (int i = 0; i < 3; ++i)
        {
            DrawTxt(g, kLabel[i], g_f.label.get(), LabelR(i), Color(255, 235, 235, 242));
            DrawCombo(g, i);
        }
        DrawButton(g, StartR(), 4, Color(255, 88, 172, 255), true, L"Start");
        DrawButton(g, StopR(), 5, Color(255, 255, 112, 80), false, L"Stopp");
        if (g_cameraReady) DrawButton(g, SettingsR(), 6, Color(255, 190, 160, 255), false, L"Settings");
    }

    // förhandsvisning
    {
        RECT pr = PanelR();
        Color border(255, 72, 72, 84);
        FillRound(g, pr, S(16), Color(255, 38, 38, 45), &border);

        RECT ir = ImgR();
        GraphicsPath clipP;
        RectF imf = F(ir);
        RoundPath(clipP, imf, (REAL)S(10));
        g.SetClip(&clipP, CombineModeIntersect);
        {
            std::lock_guard<std::mutex> lk(g_frameMx);
            if (g_camRunning && !g_frame.empty() && g_fw > 0)
            {
                Bitmap bmp(g_fw, g_fh, PixelFormat24bppRGB);
                BitmapData bits = {};
                Rect bounds(0, 0, g_fw, g_fh);
                if (bmp.GetLastStatus() == Ok && bmp.LockBits(&bounds, ImageLockModeWrite, PixelFormat24bppRGB, &bits) == Ok && bits.Scan0)
                {
                    const size_t rowBytes = (size_t)g_fw * 3;
                    if ((size_t)abs(bits.Stride) >= rowBytes)
                    {
                        for (int y = 0; y < g_fh; ++y)
                            memcpy((unsigned char*)bits.Scan0 + (ptrdiff_t)y * bits.Stride,
                                   g_frame.data() + (size_t)y * rowBytes, rowBytes);
                        bmp.UnlockBits(&bits);
                        g.DrawImage(&bmp, imf);
                    }
                    else bmp.UnlockBits(&bits);
                }
            }
            else
            {
                SolidBrush bb(Color(255, 18, 18, 22));
                g.FillRectangle(&bb, imf);
                DrawTxt(g, g_camRunning ? L"Startar kamera..." : L"Ingen f\u00f6rhandsvisning", g_f.text.get(), ir,
                    Color(255, 130, 130, 142), StringAlignmentCenter);
            }
        }
        g.SetClip(Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));
        Pen ip(Color(255, 90, 90, 102), 1.0f);
        g.DrawPath(&ip, &clipP);

        std::wstring cap = std::wstring(L"Live F\u00f6rhandsvisning (") + (g_sel[0] == 0 ? L"Bakre" : L"Fr\u00e4mre") + L")";
        DrawTxt(g, cap.c_str(), g_f.tiny.get(), CaptionR(), Color(255, 170, 170, 182), StringAlignmentFar);
    }

    // höger sida: platshållare när scrcpy inte är inbäddad
    if (!g_scr)
    {
        RECT rr = RightR();
        DrawTxt(g, g_connected ? L"Startar sk\u00e4rmspegling..." : L"Anslut telefonen via USB",
            g_f.text.get(), rr, Color(255, 120, 120, 132), StringAlignmentCenter);
    }
}

static HWND SettingControl(HWND parent, _In_z_ const wchar_t* cls, _In_z_ const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0)
{
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, SettingsPxX(parent, x), SettingsPxY(parent, y), SettingsPxX(parent, w), SettingsPxY(parent, h), parent, id ? (HMENU)(INT_PTR)id : nullptr, GetModuleHandleW(nullptr), nullptr);
}

static void ApplyManualCameraValue(int row, int value)
{
    const int previousZoom = g_zoomValue;
    if (row == 0) g_shutter = value;
    else if (row == 1) g_iso = value;
    else if (row == 2) g_ev = value;
    else if (row == 3) g_whiteBalance = value;
    else if (row == 4) g_focus = value;
    else if (row == 5) g_zoomValue = value;
    if (row == 0 || row == 1)
    {
        g_exposureAuto = false;
        SendMessageW(GetDlgItem(g_settingPages[3], 1430), BM_SETCHECK, BST_UNCHECKED, 0);
        InvalidateRect(GetDlgItem(g_settingPages[3], 1430), nullptr, TRUE);
    }
    else if (row == 3)
    {
        g_whiteBalanceAuto = false;
        SendMessageW(GetDlgItem(g_settingPages[3], 1433), BM_SETCHECK, BST_UNCHECKED, 0);
        InvalidateRect(GetDlgItem(g_settingPages[3], 1433), nullptr, TRUE);
    }
    else if (row == 4)
    {
        g_focusAuto = false;
        SendMessageW(GetDlgItem(g_settingPages[3], 1434), BM_SETCHECK, BST_UNCHECKED, 0);
        InvalidateRect(GetDlgItem(g_settingPages[3], 1434), nullptr, TRUE);
    }
    if (!g_camRunning || !g_camera) return;
    if (row == 0 || row == 1)
    {
        g_camera->SetCameraParam(2, 0);
        if (row == 0) g_camera->SetCameraParam(3, 1000000000 / max(value, 1));
        else g_camera->SetCameraParam(4, value);
    }
    else if (row == 2) { /* EV is not exposed by this Android server build. */ }
    else if (row == 3) { /* Kelvin white balance is not exposed by this Android server build. */ }
    else if (row == 4)
    {
        g_camera->SetCameraParam(0, 0);
        g_camera->SetCameraParam(1, value * 1000);
    }
    else if (row == 5 && value != previousZoom) g_camera->AdjustZoom(value > previousZoom, abs(value - previousZoom));
}

static void SetCameraReadback(int row, int value)
{
    if (row == 0) g_shutter = value;
    else if (row == 1) g_iso = value;
    else if (row == 2) g_ev = value;
    else if (row == 3) g_whiteBalance = value;
    else if (row == 4) g_focus = value;
    else if (row == 5) g_zoomValue = value;
    if (!g_settings || !IsWindow(g_settings)) return;
    HWND page = g_settingPages[3];
    HWND slider = GetDlgItem(page, 1400 + row);
    HWND edit = GetDlgItem(page, 1410 + row);
    if (!slider || !edit) return;
    int low = (int)SendMessageW(slider, TBM_GETRANGEMIN, 0, 0);
    int high = (int)SendMessageW(slider, TBM_GETRANGEMAX, 0, 0);
    value = max(low, min(high, value));
    g_updatingSettings = true;
    SendMessageW(slider, TBM_SETPOS, TRUE, value);
    wchar_t text[24]; _itow_s(value, text, 10); SetWindowTextW(edit, text);
    g_updatingSettings = false;
}

static LRESULT CALLBACK SettingsPageProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((m == WM_COMMAND || m == WM_HSCROLL || m == WM_DRAWITEM || m == WM_NOTIFY || m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX || m == WM_CTLCOLORBTN) && g_settings) return SendMessageW(g_settings, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

static BOOL CALLBACK ThemeSettingsChild(HWND child, LPARAM)
{
    wchar_t cls[32] = {}; GetClassNameW(child, cls, 32);
    if (!_wcsicmp(cls, L"ComboBox") || !_wcsicmp(cls, L"Edit")) SetWindowTheme(child, L"DarkMode_CFD", nullptr);
    else SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    if (g_settingsFont) SendMessageW(child, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
    return TRUE;
}

static HWND SettingPage(HWND parent, int index)
{
    static bool registered = false;
    if (!registered) { WNDCLASSW wc = {}; wc.lpfnWndProc = SettingsPageProc; wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = CreateSolidBrush(kDlgBg); wc.lpszClassName = L"PhoneCamSettingsPage"; RegisterClassW(&wc); registered = true; }
    const double sx = index == 3 ? 2.10 : 1.80;
    const double sy = index == 3 ? 1.68 : 1.80;
    const int x = (int)(8 * sx + 0.5);
    const int y = (int)((index == 3 ? 51 : 75) * sy + 0.5);
    const int width = (int)((index == 3 ? 660 : 600) * sx + 0.5);
    const int height = (int)((index == 3 ? 365 : 430) * sy + 0.5);
    HWND page = CreateWindowW(L"PhoneCamSettingsPage", L"", WS_CHILD | WS_CLIPSIBLINGS | (index == 0 ? WS_VISIBLE : 0), x, y, width, height, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_settingPages[index] = page;
    return page;
}

static std::wstring SettingsPath() { return proc::ExeDir() + L"\\PhoneCam.ini"; }

static void LoadRuntimeSettings()
{
    const std::wstring path = SettingsPath();
    const wchar_t* keys[] = { L"RunAtStartup", L"MinimizeToTray", L"ConnectionAnimation", L"GenerateLogs", L"HardwareDecoding" };
    const int defaults[] = { BST_UNCHECKED, BST_CHECKED, BST_CHECKED, BST_UNCHECKED, BST_CHECKED };
    for (int i = 0; i < 5; ++i)
        g_miscChecks[i] = GetPrivateProfileIntW(L"Miscellaneous", keys[i], defaults[i], path.c_str()) == BST_CHECKED;
    g_runAtStartup = g_miscChecks[0];
    g_minimizeToTray = g_miscChecks[1];
    g_connectionAnimation = g_miscChecks[2];
    g_generateLogs = g_miscChecks[3];
    g_hardwareDecoding = g_miscChecks[4];
    wchar_t decoder[64] = {};
    GetPrivateProfileStringW(L"Miscellaneous", L"Decoder", L"Default", decoder, (DWORD)_countof(decoder), path.c_str());
    g_decoder = decoder;

    g_videoWidth = GetPrivateProfileIntW(L"Video", L"Width", g_videoWidth, path.c_str());
    g_videoHeight = GetPrivateProfileIntW(L"Video", L"Height", g_videoHeight, path.c_str());
    g_videoFps = GetPrivateProfileIntW(L"Video", L"FPS", g_videoFps, path.c_str());
    g_videoHighSpeed = GetPrivateProfileIntW(L"Video", L"HighSpeed", g_videoHighSpeed ? 1 : 0, path.c_str()) != 0;
    g_videoOrientation = GetPrivateProfileIntW(L"Video", L"Orientation", g_videoOrientation, path.c_str());
    g_thisSizeOnly = GetPrivateProfileIntW(L"Video", L"ThisSizeOnly", BST_CHECKED, path.c_str()) == BST_CHECKED;
    if (g_videoHeight == 480) g_sel[1] = 0;
    else if (g_videoHeight == 720) g_sel[1] = 1;
    else if (g_videoHeight == 1080) g_sel[1] = 2;
    else if (g_videoHeight == 2160 && g_sel[0] == 0) g_sel[1] = 3;
    else if (g_videoHeight == 4320 && g_sel[0] == 0) g_sel[1] = 4;
    const wchar_t** fpsChoices = nullptr;
    const int fpsCount = MainFpsChoices(fpsChoices);
    g_sel[2] = 0;
    for (int i = 0; i < fpsCount; ++i) if (MainFpsValue(i) == g_videoFps) { g_sel[2] = i; break; }
    g_videoFps = MainFpsValue(g_sel[2]);
    g_videoHighSpeed = MainFpsIsHighSpeed(g_videoFps);

    const wchar_t* cameraKeys[] = { L"Shutter",L"ISO",L"EV",L"WhiteBalance",L"Focus",L"Zoom" };
    int* cameraValues[] = { &g_shutter,&g_iso,&g_ev,&g_whiteBalance,&g_focus,&g_zoomValue };
    const int cameraDefaults[] = { 300,6400,0,5000,16,6 };
    for (int i = 0; i < 6; ++i) *cameraValues[i] = (int)GetPrivateProfileIntW(L"Camera", cameraKeys[i], cameraDefaults[i], path.c_str());
    g_exposureAuto = GetPrivateProfileIntW(L"Camera", L"ExposureAuto", BST_CHECKED, path.c_str()) == BST_CHECKED;
    g_whiteBalanceAuto = GetPrivateProfileIntW(L"Camera", L"WhiteBalanceAuto", BST_CHECKED, path.c_str()) == BST_CHECKED;
    g_focusAuto = GetPrivateProfileIntW(L"Camera", L"FocusAuto", BST_CHECKED, path.c_str()) == BST_CHECKED;
}

static void SetRunAtStartup(bool enabled)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const std::wstring name = L"PhoneCam";
    if (enabled)
    {
        std::wstring command = L"\"" + proc::ExeDir() + L"\\PhoneCam.exe\"";
        RegSetValueExW(key, name.c_str(), 0, REG_SZ, (const BYTE*)command.c_str(), (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    }
    else RegDeleteValueW(key, name.c_str());
    RegCloseKey(key);
}

static void AddTrayIcon(HWND window)
{
    if (g_trayIconAdded) return;
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = WM_TRAYICON;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"PhoneCam");
    if (Shell_NotifyIconW(NIM_ADD, &data))
    {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        g_trayIconAdded = true;
    }
}

static void RemoveTrayIcon(HWND window)
{
    if (!g_trayIconAdded) return;
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data); data.hWnd = window; data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    g_trayIconAdded = false;
}

static void RestoreFromTray(HWND window)
{
    RemoveTrayIcon(window);
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

static void ShowTrayMenu(HWND window)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"Öppna PhoneCam");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"Avsluta");
    POINT point; GetCursorPos(&point);
    SetForegroundWindow(window);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    if (command == 1) RestoreFromTray(window);
    else if (command == 2) PostMessageW(window, WM_CLOSE, 0, 0);
}

static void AddAudioEndpointChoices(HWND combo, EDataFlow flow, bool phoneInput)
{
    ComboBox_ResetContent(combo);
    if (phoneInput) ComboBox_AddString(combo, L"Mobile Phone");
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* devices = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices)))
    {
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* device = nullptr;
            IPropertyStore* store = nullptr;
            PROPVARIANT name; PropVariantInit(&name);
            if (SUCCEEDED(devices->Item(i, &device)) && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
                SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal)
                ComboBox_AddString(combo, name.pwszVal);
            PropVariantClear(&name);
            if (store) store->Release();
            if (device) device->Release();
        }
    }
    if (devices) devices->Release();
    if (enumerator) enumerator->Release();
    if (!phoneInput) ComboBox_AddString(combo, L"None");
}

static void AddGpuChoices(HWND combo)
{
    ComboBox_ResetContent(combo);
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
    {
        for (UINT i = 0; ; ++i)
        {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) ComboBox_AddString(combo, desc.Description);
            adapter->Release();
        }
        factory->Release();
    }
    if (ComboBox_GetCount(combo) == 0) ComboBox_AddString(combo, L"Default GPU");
    ComboBox_SetCurSel(combo, 0);
}

static std::wstring ComboText(HWND combo)
{
    int index = ComboBox_GetCurSel(combo);
    if (index == CB_ERR) return L"";
    int len = ComboBox_GetLBTextLen(combo, index);
    if (len < 0) return L"";
    std::wstring text((size_t)len + 1, L'\0');
    ComboBox_GetLBText(combo, index, &text[0]);
    text.resize((size_t)len);
    return text;
}

static void SelectComboText(HWND combo, const std::wstring& value, const wchar_t* fallback)
{
    int index = ComboBox_FindStringExact(combo, -1, value.c_str());
    if (index == CB_ERR && fallback) index = ComboBox_FindStringExact(combo, -1, fallback);
    ComboBox_SetCurSel(combo, index == CB_ERR ? 0 : index);
}

static std::wstring BrowseFolder(HWND owner, const std::wstring& initial)
{
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return L"";
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (!initial.empty())
    {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder))))
        {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    std::wstring result;
    if (SUCCEEDED(dialog->Show(owner)))
    {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            result = path;
            CoTaskMemFree(path);
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

static void PopulateVideoSizes(HWND page)
{
    HWND combo = GetDlgItem(page, 1102);
    ComboBox_ResetContent(combo);
    struct ModeSize { int w, h; const wchar_t* name; };
    const ModeSize rear[] = { { 3840,2160,L"3840 x 2160 (4K)" },{ 1920,1080,L"1920 x 1080" },{ 1280,720,L"1280 x 720" },{ 7680,4320,L"7680 x 4320 (8K)" } };
    const ModeSize front[] = { { 1920,1080,L"1920 x 1080" },{ 1280,720,L"1280 x 720" } };
    const ModeSize* modes = g_sel[0] == 1 ? front : rear;
    const int count = g_sel[0] == 1 ? 2 : 4;
    int selected = 0;
    for (int i = 0; i < count; ++i)
    {
        int at = ComboBox_AddString(combo, modes[i].name);
        ComboBox_SetItemData(combo, at, MAKELONG(modes[i].w, modes[i].h));
        if (modes[i].w == g_videoWidth && modes[i].h == g_videoHeight) selected = at;
    }
    ComboBox_SetCurSel(combo, selected);
}

static void PopulateVideoFps(HWND page, bool restoreCurrent)
{
    HWND sizeCombo = GetDlgItem(page, 1102);
    HWND fpsCombo = GetDlgItem(page, 1103);
    ComboBox_ResetContent(fpsCombo);
    DWORD size = (DWORD)ComboBox_GetItemData(sizeCombo, ComboBox_GetCurSel(sizeCombo));
    const int width = LOWORD(size), height = HIWORD(size);
    struct FpsMode { int fps; bool highSpeed; };
    FpsMode rear4k[] = { { 30,false },{ 60,false } };
    FpsMode rear8k[] = { { 24,false } };
    FpsMode rear1080[] = { { 15,false },{ 24,false },{ 30,false },{ 60,false },{ 120,true },{ 240,true },{ 480,true },{ 960,true } };
    FpsMode rear720[] = { { 15,false },{ 24,false },{ 30,false },{ 60,false },{ 120,true },{ 240,true },{ 480,true },{ 960,true },{ 1920,true } };
    FpsMode front1080[] = { { 60,false } };
    FpsMode front720[] = { { 120,true } };
    FpsMode* modes;
    int count;
    if (height >= 4320) { modes = rear8k; count = 1; }
    else if (height == 2160) { modes = rear4k; count = 2; }
    else if (g_sel[0] == 1 && height == 1080) { modes = front1080; count = 1; }
    else if (g_sel[0] == 1) { modes = front720; count = 1; }
    else if (height == 1080) { modes = rear1080; count = _countof(rear1080); }
    else { modes = rear720; count = _countof(rear720); }
    int selected = 0;
    for (int i = 0; i < count; ++i)
    {
        wchar_t label[32]; swprintf_s(label, L"%d fps", modes[i].fps);
        int at = ComboBox_AddString(fpsCombo, label);
        ComboBox_SetItemData(fpsCombo, at, (DWORD_PTR)(modes[i].fps | (modes[i].highSpeed ? 0x10000 : 0)));
        if (modes[i].fps == 30) selected = at;
        if (restoreCurrent && modes[i].fps == g_videoFps && modes[i].highSpeed == g_videoHighSpeed) selected = at;
    }
    ComboBox_SetCurSel(fpsCombo, selected);
    DWORD_PTR value = ComboBox_GetItemData(fpsCombo, selected);
    g_videoFps = (int)(value & 0xffff);
    g_videoHighSpeed = (value & 0x10000) != 0;
}

static void SyncVideoControls(HWND page)
{
    PopulateVideoSizes(page);
    PopulateVideoFps(page, true);
}

static void SaveSettings(HWND)
{
    const std::wstring path = SettingsPath();
    auto putInt = [&](const wchar_t* section, const wchar_t* key, int value) { wchar_t s[24] = {}; if (_itow_s(value, s, _countof(s), 10) == 0) WritePrivateProfileStringW(section, key, s, path.c_str()); };
    auto putString = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) { WritePrivateProfileStringW(section, key, value.c_str(), path.c_str()); };
    const wchar_t* miscKeys[] = { L"RunAtStartup",L"MinimizeToTray",L"ConnectionAnimation",L"GenerateLogs",L"HardwareDecoding" };
    const wchar_t* hotkeyKeys[] = { L"PauseResume",L"Snapshot",L"Timelapse",L"Record",L"CancelRecord" };
    const wchar_t* shortcutKeys[] = { L"SwitchCamera",L"Beautify",L"Mirror",L"Flip" };
    const wchar_t* cameraKeys[] = { L"Shutter",L"ISO",L"EV",L"WhiteBalance",L"Focus",L"Zoom",L"Bokeh" };
    for (int i = 0; i < 5; ++i) putInt(L"Miscellaneous", miscKeys[i], g_miscChecks[i] ? BST_CHECKED : BST_UNCHECKED);
    g_hardwareDecoding = g_miscChecks[4];
    g_decoder = ComboText(GetDlgItem(g_settingPages[2], 1315));
    g_gpuIndex = ComboBox_GetCurSel(GetDlgItem(g_settingPages[2], 1310));
    putString(L"Miscellaneous", L"Decoder", g_decoder);
    putString(L"Miscellaneous", L"PreferredGPU", ComboText(GetDlgItem(g_settingPages[2], 1310)));
    putString(L"Miscellaneous", L"VideoRenderer", ComboText(GetDlgItem(g_settingPages[2], 1311)));
    putInt(L"Miscellaneous", L"HardwareDecoding", g_hardwareDecoding ? BST_CHECKED : BST_UNCHECKED);
    wchar_t savePath[MAX_PATH] = {}; GetWindowTextW(GetDlgItem(g_settingPages[2], 1312), savePath, MAX_PATH); g_mediaSavePath = savePath;
    putString(L"Miscellaneous", L"MediaSavePath", g_mediaSavePath);
    wchar_t deviceName[256] = {}; GetWindowTextW(GetDlgItem(g_settingPages[2], 1314), deviceName, 256);
    putString(L"Miscellaneous", L"DeviceName", deviceName);
    putString(L"Audio", L"RecordingDevice", ComboText(GetDlgItem(g_settingPages[0], 1106)));
    putString(L"Audio", L"PlaybackDevice", ComboText(GetDlgItem(g_settingPages[0], 1107)));
    putInt(L"Video", L"Orientation", ComboBox_GetCurSel(GetDlgItem(g_settingPages[0], 1101)));
    g_videoOrientation = ComboBox_GetCurSel(GetDlgItem(g_settingPages[0], 1101));
    putInt(L"Video", L"ThisSizeOnly", g_thisSizeOnly ? BST_CHECKED : BST_UNCHECKED);
    putInt(L"Video", L"Quality", ComboBox_GetCurSel(GetDlgItem(g_settingPages[0], 1104)));
    putInt(L"Video", L"Format", ComboBox_GetCurSel(GetDlgItem(g_settingPages[0], 1105)));
    HWND sizeCombo = GetDlgItem(g_settingPages[0], 1102); int sizeIndex = ComboBox_GetCurSel(sizeCombo);
    if (sizeIndex != CB_ERR) { DWORD size = (DWORD)ComboBox_GetItemData(sizeCombo, sizeIndex);g_videoWidth = LOWORD(size);g_videoHeight = HIWORD(size); }
    HWND fpsCombo = GetDlgItem(g_settingPages[0], 1103); int fpsIndex = ComboBox_GetCurSel(fpsCombo);
    if (fpsIndex != CB_ERR) { DWORD_PTR mode = ComboBox_GetItemData(fpsCombo, fpsIndex);g_videoFps = (int)(mode & 0xffff);g_videoHighSpeed = (mode & 0x10000) != 0; }
    if (g_sel[0] == 1) g_sel[1] = g_videoHeight == 1080 ? 2 : 1;
    else if (g_videoHeight == 480) g_sel[1] = 0;
    else if (g_videoHeight == 720) g_sel[1] = 1;
    else if (g_videoHeight == 1080) g_sel[1] = 2;
    else if (g_videoHeight == 2160) g_sel[1] = 3;
    else if (g_videoHeight == 4320) g_sel[1] = 4;
    const wchar_t** availableChoices = nullptr;
    const int availableCount = MainFpsChoices(availableChoices);
    g_sel[2] = 0;
    for (int i = 0; i < availableCount; ++i) if (MainFpsValue(i) == g_videoFps) { g_sel[2] = i; break; }
    g_videoFps = MainFpsValue(g_sel[2]);
    g_videoHighSpeed = MainFpsIsHighSpeed(g_videoFps);
    putInt(L"Video", L"Width", g_videoWidth); putInt(L"Video", L"Height", g_videoHeight); putInt(L"Video", L"FPS", g_videoFps); putInt(L"Video", L"HighSpeed", g_videoHighSpeed ? 1 : 0);
    g_runAtStartup = g_miscChecks[0];
    g_minimizeToTray = g_miscChecks[1];
    g_connectionAnimation = g_miscChecks[2];
    g_generateLogs = g_miscChecks[3];
    SetRunAtStartup(g_runAtStartup);
    for (int i = 0; i < 5; ++i) { wchar_t text[128] = {}; GetWindowTextW(GetDlgItem(g_settingPages[1], 1200 + i), text, 128); WritePrivateProfileStringW(L"Hotkeys", hotkeyKeys[i], text, path.c_str()); }
    for (int i = 0; i < 4; ++i) { wchar_t text[128] = {}; GetWindowTextW(GetDlgItem(g_settingPages[1], 1210 + i), text, 128); WritePrivateProfileStringW(L"Hotkeys", shortcutKeys[i], text, path.c_str()); }
    for (int i = 0; i < 7; ++i) putInt(L"Camera", cameraKeys[i], (int)SendMessageW(GetDlgItem(g_settingPages[3], 1400 + i), TBM_GETPOS, 0, 0));
    putInt(L"Camera", L"ExposureAuto", g_exposureAuto ? BST_CHECKED : BST_UNCHECKED);
    putInt(L"Camera", L"WhiteBalanceAuto", g_whiteBalanceAuto ? BST_CHECKED : BST_UNCHECKED);
    putInt(L"Camera", L"FocusAuto", g_focusAuto ? BST_CHECKED : BST_UNCHECKED);
    for (int i = 0; i < 7; ++i)
    {
        int value = (int)SendMessageW(GetDlgItem(g_settingPages[3], 1400 + i), TBM_GETPOS, 0, 0);
        if (i == 0)g_shutter = value;else if (i == 1)g_iso = value;else if (i == 2)g_ev = value;else if (i == 3)g_whiteBalance = value;else if (i == 4)g_focus = value;else if (i == 5)g_zoomValue = value;
        if (g_camRunning && g_camera) { if (i == 0 && !g_exposureAuto) { g_camera->SetCameraParam(2, 0);g_camera->SetCameraParam(3, 1000000000 / max(value, 1)); } else if (i == 1 && !g_exposureAuto)g_camera->SetCameraParam(4, value);else if (i == 4 && !g_focusAuto) { g_camera->SetCameraParam(0, 0);g_camera->SetCameraParam(1, value * 1000); } }
    }
    if (g_camRunning && g_hasActiveCameraConfig)
    {
        const CameraConfig requested = ReadConfig();
        const bool resolutionChanged =
            requested.width != g_activeCameraConfig.width ||
            requested.height != g_activeCameraConfig.height;
        if (resolutionChanged) RestartCamera();
    }
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

static void LoadSettings(HWND)
{
    // Loading saved control text also emits EN_CHANGE. Treat it as population,
    // not as user input: otherwise opening Settings sends stale manual values
    // to the live camera and can change focus/exposure immediately.
    const bool wasUpdatingSettings = g_updatingSettings;
    g_updatingSettings = true;
    const bool preserveLiveCamera = g_camRunning && g_hasActiveCameraConfig;
    const CameraConfig liveCamera = g_activeCameraConfig;
    const int liveCameraValues[] = { g_shutter, g_iso, g_ev, g_whiteBalance, g_focus, g_zoomValue, 0 };
    const std::wstring path = SettingsPath();
    auto getInt = [&](const wchar_t* key, int fallback) { return (int)GetPrivateProfileIntW(L"Camera", key, fallback, path.c_str()); };
    const wchar_t* miscKeys[] = { L"RunAtStartup",L"MinimizeToTray",L"ConnectionAnimation",L"GenerateLogs",L"HardwareDecoding" };
    const int miscDefaults[] = { BST_UNCHECKED,BST_CHECKED,BST_CHECKED,BST_UNCHECKED,BST_CHECKED };
    for (int i = 0;i < 5;++i) g_miscChecks[i] = GetPrivateProfileIntW(L"Miscellaneous", miscKeys[i], miscDefaults[i], path.c_str()) == BST_CHECKED;
    if (preserveLiveCamera) g_miscChecks[4] = liveCamera.hardwareDecoding;
    g_runAtStartup = g_miscChecks[0]; g_minimizeToTray = g_miscChecks[1];
    g_connectionAnimation = g_miscChecks[2]; g_generateLogs = g_miscChecks[3];
    g_hardwareDecoding = g_miscChecks[4];
    for (int i = 0;i < 5;++i) InvalidateRect(GetDlgItem(g_settingPages[2], 1300 + i), nullptr, TRUE);
    wchar_t stringValue[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Miscellaneous", L"MediaSavePath", g_mediaSavePath.c_str(), stringValue, MAX_PATH, path.c_str());
    g_mediaSavePath = stringValue; SetWindowTextW(GetDlgItem(g_settingPages[2], 1312), g_mediaSavePath.c_str());
    GetPrivateProfileStringW(L"Miscellaneous", L"DeviceName", L"e2eSoft iVCam", stringValue, MAX_PATH, path.c_str()); SetWindowTextW(GetDlgItem(g_settingPages[2], 1314), stringValue);
    if (preserveLiveCamera) { SelectComboText(GetDlgItem(g_settingPages[2], 1315), liveCamera.decoder.c_str(), L"Default"); }
    else { GetPrivateProfileStringW(L"Miscellaneous", L"Decoder", L"Default", stringValue, MAX_PATH, path.c_str()); SelectComboText(GetDlgItem(g_settingPages[2], 1315), stringValue, L"Default"); }
    g_decoder = ComboText(GetDlgItem(g_settingPages[2], 1315));
    GetPrivateProfileStringW(L"Miscellaneous", L"PreferredGPU", L"", stringValue, MAX_PATH, path.c_str()); SelectComboText(GetDlgItem(g_settingPages[2], 1310), stringValue, L"Default GPU");
    GetPrivateProfileStringW(L"Miscellaneous", L"VideoRenderer", L"EVR (Default)", stringValue, MAX_PATH, path.c_str()); SelectComboText(GetDlgItem(g_settingPages[2], 1311), stringValue, L"EVR (Default)");
    EnableWindow(GetDlgItem(g_settingPages[2], 1315), g_hardwareDecoding);
    GetPrivateProfileStringW(L"Audio", L"RecordingDevice", L"Mobile Phone", stringValue, MAX_PATH, path.c_str()); SelectComboText(GetDlgItem(g_settingPages[0], 1106), stringValue, L"Mobile Phone");
    GetPrivateProfileStringW(L"Audio", L"PlaybackDevice", L"None", stringValue, MAX_PATH, path.c_str()); SelectComboText(GetDlgItem(g_settingPages[0], 1107), stringValue, L"None");
    if (preserveLiveCamera)
    {
        g_videoWidth = liveCamera.width; g_videoHeight = liveCamera.height;
        g_videoFps = liveCamera.fps; g_videoHighSpeed = liveCamera.highSpeed;
        static const int rotations[] = { 0, 90, 180, 270 };
        g_videoOrientation = 0;
        for (int i = 0; i < 4; ++i) if (rotations[i] == liveCamera.rotation) { g_videoOrientation = i; break; }
    }
    else
    {
        g_videoWidth = GetPrivateProfileIntW(L"Video", L"Width", g_videoWidth, path.c_str()); g_videoHeight = GetPrivateProfileIntW(L"Video", L"Height", g_videoHeight, path.c_str());
        g_videoFps = GetPrivateProfileIntW(L"Video", L"FPS", g_videoFps, path.c_str()); g_videoHighSpeed = GetPrivateProfileIntW(L"Video", L"HighSpeed", g_videoHighSpeed ? 1 : 0, path.c_str()) != 0;
        g_videoOrientation = GetPrivateProfileIntW(L"Video", L"Orientation", 0, path.c_str());
    }
    HWND orientation = GetDlgItem(g_settingPages[0], 1101); int orientationIndex = g_videoOrientation; ComboBox_SetCurSel(orientation, orientationIndex);
    g_thisSizeOnly = GetPrivateProfileIntW(L"Video", L"ThisSizeOnly", BST_CHECKED, path.c_str()) == BST_CHECKED;
    HWND sizeOnly = GetDlgItem(g_settingPages[0], 1108); InvalidateRect(sizeOnly, nullptr, TRUE);
    ComboBox_SetCurSel(GetDlgItem(g_settingPages[0], 1104), GetPrivateProfileIntW(L"Video", L"Quality", 1, path.c_str())); ComboBox_SetCurSel(GetDlgItem(g_settingPages[0], 1105), GetPrivateProfileIntW(L"Video", L"Format", 0, path.c_str()));
    SyncVideoControls(g_settingPages[0]);
    if (g_videoHeight == 480) g_sel[1] = 0;
    else if (g_videoHeight == 720) g_sel[1] = 1;
    else if (g_videoHeight == 1080) g_sel[1] = 2;
    else if (g_videoHeight == 2160 && g_sel[0] == 0) g_sel[1] = 3;
    else if (g_videoHeight == 4320 && g_sel[0] == 0) g_sel[1] = 4;
    const wchar_t** fpsChoices = nullptr; int availableFps = MainFpsChoices(fpsChoices);
    g_sel[2] = 0;
    for (int i = 0; i < availableFps; ++i) if (MainFpsValue(i) == g_videoFps) { g_sel[2] = i; break; }
    g_videoFps = MainFpsValue(g_sel[2]); g_videoHighSpeed = MainFpsIsHighSpeed(g_videoFps);
    const wchar_t* hotkeys[] = { L"PauseResume",L"Snapshot",L"Timelapse",L"Record",L"CancelRecord" };
    for (int i = 0;i < 5;++i) { wchar_t value[128] = {};GetPrivateProfileStringW(L"Hotkeys", hotkeys[i], L"", value, 128, path.c_str());if (value[0])SetWindowTextW(GetDlgItem(g_settingPages[1], 1200 + i), value); }
    const wchar_t* shortcuts[] = { L"SwitchCamera",L"Beautify",L"Mirror",L"Flip" };
    for (int i = 0;i < 4;++i) { wchar_t value[128] = {};GetPrivateProfileStringW(L"Hotkeys", shortcuts[i], L"", value, 128, path.c_str());if (value[0])SetWindowTextW(GetDlgItem(g_settingPages[1], 1210 + i), value); }
    const int defaults[] = { 300,6400,0,5000,16,6,0 }; const wchar_t* cameraKeys[] = { L"Shutter",L"ISO",L"EV",L"WhiteBalance",L"Focus",L"Zoom",L"Bokeh" };
    for (int i = 0;i < 7;++i) { int value = preserveLiveCamera ? liveCameraValues[i] : getInt(cameraKeys[i], defaults[i]);HWND slider = GetDlgItem(g_settingPages[3], 1400 + i);SendMessageW(slider, TBM_SETPOS, TRUE, value);wchar_t n[24] = {};if (_itow_s(value, n, _countof(n), 10) == 0)SetWindowTextW(GetDlgItem(g_settingPages[3], 1410 + i), n);if (i == 0)g_shutter = value;else if (i == 1)g_iso = value;else if (i == 2)g_ev = value;else if (i == 3)g_whiteBalance = value;else if (i == 4)g_focus = value;else if (i == 5)g_zoomValue = value; }
    if (!preserveLiveCamera)
    {
        g_exposureAuto = GetPrivateProfileIntW(L"Camera", L"ExposureAuto", BST_CHECKED, path.c_str()) == BST_CHECKED;
        g_whiteBalanceAuto = GetPrivateProfileIntW(L"Camera", L"WhiteBalanceAuto", BST_CHECKED, path.c_str()) == BST_CHECKED;
        g_focusAuto = GetPrivateProfileIntW(L"Camera", L"FocusAuto", BST_CHECKED, path.c_str()) == BST_CHECKED;
    }
    SendMessageW(GetDlgItem(g_settingPages[3], 1430), BM_SETCHECK, g_exposureAuto ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(g_settingPages[3], 1433), BM_SETCHECK, g_whiteBalanceAuto ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(g_settingPages[3], 1434), BM_SETCHECK, g_focusAuto ? BST_CHECKED : BST_UNCHECKED, 0);
    g_updatingSettings = wasUpdatingSettings;
}

static void ShowSettings()
{
    if (g_settings && IsWindow(g_settings)) { SetForegroundWindow(g_settings); return; }
    static bool registered = false;
    if (!registered) { WNDCLASSW wc = {}; wc.lpfnWndProc = SettingsProc; wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = CreateSolidBrush(kDlgBg); wc.lpszClassName = L"PhoneCamSettings"; RegisterClassW(&wc); registered = true; }
    if (!g_settingsFont) g_settingsFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HWND d = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PhoneCamSettings", L"PhoneCam Settings - Xiaomi 12 Pro", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1408, 741, g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_settings = d;
    g_settingTabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_CLIPSIBLINGS | TCS_FOCUSNEVER, 14, 8, 1080, 84, d, nullptr, GetModuleHandleW(nullptr), nullptr);
    const wchar_t* names[] = { L"General", L"Hotkeys", L"Miscellaneous", L"Avancerade kamerainställningar" };
    for (int i = 0; i < 4; ++i) { TCITEMW ti = {}; ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)names[i]; TabCtrl_InsertItem(g_settingTabs, i, &ti); }
    const int tabX[] = { 55, 170, 300, 468 };
    const int tabW[] = { 100, 120, 170, 370 };
    for (int i = 0; i < 4; ++i)
    {
        g_settingTabButtons[i] = CreateWindowW(L"BUTTON", names[i], WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            tabX[i], 47, tabW[i], 42, d, (HMENU)(INT_PTR)(1600 + i), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_settingTabButtons[i], WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
    }
    CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 20, 88, 1360, 2, d, (HMENU)1702, GetModuleHandleW(nullptr), nullptr);
    HWND page = SettingPage(d, 0);
    SettingControl(page, L"STATIC", L"Video", SS_OWNERDRAW | WS_CLIPSIBLINGS, 12, 6, 548, 190, 1700);
    SettingControl(page, L"STATIC", L"Video Orientation:", SS_RIGHT, 28, 32, 150, 22); HWND combo = SettingControl(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 190, 30, 190, 140, 1101); for (const wchar_t* v : { L"Landscape",L"Portrait",L"Landscape (flipped)",L"Portrait (flipped)" }) ComboBox_AddString(combo, v); ComboBox_SetCurSel(combo, 0);
    SettingControl(page, L"STATIC", L"Video Size:", SS_RIGHT, 28, 60, 150, 22); combo = SettingControl(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 190, 58, 190, 120, 1102);
    SettingControl(page, L"BUTTON", L"This size only", BS_OWNERDRAW | WS_TABSTOP, 190, 84, 150, 22, 1108);
    SettingControl(page, L"STATIC", L"Video FPS:", SS_RIGHT, 28, 108, 150, 22); combo = SettingControl(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 190, 106, 190, 120, 1103);
    SettingControl(page, L"STATIC", L"Video Quality:", SS_RIGHT, 28, 136, 150, 22); combo = SettingControl(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 190, 134, 190, 120, 1104); for (const wchar_t* v : { L"Low",L"Medium",L"High" }) ComboBox_AddString(combo, v); ComboBox_SetCurSel(combo, 1);
    SettingControl(page, L"STATIC", L"Video Format:", SS_RIGHT, 28, 164, 150, 22); combo = SettingControl(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 190, 162, 190, 120, 1105); for (const wchar_t* v : { L"Default",L"YUY2",L"RGB24",L"ARGB32" }) ComboBox_AddString(combo, v); ComboBox_SetCurSel(combo, 0);
    SettingControl(page, L"STATIC", L"Audio", SS_OWNERDRAW | WS_CLIPSIBLINGS, 12, 204, 548, 100, 1700);
    SettingControl(page, L"STATIC", L"Recording Device:", SS_RIGHT, 28, 232, 150, 22); combo = SettingControl(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 190, 230, 300, 120, 1106); AddAudioEndpointChoices(combo, eCapture, true); ComboBox_SetCurSel(combo, 0);
    SettingControl(page, L"STATIC", L"Playback Device:", SS_RIGHT, 28, 264, 150, 22); combo = SettingControl(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 190, 262, 300, 120, 1107); AddAudioEndpointChoices(combo, eRender, false); ComboBox_SetCurSel(combo, 0);
    page = SettingPage(d, 1);
    SettingControl(page, L"STATIC", L"Global Hotkeys", SS_OWNERDRAW | WS_CLIPSIBLINGS, 12, 6, 548, 165, 1700);
    const wchar_t* hotkeys[] = { L"Pause/Resume Video",L"Snapshot",L"Time-lapse Snapshot",L"Start/Stop Video Recording",L"Cancel Video Recording" }; const wchar_t* keys[] = { L"Ctrl + Alt + P",L"Ctrl + Alt + S",L"Ctrl + Alt + T",L"Ctrl + Alt + R",L"Ctrl + Alt + Q" };
    for (int i = 0;i < 5;++i) { int x = (i == 0 || i % 2 == 1) ? 32 : 300;int y = i == 0 ? 34 : 82 + ((i - 1) / 2) * 48;SettingControl(page, L"STATIC", hotkeys[i], 0, x, y, 245, 20);SettingControl(page, L"EDIT", keys[i], ES_AUTOHSCROLL | WS_TABSTOP, x, y + 20, 210, 24, 1200 + i); }
    SettingControl(page, L"STATIC", L"Keyboard Shortcuts", SS_OWNERDRAW | WS_CLIPSIBLINGS, 12, 181, 548, 145, 1700);
    const wchar_t* shortcutNames[] = { L"Switch cameras",L"Beautify",L"Mirror",L"Flip" }; const wchar_t* shortcutKeys[] = { L"S",L"B",L"M",L"F" };
    for (int i = 0;i < 4;++i) { int x = (i % 2) ? 300 : 32;int y = 208 + (i / 2) * 54;SettingControl(page, L"STATIC", shortcutNames[i], 0, x, y, 245, 20);SettingControl(page, L"EDIT", shortcutKeys[i], ES_AUTOHSCROLL | WS_TABSTOP, x, y + 20, 210, 24, 1210 + i); }
    page = SettingPage(d, 2);
    SettingControl(page, L"STATIC", L"Miscellaneous", SS_OWNERDRAW | WS_CLIPSIBLINGS, 12, 6, 548, 325, 1700);
    const wchar_t* checks[] = { L"Run at Startup",L"Minimize to Tray",L"Show Connection Waiting Animation",L"Generate Logs",L"Hardware Decoding" };
    for (int i = 0;i < 5;++i)SettingControl(page, L"BUTTON", checks[i], BS_OWNERDRAW | WS_TABSTOP, 28, 34 + i * 30, 340, 24, 1300 + i);
    SettingControl(page, L"STATIC", L"Hardware decoder:", 0, 28, 184, 170, 22);combo = SettingControl(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 200, 182, 300, 120, 1315);for (const wchar_t* v : { L"Default",L"CUDA",L"D3D11",L"DXVA2",L"Intel QuickSync" })ComboBox_AddString(combo, v);ComboBox_SetCurSel(combo, 0);
    SettingControl(page, L"STATIC", L"Preferred GPU:", 0, 28, 212, 170, 22);combo = SettingControl(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 200, 210, 300, 120, 1310);AddGpuChoices(combo);
    SettingControl(page, L"STATIC", L"Video Renderer:", 0, 28, 240, 170, 22);combo = SettingControl(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 200, 238, 300, 120, 1311);for (const wchar_t* v : { L"EVR (Default)",L"Legacy",L"VMR-7",L"VMR-9",L"madVR" })ComboBox_AddString(combo, v);ComboBox_SetCurSel(combo, 0);
    SettingControl(page, L"STATIC", L"Media Save Path:", 0, 28, 268, 165, 22);SettingControl(page, L"EDIT", g_mediaSavePath.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 200, 266, 300, 24, 1312);SettingControl(page, L"BUTTON", L"...", BS_OWNERDRAW | WS_TABSTOP, 506, 266, 32, 24, 1313);
    SettingControl(page, L"STATIC", L"Device Name:", 0, 28, 296, 165, 22);SettingControl(page, L"EDIT", L"e2eSoft iVCam", ES_AUTOHSCROLL | WS_TABSTOP, 200, 294, 300, 24, 1314);
    page = SettingPage(d, 3);
    const wchar_t* camLabels[] = { L"Shutter",L"ISO",L"Exposure compensation",L"White balance",L"Focus",L"Zoom",L"Bokeh / Green screen" }; const int camValues[] = { 300,6400,0,5000,16,6,0 }; const int camMin[] = { 1,50,-4,2000,0,0,0 }; const int camMax[] = { 1000,6400,4,10000,16,12,100 };
    for (int i = 0;i < 7;++i) { int y = 12 + i * 38;SettingControl(page, L"STATIC", L"", SS_OWNERDRAW | WS_CLIPSIBLINGS, 20, y - 4, 615, 34, 1700);SettingControl(page, L"STATIC", camLabels[i], 0, 30, y, 128, 24);HWND t = SettingControl(page, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 244, y, 305, 20, 1400 + i);SendMessageW(t, TBM_SETRANGE, TRUE, MAKELONG(camMin[i], camMax[i]));SendMessageW(t, TBM_SETPOS, TRUE, camValues[i]);HWND e = SettingControl(page, L"EDIT", std::to_wstring(camValues[i]).c_str(), WS_TABSTOP, 560, y, 54, 24, 1410 + i);HWND up = SettingControl(page, UPDOWN_CLASSW, L"", UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT, 614, y, 22, 24, 1420 + i);SendMessageW(up, UDM_SETBUDDY, (WPARAM)e, 0);SendMessageW(up, UDM_SETRANGE32, camMin[i], camMax[i]);if (i == 0 || i == 3 || i == 4)SettingControl(page, L"BUTTON", L"Auto", BS_OWNERDRAW | WS_TABSTOP, 177, y + 3, 100, 20, 1430 + i);if (i == 6) { EnableWindow(t, FALSE);EnableWindow(e, FALSE);EnableWindow(up, FALSE); } }
    SettingControl(page, L"STATIC", L"", SS_OWNERDRAW | WS_CLIPSIBLINGS, 20, 282, 615, 40, 1700);
    const wchar_t* tools[] = { L"▰",L"ϟ",L"▦",L"ⓘ",L"⛶",L"▣",L"▣",L"×" };for (int i = 0;i < 8;++i)SettingControl(page, L"BUTTON", tools[i], BS_OWNERDRAW | WS_TABSTOP, 38 + i * 70, 286, 62, 34, 1450 + i);
    SettingControl(page, L"BUTTON", L"OK", BS_OWNERDRAW | WS_TABSTOP, 565, 286, 63, 34, 1458);
    SettingControl(page, L"STATIC", L"K", 0, 620, 12 + 3 * 38, 18, 24);
    ShowWindow(GetDlgItem(page, 1423), SW_HIDE);
    HWND innerOk = GetDlgItem(page, 1458); SendMessageW(innerOk, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
    CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 680, 900, 110, 42, d, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"BUTTON", L"Avbryt", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 800, 900, 110, 42, d, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"BUTTON", L"Verkställ", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 920, 900, 140, 42, d, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(d, GWLP_USERDATA, (LONG_PTR)g_settingTabs);
    g_selectedSettingsTab = 3;
    ShowWindow(g_settingTabs, SW_HIDE);
    for (int i = 0; i < 4; ++i) ShowWindow(g_settingPages[i], i == 3 ? SW_SHOW : SW_HIDE);
    SetWindowPos(GetDlgItem(d, IDOK), nullptr, 980, 660, 120, 40, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(d, IDCANCEL), nullptr, 1110, 660, 120, 40, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(d, 1001), nullptr, 1240, 660, 130, 40, SWP_NOZORDER | SWP_NOACTIVATE);
    LoadSettings(d);
    BOOL darkTitle = TRUE;
    DwmSetWindowAttribute(d, 20, &darkTitle, sizeof(darkTitle));
    DwmSetWindowAttribute(d, 19, &darkTitle, sizeof(darkTitle));
    COLORREF capCol = RGB(30, 30, 36), capTxt = RGB(235, 235, 242), capBrd = RGB(60, 60, 70);
    DwmSetWindowAttribute(d, 35, &capCol, sizeof(capCol));
    DwmSetWindowAttribute(d, 36, &capTxt, sizeof(capTxt));
    DwmSetWindowAttribute(d, 34, &capBrd, sizeof(capBrd));
    EnumChildWindows(d, ThemeSettingsChild, 0);
    ShowWindow(d, SW_SHOW);
    UpdateWindow(d);
}

// ---------------------------------------------------------------- hjälpfunktioner
static void Log(const std::wstring& msg)
{
    if (g_generateLogs.load())
    {
        std::lock_guard<std::mutex> lock(g_fileLogMutex);
        const std::wstring logPath = proc::ExeDir() + L"\\PhoneCam.log";
        HANDLE file = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            SYSTEMTIME now; GetLocalTime(&now);
            wchar_t line[32]; swprintf_s(line, L"[%04u-%02u-%02u %02u:%02u:%02u] ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
            std::wstring entry = line + msg + L"\r\n";
            const int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, entry.c_str(), (int)entry.size(), nullptr, 0, nullptr, nullptr);
            if (bytesNeeded > 0)
            {
                std::vector<char> utf8((size_t)bytesNeeded);
                WideCharToMultiByte(CP_UTF8, 0, entry.c_str(), (int)entry.size(), utf8.data(), bytesNeeded, nullptr, nullptr);
                DWORD written = 0;
                WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
            }
            CloseHandle(file);
        }
    }
    HWND w = g_wnd;
    if (w) PostMessageW(w, WM_LOG, 0, (LPARAM)new std::wstring(msg));
}

static void OnFrame(const unsigned char* d, int w, int h)
{
    // Previewen behöver inte lagra full 4K/8K. Begränsa kopian till 960x540
    // för att undvika hundratals MB allokering/kopiering per sekund i UI:t.
    const double scale = min(1.0, min(960.0 / w, 540.0 / h));
    const int previewW = max(1, (int)(w * scale));
    const int previewH = max(1, (int)(h * scale));
    static std::vector<unsigned char> preview;
    preview.resize((size_t)previewW * previewH * 3);
    if (previewW == w && previewH == h)
        memcpy(preview.data(), d, preview.size());
    else
    {
        for (int y = 0; y < previewH; ++y)
        {
            const int sy = (int)((int64_t)y * h / previewH);
            for (int x = 0; x < previewW; ++x)
            {
                const int sx = (int)((int64_t)x * w / previewW);
                memcpy(preview.data() + ((size_t)y * previewW + x) * 3,
                    d + ((size_t)sy * w + sx) * 3, 3);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_frameMx);
        g_frame.assign(preview.begin(), preview.end());
        g_fw = previewW;
        g_fh = previewH;
    }
    HWND wnd = g_wnd;
    if (wnd)
    {
        RECT r = ImgR();
        InvalidateRect(wnd, &r, FALSE);
    }
}

static void LayoutScrcpy()
{
    if (!g_scr) return;
    RECT r = RightR();
    SetWindowPos(g_scr, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | SWP_SHOWWINDOW);
}

static CameraConfig ReadConfig()
{
    CameraConfig c;
    c.serial = g_serial;
    c.facing = g_sel[0] == 1 ? "front" : "back";
    c.width = g_videoWidth;
    c.height = g_videoHeight;
    c.fps = g_videoFps;
    static const int rotations[] = { 0, 90, 180, 270 };
    c.rotation = rotations[max(0, min(3, g_videoOrientation))];
    c.highSpeed = g_videoHighSpeed;
    c.hardwareDecoding = g_hardwareDecoding;
    c.decoder = g_decoder;
    c.mirror = g_mirrorImage;
    c.flip = g_flipImage;
    c.beautify = g_beautify;
    c.shutterSpeed = g_shutter;
    c.iso = g_iso;
    c.focus = g_focus;
    c.zoom = g_zoomValue;
    c.exposureAuto = g_exposureAuto;
    c.focusAuto = g_focusAuto;
    c.whiteBalanceAuto = g_whiteBalanceAuto;
    c.ev = g_ev;
    c.whiteBalanceKelvin = g_whiteBalance;
    return c;
}

static void RestartCamera()
{
    if (!g_camera || !g_camRunning || !g_connected) return;
    CameraConfig cfg = ReadConfig();
    g_camera->Stop();
    g_camera->Start(cfg);
    g_activeCameraConfig = cfg;
    g_hasActiveCameraConfig = true;
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

static void ToggleRotationLock()
{
    if (g_serial.empty()) return;
    const std::wstring adbExe = L"\"" + proc::ExeDir() + L"\\tools\\scrcpy\\adb.exe\" -s " + proc::ToWide(g_serial) + L" shell ";
    std::string out;
    if (!g_rotationLocked)
    {
        if (!proc::RunCapture(adbExe + L"settings get system accelerometer_rotation", proc::ExeDir(), out, 4000)) { g_msg = L"Kunde inte läsa rotationsinställningen"; return; }
        g_originalAccelerometer = out.find('0') != std::string::npos ? "0" : "1";
        if (g_originalAccelerometer != "0" && !proc::RunCapture(adbExe + L"settings put system accelerometer_rotation 0", proc::ExeDir(), out, 4000)) { g_msg = L"Kunde inte låsa skärmrotationen"; return; }
        g_rotationLocked = true;
        g_msg = L"Skärmrotation låst";
    }
    else
    {
        if (!proc::RunCapture(adbExe + L"settings put system accelerometer_rotation " + proc::ToWide(g_originalAccelerometer), proc::ExeDir(), out, 4000)) { g_msg = L"Kunde inte låsa upp skärmrotationen"; return; }
        g_rotationLocked = false;
        g_msg = L"Skärmrotation upplåst";
    }
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- dropdown
static HWND g_pop = nullptr;
static int g_popCombo = -1, g_popHover = -1;

static LRESULT CALLBACK PopProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    const wchar_t** fpsChoices = kRear720Fps;
    int n = 0;
    if (g_popCombo >= 0 && g_popCombo < _countof(kCount))
        n = g_popCombo == 2 ? MainFpsChoices(fpsChoices) :
            (g_popCombo == 1 && g_sel[0] == 1 ? 2 : kCount[g_popCombo]);
    int itemH = S(36), pad = S(4);
    switch (m)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c;
        GetClientRect(h, &c);
        {
            Graphics g(dc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            SolidBrush bg(Color(255, 44, 44, 52));
            g.FillRectangle(&bg, 0, 0, c.right, c.bottom);
            Pen bp(Color(255, 110, 110, 124), 1.0f);
            g.DrawRectangle(&bp, 0, 0, c.right - 1, c.bottom - 1);
            for (int i = 0; i < n; ++i)
            {
                RECT ir = { pad, pad + i * itemH, c.right - pad, pad + (i + 1) * itemH };
                if (i == g_popHover)
                {
                    SolidBrush hb(Color(255, 72, 72, 86));
                    g.FillRectangle(&hb, F(ir));
                }
                RECT tr = { ir.left + S(10), ir.top, ir.right, ir.bottom };
                bool sel = i == (g_popCombo == 1 && g_sel[0] == 1 ? g_sel[1] - 1 : g_sel[g_popCombo]);
                const wchar_t* itemText = L"";
                if (g_popCombo == 2)
                {
                    if (fpsChoices && fpsChoices[i]) itemText = fpsChoices[i];
                }
                else if (g_popCombo == 1 && g_sel[0] == 1)
                    itemText = kFrontRes[i];
                else if (g_popCombo >= 0 && g_popCombo < _countof(kItems) && kItems[g_popCombo] && kItems[g_popCombo][i])
                    itemText = kItems[g_popCombo][i];
                DrawTxt(g, itemText, g_f.text.get(), tr,
                    sel ? Color(255, 130, 196, 255) : Color(255, 235, 235, 242));
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        int y = GET_Y_LPARAM(l), x = GET_X_LPARAM(l);
        RECT c;
        GetClientRect(h, &c);
        int idx = (x >= 0 && x < c.right && y >= pad) ? (y - pad) / itemH : -1;
        if (idx >= n) idx = -1;
        if (idx != g_popHover) { g_popHover = idx; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        int y = GET_Y_LPARAM(l), x = GET_X_LPARAM(l);
        RECT c;
        GetClientRect(h, &c);
        if (x >= 0 && x < c.right && y >= pad)
        {
            int idx = (y - pad) / itemH;
            if (idx >= 0 && idx < n)
            {
                if (g_popCombo == 1 && g_sel[0] == 1) g_sel[1] = idx + 1;
                else g_sel[g_popCombo] = idx;
                if (g_popCombo == 0 && g_sel[0] == 1)
                {
                    if (g_sel[1] == 0) g_sel[1] = 1;
                    else if (g_sel[1] >= 3) g_sel[1] = 2;
                }
                if (g_popCombo == 0 || g_popCombo == 1)
                {
                    const wchar_t** choices = nullptr; int available = MainFpsChoices(choices);
                    if (g_sel[2] >= available) g_sel[2] = available - 1;
                }
                if (g_popCombo == 0 || g_popCombo == 1)
                {
                    static const int widths[] = { 640, 1280, 1920, 3840, 7680 };
                    static const int heights[] = { 480, 720, 1080, 2160, 4320 };
                    g_videoWidth = widths[g_sel[1]]; g_videoHeight = heights[g_sel[1]];
                    const wchar_t** choices = nullptr;
                    int available = MainFpsChoices(choices);
                    int selectedFps = -1;
                    for (int i = 0; i < available; ++i) if (MainFpsValue(i) == g_videoFps) { selectedFps = i; break; }
                    if (selectedFps < 0)
                    {
                        selectedFps = 0;
                        for (int i = 0; i < available; ++i) if (MainFpsValue(i) == 30) { selectedFps = i; break; }
                    }
                    g_sel[2] = selectedFps;
                    g_videoFps = MainFpsValue(selectedFps);
                    g_videoHighSpeed = MainFpsIsHighSpeed(g_videoFps);
                }
                else if (g_popCombo == 2)
                {
                    g_videoFps = MainFpsValue(g_sel[2]); g_videoHighSpeed = MainFpsIsHighSpeed(g_videoFps);
                }
            }
        }
        g_pop = nullptr;
        DestroyWindow(h);
        if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
    case WM_ACTIVATEAPP:
        if (m == WM_ACTIVATEAPP && w) break;
        if (g_pop == h) { g_pop = nullptr; DestroyWindow(h); }
        return 0;
    case WM_DESTROY:
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowDropdown(int combo)
{
    if (g_pop) { HWND p = g_pop; g_pop = nullptr; DestroyWindow(p); }
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg)
    {
        WNDCLASSW wc = {};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = PopProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"PhoneCamDrop";
        RegisterClassW(&wc);
        reg = true;
    }
    RECT cr = ComboR(combo);
    POINT pt = { cr.left, cr.bottom + S(4) };
    ClientToScreen(g_wnd, &pt);
    int w = cr.right - cr.left, h = kCount[combo] * S(36) + S(8);
    g_popCombo = combo;
    g_popHover = -1;
    g_pop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"PhoneCamDrop", L"",
        WS_POPUP, pt.x, pt.y, w, h, g_wnd, nullptr, inst, nullptr);
    ShowWindow(g_pop, SW_SHOWNOACTIVATE);
    SetCapture(g_pop);
}

// ---------------------------------------------------------------- enhetsövervakning
static void PollDevices()
{
    const std::wstring adb = proc::ExeDir() + L"\\tools\\scrcpy\\adb.exe";
    while (!g_quit)
    {
        auto devs = proc::ListDevices(adb);
        HWND w = g_wnd;
        if (g_quit || !w) break;
        PostMessageW(w, WM_DEVICE, 0, (LPARAM)new std::string(devs.empty() ? "" : devs[0]));
        for (int i = 0; i < 20 && !g_quit; ++i) Sleep(100);
    }
}

// ---------------------------------------------------------------- fönster
static void ApplyDarkTitle(HWND h)
{
    BOOL on = TRUE;
    DwmSetWindowAttribute(h, 20, &on, sizeof(on));
    DwmSetWindowAttribute(h, 19, &on, sizeof(on));
    COLORREF cap = RGB(30, 30, 36);
    DwmSetWindowAttribute(h, 35, &cap, sizeof(cap));
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_CREATE:
    {
        g_wnd = h;
        LoadMiscPreferences();
        SetTimer(h, 1, 400, nullptr);
        typedef UINT(WINAPI* GetDpiForWindow_t)(HWND);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto fn = user32 ? (GetDpiForWindow_t)GetProcAddress(user32, "GetDpiForWindow") : nullptr;
        if (fn) g_dpi = (int)fn(h);
        ApplyDarkTitle(h);

        g_mirror = std::make_unique<Mirror>(
            Log,
            [](bool r) { if (g_wnd) PostMessageW(g_wnd, WM_MIRROR_STATE, r, 0); },
            [](HWND c) { if (g_wnd) PostMessageW(g_wnd, WM_MIRROR_WND, 0, (LPARAM)c); });
        g_camera = std::make_unique<Camera>(
            Log,
            [](bool r) { if (g_wnd) PostMessageW(g_wnd, WM_CAMERA_STATE, r, 0); },
            OnFrame,
            [](const CameraStatus& status) { if (g_wnd) PostMessageW(g_wnd, WM_CAMERA_STATUS, 0, (LPARAM)new CameraStatus(status)); });
        g_poll = std::thread(PollDevices);
        return 0;
    }

    case WM_GETMINMAXINFO:
    {
        RECT r = { 0, 0, S(900), S(912) };
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), FALSE, 0);
        ((MINMAXINFO*)l)->ptMinTrackSize.x = r.right - r.left;
        ((MINMAXINFO*)l)->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_DPICHANGED:
    {
        g_dpi = HIWORD(w);
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SIZE:
        if (w == SIZE_MINIMIZED && g_minimizeToTray)
        {
            AddTrayIcon(h);
            ShowWindow(h, SW_HIDE);
            return 0;
        }
        g_cw = LOWORD(l);
        g_ch = HIWORD(l);
        LayoutScrcpy();
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_TIMER:
        if (w == 1 && !g_connected && g_connectionAnimation)
        {
            ++g_waitAnimationFrame;
            RECT status = StatusR();
            InvalidateRect(h, &status, FALSE);
        }
        return 0;

    case WM_TRAYICON:
    {
        const UINT event = (UINT)LOWORD(l);
        if (event == WM_LBUTTONDBLCLK || event == NIN_SELECT || event == NIN_KEYSELECT)
            RestoreFromTray(h);
        else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU)
            ShowTrayMenu(h);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        int cw = max(g_cw, 1), ch = max(g_ch, 1);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cw, ch);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Graphics g(mem);
            Paint(g, ps.rcPaint);
        }
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
            ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        int id = HitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (id && !Enabled(id)) id = 0;
        if (id != g_hover)
        {
            g_hover = id;
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hover) { g_hover = 0; InvalidateRect(h, nullptr, FALSE); }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && g_hover)
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN:
    {
        int id = HitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (!id || !Enabled(id)) return 0;
        if (id <= 3) ShowDropdown(id - 1);
        else if (id == 4)
        {
            g_activeCameraConfig = ReadConfig();
            g_hasActiveCameraConfig = true;
            g_camera->Start(g_activeCameraConfig);
        }
        else if (id == 5) g_camera->Stop();
        else if (id == 6) ShowSettings();
        return 0;
    }

    case WM_LOG:
    {
        std::unique_ptr<std::wstring> s((std::wstring*)l);
        g_msg = *s;
        const bool becameReady = g_msg.find(L"[Kamera] igang") != std::wstring::npos && !g_cameraReady;
        if (g_msg.find(L"[Kamera] igang") != std::wstring::npos) g_cameraReady = true;
        if (becameReady) InvalidateRect(h, nullptr, FALSE);
        else { RECT r = MsgR(); InvalidateRect(h, &r, FALSE); }
        return 0;
    }

    case WM_DEVICE:
    {
        std::unique_ptr<std::string> s((std::string*)l);
        bool had = !g_serial.empty();
        g_serial = *s;
        bool nowConnected = !g_serial.empty();
        if (!nowConnected && had)
        {
            g_camera->Stop();
            g_mirror->Stop();
        }
        else if (nowConnected && !g_mirror->Running())
        {
            g_mirror->Start(g_serial);
        }
        if (nowConnected != g_connected)
        {
            g_connected = nowConnected;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MIRROR_WND:
    {
        HWND c = (HWND)l;
        if (!c) { g_scr = nullptr; InvalidateRect(h, nullptr, FALSE); return 0; }
        LONG_PTR st = GetWindowLongPtrW(c, GWL_STYLE);
        st &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME);
        st |= WS_CHILD | WS_VISIBLE;
        SetWindowLongPtrW(c, GWL_STYLE, st);
        LONG_PTR ex = GetWindowLongPtrW(c, GWL_EXSTYLE);
        ex &= ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW);
        SetWindowLongPtrW(c, GWL_EXSTYLE, ex);
        SetParent(c, h);
        g_scr = c;
        LayoutScrcpy();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_MIRROR_STATE:
        if (!w)
        {
            g_scr = nullptr;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_CAMERA_STATE:
        g_camRunning = w != 0;
        if (!g_camRunning && (!g_camera || !g_camera->Running())) g_cameraReady = false;
        if (!g_camRunning && g_settings && (!g_camera || !g_camera->Running())) DestroyWindow(g_settings);
        if (!g_camRunning)
        {
            std::lock_guard<std::mutex> lk(g_frameMx);
            g_frame.clear();
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_CAMERA_STATUS:
    {
        std::unique_ptr<CameraStatus> status((CameraStatus*)l);
        if (status->exposureNs > 0 && g_exposureAuto)
            SetCameraReadback(0, (int)max<int64_t>(1, min<int64_t>(1000, llround(1000000000.0 / status->exposureNs))));
        if (status->iso > 0 && g_exposureAuto) SetCameraReadback(1, status->iso);
        if (g_exposureAuto) SetCameraReadback(2, status->ev);
        if (g_focusAuto && status->focusMilli >= 0) SetCameraReadback(4, status->focusMilli / 1000);
        if (g_whiteBalanceAuto && status->whiteBalanceKelvin > 0) SetCameraReadback(3, status->whiteBalanceKelvin);
        if (status->zoomTenths > 0) SetCameraReadback(5, status->zoomTenths);
        return 0;
    }

    case WM_DESTROY:
        g_quit = true;
        KillTimer(h, 1);
        RemoveTrayIcon(h);
        g_wnd = nullptr;
        if (g_pop) { HWND p = g_pop; g_pop = nullptr; DestroyWindow(p); }
        if (g_poll.joinable()) g_poll.join();
        g_camera.reset();
        g_mirror.reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int show)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) return 1;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icc);
    // Per-monitor DPI + tillåt inbäddning av fönster med annan DPI-medvetenhet (scrcpy).
    if (HMODULE u = GetModuleHandleW(L"user32.dll"))
    {
        typedef BOOL(WINAPI* SetCtx_t)(HANDLE);
        typedef int(WINAPI* SetHost_t)(int);
        if (auto f = (SetCtx_t)GetProcAddress(u, "SetProcessDpiAwarenessContext")) f((HANDLE)-4);
        if (auto f = (SetHost_t)GetProcAddress(u, "SetThreadDpiHostingBehavior")) f(1);
    }

    proc::InitJob();
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gsi, nullptr);

    HDC sdc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"PhoneCamWnd";
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S(1040), S(912) };
    AdjustWindowRectEx(&r, style, FALSE, 0);
    HWND hwnd = CreateWindowW(L"PhoneCamWnd", L"PhoneCam", style, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GdiplusShutdown(token);
    if (SUCCEEDED(comResult)) CoUninitialize();
    return (int)msg.wParam;
}
