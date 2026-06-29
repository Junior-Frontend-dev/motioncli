#include "core/wallpaper.h"
#include "core/config.h"
#include "core/monitors.h"
#include "resource.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <mfapi.h>
#include <mfplay.h>
#include <propvarutil.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace motion {
namespace {

constexpr wchar_t kStopEventName[] = L"Local\\MotionCLI_StopEvent";
constexpr wchar_t kWindowClass[]   = L"MotionCLIWallpaperPane";
constexpr wchar_t kTrayClass[]     = L"MotionCLITray";
constexpr UINT WM_TRAY_CALLBACK    = WM_APP + 1;
constexpr UINT WM_PLAYER_EVENT     = WM_APP + 2;
constexpr UINT TRAY_ICON_ID        = 1;
constexpr UINT OCCLUSION_TIMER     = 1;
constexpr UINT GRACE_TIMER         = 2;
constexpr UINT HOST_TIMER          = 3;
constexpr UINT GRACE_MS            = 3000;

enum {
    IDM_MUTE = 1001,
    IDM_OPEN = 1002,
    IDM_STOP = 1003
};

static std::wofstream g_log;

void logLine(const wchar_t* msg) {
    if (!g_log.is_open()) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    g_log << L"[" << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"] "
          << msg << L"\n";
}

void logf(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    logLine(buf);
}

void openLog() {
    std::wstring logDir = Config::dataDir() + L"\\logs";
    CreateDirectoryW(logDir.c_str(), nullptr);
    std::wstring path = logDir + L"\\engine.log";
    g_log.open(path, std::ios::out | std::ios::trunc);
    logLine(L"=== MotionCLI Media Foundation engine started ===");
}

bool fileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring exePath() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

HICON appIcon() {
    return LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
}

struct Pane {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    RECT absRect{};
    bool isSpan = false;
    std::wstring media;
};

struct PaneRT;

class PlayerCallback final : public IMFPMediaPlayerCallback {
public:
    explicit PlayerCallback(PaneRT* pane) : m_refs(1), m_pane(pane) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++m_refs;
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG refs = --m_refs;
        if (refs == 0) delete this;
        return refs;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader) override;

private:
    std::atomic<ULONG> m_refs;
    PaneRT* m_pane;
};

struct PaneRT {
    HWND hwnd = nullptr;
    IMFPMediaPlayer* player = nullptr;
    PlayerCallback* callback = nullptr;
    RECT absRect{};
    bool isSpan = false;
    bool paused = false;
    bool muted = false;
    float rate = 1.0f;
    std::wstring media;
    bool fullyStopped = false;
    DWORD pauseTickMs = 0;
};

struct EngineState {
    std::vector<PaneRT> panes;
    HWND wallpaperHost = nullptr;
    bool muted = false;
    bool pauseOnFullscreen = true;
    bool pauseWhenMaximized = true;
    bool pauseUnlessDesktop = false;
    bool pauseOnBattery = false;
    bool lowEndMode = false;
    float playbackSpeed = 1.0f;
    bool occlusionActive = false;
    int occlusionTimeoutSec = 0;
};

HWND g_progman = nullptr;
HWND g_defviewHost = nullptr;
HWND g_workerW = nullptr;
HWND g_desktopDefView = nullptr;
HWND g_desktopListView = nullptr;

bool windowLooksFullscreen(HWND hwnd) {
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return false;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0) vw = GetSystemMetrics(SM_CXSCREEN);
    if (vh <= 0) vh = GetSystemMetrics(SM_CYSCREEN);

    int w = r.right - r.left;
    int h = r.bottom - r.top;
    return w >= vw / 2 && h >= vh / 2 && r.right > vx && r.bottom > vy;
}

BOOL CALLBACK findDesktopViewProc(HWND hwnd, LPARAM) {
    HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!defView || !IsWindowVisible(defView) || !windowLooksFullscreen(defView))
        return TRUE;

    HWND listView = FindWindowExW(defView, nullptr, L"SysListView32", L"FolderView");
    if (!listView) listView = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
    if (!listView || !IsWindowVisible(listView)) return TRUE;

    g_defviewHost = hwnd;
    g_desktopDefView = defView;
    g_desktopListView = listView;
    logf(L"Visible desktop view host=0x%p defview=0x%p listview=0x%p",
         g_defviewHost, g_desktopDefView, g_desktopListView);
    return FALSE;
}

BOOL CALLBACK findWorkerWProc(HWND hwnd, LPARAM) {
    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, _countof(cls));

    HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defView) {
        g_defviewHost = hwnd;

        HWND next = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
        if (next && !FindWindowExW(next, nullptr, L"SHELLDLL_DefView", nullptr)) {
            g_workerW = next;
            logf(L"Desktop icon host=0x%p wallpaper WorkerW=0x%p", g_defviewHost, g_workerW);
            return FALSE;
        }
    }

    if (!lstrcmpW(cls, L"WorkerW") && !defView) {
        if (IsWindowVisible(hwnd) && windowLooksFullscreen(hwnd)) {
            g_workerW = hwnd;
            logf(L"Visible wallpaper WorkerW candidate=0x%p", g_workerW);
            return FALSE;
        }
    }

    return TRUE;
}

bool isDesktopClass(HWND hwnd) {
    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, _countof(cls));
    return lstrcmpW(cls, L"Progman") == 0 || lstrcmpW(cls, L"WorkerW") == 0;
}

HWND findWallpaperHost() {
    g_progman = FindWindowW(L"Progman", nullptr);
    g_workerW = nullptr;
    g_defviewHost = nullptr;
    g_desktopDefView = nullptr;
    g_desktopListView = nullptr;

    EnumWindows(findDesktopViewProc, 0);

    if (g_progman) {
        const struct {
            WPARAM wp;
            LPARAM lp;
        } messages[] = {
            { 0, 0 },
            { 0x0D, 0 },
            { 0x0D, 1 },
            { 0, 1 },
        };

        for (const auto& m : messages) {
            DWORD_PTR unused = 0;
            SendMessageTimeoutW(g_progman, 0x052C, m.wp, m.lp,
                                SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000, &unused);
            Sleep(80);
            EnumWindows(findDesktopViewProc, 0);
            EnumWindows(findWorkerWProc, 0);
            if (g_workerW && IsWindow(g_workerW)) return g_workerW;
        }
    }

    for (int attempt = 0; attempt < 24; ++attempt) {
        g_workerW = nullptr;
        g_defviewHost = nullptr;
        g_desktopDefView = nullptr;
        g_desktopListView = nullptr;
        EnumWindows(findDesktopViewProc, 0);
        EnumWindows(findWorkerWProc, 0);
        if (g_workerW && IsWindow(g_workerW)) return g_workerW;
        Sleep(100);
    }

    if (g_progman && g_desktopDefView && IsWindow(g_desktopDefView)) {
        logLine(L"Using Progman child fallback behind SHELLDLL_DefView.");
        return g_progman;
    }

    logLine(L"No desktop host found; using no-parent diagnostic fallback.");
    return nullptr;
}

void restartPlayback(PaneRT& pane) {
    if (!pane.player) return;

    PROPVARIANT start;
    PropVariantInit(&start);
    start.vt = VT_I8;
    start.hVal.QuadPart = 0;
    pane.player->SetPosition(MFP_POSITIONTYPE_100NS, &start);
    PropVariantClear(&start);
    pane.player->Play();
}

bool startPlayerForPane(PaneRT& pane) {
    if (pane.player || !pane.hwnd) return true;

    pane.callback = new (std::nothrow) PlayerCallback(&pane);
    if (!pane.callback) return false;

    HRESULT hr = MFPCreateMediaPlayer(nullptr, FALSE, 0, pane.callback, pane.hwnd, &pane.player);
    if (SUCCEEDED(hr)) {
        hr = pane.player->CreateMediaItemFromURL(pane.media.c_str(), FALSE, 0, nullptr);
    }
    if (FAILED(hr)) {
        if (pane.player) { pane.player->Shutdown(); pane.player->Release(); pane.player = nullptr; }
        if (pane.callback) { pane.callback->Release(); pane.callback = nullptr; }
        return false;
    }
    pane.fullyStopped = false;
    return true;
}

void stopPlayerForPane(PaneRT& pane) {
    if (!pane.player) return;
    pane.player->Stop();
    pane.player->Shutdown();
    pane.player->Release();
    pane.player = nullptr;
    if (pane.callback) { pane.callback->Release(); pane.callback = nullptr; }
    pane.fullyStopped = true;
}

void setPanePaused(PaneRT& pane, bool paused) {
    if (pane.paused == paused && !pane.fullyStopped) return;

    if (paused) {
        if (pane.player && !pane.fullyStopped) {
            pane.player->Pause();
        }
        pane.paused = true;
        pane.pauseTickMs = GetTickCount();
    } else {
        if (pane.fullyStopped) {
            startPlayerForPane(pane);
        } else if (pane.player) {
            pane.player->Play();
        }
        pane.paused = false;
        pane.pauseTickMs = 0;
    }
}

void deepSleepPane(PaneRT& pane) {
    if (!pane.paused || pane.fullyStopped) return;
    stopPlayerForPane(pane);
}

void applyPaneSettings(PaneRT& pane) {
    if (!pane.player) return;
    pane.player->SetMute(pane.muted ? TRUE : FALSE);
    if (pane.rate > 0.0f && pane.rate != 1.0f)
        pane.player->SetRate(pane.rate);
}

void PlayerCallback::OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader) {
    if (!eventHeader || !m_pane) return;

    if (FAILED(eventHeader->hrEvent)) {
        logf(L"MFPlay event failed type=%u hr=0x%08X", eventHeader->eEventType,
             (unsigned)eventHeader->hrEvent);
        return;
    }

    switch (eventHeader->eEventType) {
        case MFP_EVENT_TYPE_MEDIAITEM_CREATED: {
            auto* ev = MFP_GET_MEDIAITEM_CREATED_EVENT(eventHeader);
            HRESULT hr = m_pane->player->SetMediaItem(ev->pMediaItem);
            logf(L"MFPlay media item created hr=0x%08X", (unsigned)hr);
            break;
        }
        case MFP_EVENT_TYPE_MEDIAITEM_SET:
            m_pane->player->UpdateVideo();
            m_pane->player->SetAspectRatioMode(MFVideoARMode_PreservePicture);
            applyPaneSettings(*m_pane);
            if (!m_pane->paused) m_pane->player->Play();
            logLine(L"MFPlay media item set; playback started.");
            break;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            if (m_pane->hwnd) PostMessageW(m_pane->hwnd, WM_PLAYER_EVENT, 1, 0);
            break;
        case MFP_EVENT_TYPE_ERROR:
            logLine(L"MFPlay reported an error event.");
            break;
        default:
            break;
    }
}

LRESULT CALLBACK wallpaperWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* pane = reinterpret_cast<PaneRT*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (pane && pane->player && !pane->fullyStopped) pane->player->UpdateVideo();
            return 0;
        case WM_DISPLAYCHANGE:
            if (pane && pane->player && !pane->fullyStopped) pane->player->UpdateVideo();
            return 0;
        case WM_PLAYER_EVENT:
            if (pane && wp == 1 && !pane->paused) restartPlayback(*pane);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool probeForeground(RECT& monRect, bool& fullscreen, bool& maximized) {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindowVisible(fg) || isDesktopClass(fg)) return false;

    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, _countof(cls));
    if (!lstrcmpW(cls, kWindowClass) || !lstrcmpW(cls, kTrayClass)) return false;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
    if (!mon) return false;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;

    monRect = mi.rcMonitor;
    maximized = IsZoomed(fg) != 0;
    fullscreen = wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
                 wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    return true;
}

bool onBattery() {
    SYSTEM_POWER_STATUS ps{};
    return GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0;
}

void updateOcclusion(EngineState* st, HWND trayHwnd) {
    if (!st || !st->occlusionActive) return;

    RECT monRect{};
    bool fullscreen = false;
    bool maximized = false;
    bool haveFg = probeForeground(monRect, fullscreen, maximized);

    bool forceAll = (st->pauseUnlessDesktop && haveFg) ||
                    (st->pauseOnBattery && onBattery());

    bool allOccluded = true;
    DWORD now = GetTickCount();
    DWORD timeoutMs = st->occlusionTimeoutSec > 0 ? (DWORD)st->occlusionTimeoutSec * 1000u : 0;

    for (PaneRT& pane : st->panes) {
        bool occluded = forceAll;
        if (!occluded && haveFg) {
            POINT center{ (pane.absRect.left + pane.absRect.right) / 2,
                          (pane.absRect.top + pane.absRect.bottom) / 2 };
            bool onThisMonitor = pane.isSpan || PtInRect(&monRect, center);
            if (onThisMonitor) {
                if (fullscreen && (st->pauseOnFullscreen || st->lowEndMode)) occluded = true;
                if (maximized && (st->pauseWhenMaximized || st->lowEndMode)) occluded = true;
            }
        }

        if (occluded && !pane.paused) {
            setPanePaused(pane, true);
        } else if (!occluded && pane.paused) {
            setPanePaused(pane, false);
        } else if (occluded && pane.paused && timeoutMs > 0 && !pane.fullyStopped) {
            if (now - pane.pauseTickMs >= timeoutMs) {
                deepSleepPane(pane);
            }
        }

        if (!occluded) allOccluded = false;
    }

    if (trayHwnd) {
        UINT interval = allOccluded ? 3000 : (st->lowEndMode ? 300 : 700);
        KillTimer(trayHwnd, OCCLUSION_TIMER);
        SetTimer(trayHwnd, OCCLUSION_TIMER, interval, nullptr);
    }
}

void showTrayMenu(HWND hwnd, EngineState* st) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Motion CLI - live wallpaper");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (st && st->muted ? MF_CHECKED : 0), IDM_MUTE, L"Mute audio");
    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Open Motion CLI...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_STOP, L"Stop wallpaper");

    SetForegroundWindow(hwnd);
    POINT pt{};
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK trayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<EngineState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_TIMER:
            if (wp == OCCLUSION_TIMER) updateOcclusion(st, hwnd);
            else if (wp == GRACE_TIMER) {
                KillTimer(hwnd, GRACE_TIMER);
                if (st) {
                    st->occlusionActive = true;
                    for (PaneRT& p : st->panes) setPanePaused(p, false);
                }
            } else if (wp == HOST_TIMER && st && st->wallpaperHost && !IsWindow(st->wallpaperHost)) {
                logLine(L"Desktop host disappeared; stopping render process.");
                PostQuitMessage(0);
            }
            return 0;
        case WM_TRAY_CALLBACK:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU ||
                LOWORD(lp) == WM_LBUTTONUP) {
                showTrayMenu(hwnd, st);
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_MUTE:
                    if (st) {
                        st->muted = !st->muted;
                        for (PaneRT& p : st->panes) {
                            p.muted = st->muted;
                            applyPaneSettings(p);
                        }
                    }
                    return 0;
                case IDM_OPEN: {
                    std::wstring cmd = L"\"" + exePath() + L"\"";
                    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
                    mutableCmd.push_back(L'\0');
                    STARTUPINFOW si{};
                    si.cb = sizeof(si);
                    PROCESS_INFORMATION pi{};
                    if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                       CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                    }
                    return 0;
                }
                case IDM_STOP:
                    PostQuitMessage(0);
                    return 0;
                default:
                    return 0;
            }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

HWND createTray(HINSTANCE inst, EngineState* st, NOTIFYICONDATAW& nid) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = trayWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kTrayClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClass, L"MotionCLITray",
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return nullptr;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

    nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = appIcon();
    wcscpy_s(nid.szTip, L"Motion CLI - live wallpaper running");
    Shell_NotifyIconW(NIM_ADD, &nid);

    SetTimer(hwnd, OCCLUSION_TIMER, st && st->lowEndMode ? 300 : 700, nullptr);
    SetTimer(hwnd, HOST_TIMER, 5000, nullptr);
    SetTimer(hwnd, GRACE_TIMER, GRACE_MS, nullptr);
    return hwnd;
}

bool processAlive(unsigned long pid) {
    if (pid == 0) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;

    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

std::vector<Pane> buildPanes(const Config& cfg) {
    std::vector<Pane> panes;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (cfg.mode == WallpaperMode::PerMonitor) {
        for (const MonitorInfo& m : enumerateMonitors()) {
            std::wstring media = cfg.currentMediaPath;
            auto it = cfg.monitorAssignments.find(m.device);
            if (it != cfg.monitorAssignments.end()) media = it->second;
            if (media.empty() || !fileExists(media)) continue;

            Pane p;
            p.x = m.x - vx;
            p.y = m.y - vy;
            p.w = m.width;
            p.h = m.height;
            p.absRect = { m.x, m.y, m.x + m.width, m.y + m.height };
            p.media = media;
            panes.push_back(std::move(p));
        }
    } else if (!cfg.currentMediaPath.empty() && fileExists(cfg.currentMediaPath)) {
        int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (w <= 0) w = GetSystemMetrics(SM_CXSCREEN);
        if (h <= 0) h = GetSystemMetrics(SM_CYSCREEN);

        Pane p;
        p.x = 0;
        p.y = 0;
        p.w = w;
        p.h = h;
        p.absRect = { vx, vy, vx + w, vy + h };
        p.isSpan = true;
        p.media = cfg.currentMediaPath;
        panes.push_back(std::move(p));
    }

    return panes;
}

bool createWallpaperClass(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wallpaperWndProc;
    wc.hInstance = inst;
    wc.hIcon = appIcon();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kWindowClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool startPane(HINSTANCE inst, HWND host, const Pane& pane, EngineState& state) {
    bool hostIsProgmanFallback = host && host == g_progman && g_desktopDefView;
    HWND hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        kWindowClass,
        L"MotionCLI Wallpaper",
        (host ? WS_CHILD : WS_POPUP) | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        pane.x, pane.y, pane.w, pane.h,
        host, nullptr, inst, nullptr);

    if (!hwnd) {
        logf(L"CreateWindowEx failed err=%lu", GetLastError());
        return false;
    }

    if (!host) {
        SetWindowPos(hwnd, HWND_BOTTOM, pane.absRect.left, pane.absRect.top, pane.w, pane.h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else if (hostIsProgmanFallback) {
        SetWindowPos(hwnd, g_desktopDefView, pane.x, pane.y, pane.w, pane.h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        logf(L"Pane ordered behind desktop DefView: pane=0x%p defview=0x%p", hwnd, g_desktopDefView);
    } else {
        SetWindowPos(hwnd, HWND_BOTTOM, pane.x, pane.y, pane.w, pane.h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    state.panes.emplace_back();
    PaneRT& rt = state.panes.back();
    rt.hwnd = hwnd;
    rt.absRect = pane.absRect;
    rt.isSpan = pane.isSpan;
    rt.muted = state.muted;
    rt.rate = state.lowEndMode ? 0.5f : state.playbackSpeed;
    if (rt.rate < 0.25f || rt.rate > 4.0f) rt.rate = 1.0f;
    rt.media = pane.media;
    rt.callback = new (std::nothrow) PlayerCallback(&rt);

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&rt));

    if (!rt.callback) {
        logLine(L"Could not allocate MFPlay callback.");
        return false;
    }

    HRESULT hr = MFPCreateMediaPlayer(nullptr, FALSE, 0, rt.callback, hwnd, &rt.player);
    if (SUCCEEDED(hr)) {
        hr = rt.player->CreateMediaItemFromURL(pane.media.c_str(), FALSE, 0, nullptr);
    }

    logf(L"Pane start hr=0x%08X hwnd=0x%p x=%d y=%d w=%d h=%d media=%s",
         (unsigned)hr, hwnd, pane.x, pane.y, pane.w, pane.h, pane.media.c_str());

    if (FAILED(hr)) {
        if (rt.player) {
            rt.player->Shutdown();
            rt.player->Release();
            rt.player = nullptr;
        }
        if (rt.callback) {
            rt.callback->Release();
            rt.callback = nullptr;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        state.panes.pop_back();
        return false;
    }

    return true;
}

void shutdownPane(PaneRT& pane) {
    if (pane.player) {
        pane.player->Stop();
        pane.player->Shutdown();
        pane.player->Release();
        pane.player = nullptr;
    }
    if (pane.callback) {
        pane.callback->Release();
        pane.callback = nullptr;
    }
    if (pane.hwnd && IsWindow(pane.hwnd)) {
        SetWindowLongPtrW(pane.hwnd, GWLP_USERDATA, 0);
        DestroyWindow(pane.hwnd);
    }
    pane.hwnd = nullptr;
}

static bool launchAsShellUser(const std::wstring& cmd, DWORD& outPid, std::string& err) {
    HWND shell = GetShellWindow();
    DWORD shellPid = 0;
    if (shell) GetWindowThreadProcessId(shell, &shellPid);

    if (shellPid == 0) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                        shellPid = pe.th32ProcessID;
                        break;
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

    if (shellPid == 0) {
        err = "Could not find explorer.exe process.";
        return false;
    }

    HANDLE explorerProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, shellPid);
    if (explorerProc) {
        HANDLE shellToken = nullptr;
        if (OpenProcessToken(explorerProc, TOKEN_DUPLICATE | TOKEN_QUERY |
                             TOKEN_ASSIGN_PRIMARY, &shellToken)) {
            HANDLE dupToken = nullptr;
            if (DuplicateTokenEx(shellToken, MAXIMUM_ALLOWED, nullptr,
                                 SecurityImpersonation, TokenPrimary, &dupToken)) {
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
                mutableCmd.push_back(L'\0');

                BOOL ok = CreateProcessAsUserW(dupToken, nullptr, mutableCmd.data(),
                                               nullptr, nullptr, FALSE,
                                               DETACHED_PROCESS, nullptr, nullptr,
                                               &si, &pi);
                CloseHandle(dupToken);
                CloseHandle(shellToken);
                CloseHandle(explorerProc);

                if (ok) {
                    outPid = pi.dwProcessId;
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    return true;
                }
                goto fallback;
            }
            CloseHandle(shellToken);
        }
        CloseHandle(explorerProc);
    }

fallback:
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                       DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        outPid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    char buf[96];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "CreateProcess failed (%lu)", GetLastError());
    err = buf;
    return false;
}

} // namespace

int runEngineFromConfig() {
    openLog();

    Config cfg = Config::load();
    std::vector<Pane> panes = buildPanes(cfg);
    logf(L"Panes requested: %d", (int)panes.size());
    if (panes.empty()) {
        logLine(L"No playable panes; exiting.");
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        logf(L"CoInitializeEx failed hr=0x%08X", (unsigned)hr);
        return 1;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        logf(L"MFStartup failed hr=0x%08X", (unsigned)hr);
        CoUninitialize();
        return 1;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    HANDLE stopEvent = CreateEventW(nullptr, FALSE, FALSE, kStopEventName);

    EngineState state;
    state.muted = cfg.muteByDefault;
    state.pauseOnFullscreen = cfg.pauseOnFullscreen;
    state.pauseWhenMaximized = cfg.pauseWhenMaximized;
    state.pauseUnlessDesktop = cfg.pauseUnlessDesktop;
    state.pauseOnBattery = cfg.pauseOnBattery;
    state.lowEndMode = cfg.lowEndMode;
    state.playbackSpeed = (float)cfg.playbackSpeed;
    if (state.playbackSpeed < 0.25f || state.playbackSpeed > 4.0f) state.playbackSpeed = 1.0f;
    state.occlusionTimeoutSec = cfg.occlusionTimeoutSec;
    state.occlusionActive = false;
    state.wallpaperHost = findWallpaperHost();

    logf(L"Wallpaper host=0x%p progman=0x%p defviewHost=0x%p workerW=0x%p",
         state.wallpaperHost, g_progman, g_defviewHost, g_workerW);

    if (!createWallpaperClass(inst)) {
        logf(L"RegisterClassEx failed err=%lu", GetLastError());
        if (stopEvent) CloseHandle(stopEvent);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    state.panes.reserve(panes.size());
    for (const Pane& pane : panes) {
        startPane(inst, state.wallpaperHost, pane, state);
    }

    logf(L"Active panes: %d", (int)state.panes.size());
    if (state.panes.empty()) {
        if (stopEvent) CloseHandle(stopEvent);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    NOTIFYICONDATAW nid{};
    HWND tray = createTray(inst, &state, nid);

    bool quit = false;
    while (!quit) {
        HANDLE waits[1] = { stopEvent };
        DWORD wr = MsgWaitForMultipleObjects(stopEvent ? 1 : 0,
                                             stopEvent ? waits : nullptr,
                                             FALSE, INFINITE, QS_ALLINPUT);
        if (stopEvent && wr == WAIT_OBJECT_0) {
            quit = true;
            break;
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (tray) {
        KillTimer(tray, OCCLUSION_TIMER);
        KillTimer(tray, GRACE_TIMER);
        KillTimer(tray, HOST_TIMER);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (IsWindow(tray)) DestroyWindow(tray);
    }

    for (PaneRT& pane : state.panes) shutdownPane(pane);
    if (stopEvent) CloseHandle(stopEvent);

    MFShutdown();
    CoUninitialize();
    logLine(L"=== MotionCLI engine stopped ===");
    g_log.flush();
    return 0;
}

bool EngineController::restart(std::string& err) {
    stop();

    if (HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, kStopEventName)) {
        ResetEvent(ev);
        CloseHandle(ev);
    }

    std::wstring cmd = L"\"" + exePath() + L"\" --render";

    DWORD pid = 0;
    if (!launchAsShellUser(cmd, pid, err)) return false;

    m_config.enginePid = pid;
    m_config.save();
    return true;
}

void EngineController::stop() {
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName)) {
        SetEvent(ev);
        CloseHandle(ev);
    }

    unsigned long pid = m_config.enginePid;
    if (pid != 0) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (h) {
            if (WaitForSingleObject(h, 3000) != WAIT_OBJECT_0) {
                TerminateProcess(h, 0);
            }
            CloseHandle(h);
        }
    }

    m_config.enginePid = 0;
    m_config.save();
}

bool EngineController::isRunning() const {
    return processAlive(m_config.enginePid);
}

} // namespace motion
