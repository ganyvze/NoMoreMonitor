#include "notice.h"
#include "messager.h"
#include "config.h"
#include "osd_render.h"
#include "config_panel.h"
#include "position_panel.h"
#include "analysis.h"
#include "toast_sound.h"
#include "img_loader.h"
#include <shlobj.h>
#include "resource.h"
#pragma comment(lib, "msimg32.lib")

const wchar_t* notice_window_name = L"Lilith is all you need!";
const wchar_t* notice_class_name = L"YDS & Lilith";
HINSTANCE instacne;
HWND hwnd;
int current_ui_state = status_waiting;

bool g_use_osd = true;
bool g_silent = false;   /* 静默启动(--autostart)：通知不播放提示音 */
wchar_t g_log_text[1024] = { 0 };

#define FADE_TIMER_ID 1
#define FADE_STEP     0.02f   /* 每次计时器滴答减少的透明度(约 1.5s 淡出) */

static float g_text_alpha = 1.0f;
static bool  g_fading = false;

/* ---- 桌面 Toast 弹窗（不依赖系统通知中心，保证可见） ---- */
#define TOAST_CLASS_NAME L"NoMoreMonitor.Toast"
#define TOAST_TIMER_ID   3
#define TOAST_SHOW_MS    5000
#define TOAST_W          360
#define TOAST_H          110
#define TOAST_IMG_SIZE   80

static HWND g_toast_hwnd = NULL;
static wchar_t g_toast_title[128] = { 0 };
static wchar_t g_toast_body[256] = { 0 };

/* 系统 DPI 缩放系数：高分辨率屏下不再被 Windows 位图拉伸(否则字体会变像素风)，
   感知 DPI 后按比例放大字号与窗口尺寸，使文字清晰且大小不变。 */
static float dpi_scale(void) {
    static float s = -1.0f;
    if (s < 0.0f) {
        HDC hdc = GetDC(NULL);
        int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
        if (hdc) ReleaseDC(NULL, hdc);
        if (dpi <= 0) dpi = 96;
        s = (float)dpi / 96.0f;
    }
    return s;
}

/* Toast 小图缓存（预乘 BGRA） */
static wchar_t g_toast_img_path[MAX_PATH] = { 0 };
static unsigned char* g_toast_img_bgra = NULL;
static int g_toast_img_w = 0;
static int g_toast_img_h = 0;

static void toast_img_clear(void) {
    if (g_toast_img_bgra) { free(g_toast_img_bgra); g_toast_img_bgra = NULL; }
    g_toast_img_path[0] = L'\0';
    g_toast_img_w = 0;
    g_toast_img_h = 0;
}

static void toast_img_draw(HDC hdc, const wchar_t* path, int x, int y, int dw, int dh) {
    if (!path || path[0] == L'\0') return;

    if (wcscmp(g_toast_img_path, path) != 0 || !g_toast_img_bgra) {
        toast_img_clear();
        int iw = 0, ih = 0;
        unsigned char* px = img_load_file(path, &iw, &ih);
        if (!px || iw <= 0 || ih <= 0) { img_free(px); return; }

        unsigned char* scaled = img_scale_rgba(px, &iw, &ih, dw, dh);
        if (scaled == px) { dw = iw; dh = ih; }

        unsigned char* bgra = (unsigned char*)malloc((size_t)dw * dh * 4);
        if (!bgra) { img_free(scaled); return; }
        for (int i = 0; i < dw * dh; i++) {
            int a = scaled[i * 4 + 3];
            bgra[i * 4 + 0] = (BYTE)(scaled[i * 4 + 2] * a / 255);
            bgra[i * 4 + 1] = (BYTE)(scaled[i * 4 + 1] * a / 255);
            bgra[i * 4 + 2] = (BYTE)(scaled[i * 4 + 0] * a / 255);
            bgra[i * 4 + 3] = (BYTE)a;
        }
        img_free(scaled);

        wcsncpy_s(g_toast_img_path, _countof(g_toast_img_path), path, _TRUNCATE);
        g_toast_img_bgra = bgra;
        g_toast_img_w = dw;
        g_toast_img_h = dh;
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_toast_img_w;
    bmi.bmiHeader.biHeight = -g_toast_img_h; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC mem = CreateCompatibleDC(hdc);
    void* bits = NULL;
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        return;
    }
    memcpy(bits, g_toast_img_bgra, (size_t)g_toast_img_w * g_toast_img_h * 4);
    HGDIOBJ old = SelectObject(mem, dib);

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(hdc, x, y, dw, dh, mem, 0, 0, g_toast_img_w, g_toast_img_h, bf);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
}

static void toast_hide(void) {
    if (!g_toast_hwnd) return;
    KillTimer(g_toast_hwnd, TOAST_TIMER_ID);
    ShowWindow(g_toast_hwnd, SW_HIDE);
}

/* 30 分钟静音：点 Toast 右上角 X 后，期间不再显示任何通知/文本 */
static ULONGLONG g_mute_until = 0;

static bool is_muted(void) {
    if (g_mute_until == 0) return false;
    if (GetTickCount64() < g_mute_until) return true;
    g_mute_until = 0;   /* 已过期，自动解除 */
    return false;
}

static void mute_30min(void) {
    g_mute_until = GetTickCount64() + (ULONGLONG)30 * 60 * 1000;
    toast_hide();
}

/* 右上角关闭按钮的命中区域（按 DPI 缩放） */
static RECT toast_close_rect(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    float s = dpi_scale();
    int size = (int)(20 * s + 0.5f);
    int margin = (int)(8 * s + 0.5f);
    RECT r;
    r.right = rc.right - margin;
    r.top = margin;
    r.left = r.right - size;
    r.bottom = r.top + size;
    return r;
}

static void toast_place(void) {
    if (!g_toast_hwnd) return;
    int tw = (int)(TOAST_W * dpi_scale() + 0.5f);
    int th = (int)(TOAST_H * dpi_scale() + 0.5f);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.right - tw - 16;
    int y = wa.bottom - th - 16;
    SetWindowPos(g_toast_hwnd, HWND_TOPMOST, x, y, tw, th,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK toast_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_LBUTTONDOWN: {
        POINT pt = { (SHORT)LOWORD(lp), (SHORT)HIWORD(lp) };
        RECT cr = toast_close_rect(h);
        if (PtInRect(&cr, pt)) {
            mute_30min();   /* 点右上角 X：30 分钟内不再显示任何通知/文本 */
        } else {
            toast_hide();
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        toast_hide();
        return 0;
    case WM_TIMER:
        if (wp == TOAST_TIMER_ID) {
            toast_hide();
            return 0;
        }
        break;
    case WM_ERASEBKGND: {
        /* 背景必须在这里画实，否则 class 无画刷时会出现“透明空窗” */
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(g_config.toast_bg_color);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);

        float s = dpi_scale();
        int accent_w = (int)(6 * s + 0.5f);
        int pad = (int)(12 * s + 0.5f);
        int title_size = (int)(22 * s + 0.5f);
        int body_size = (int)(18 * s + 0.5f);
        int img_size = (int)(TOAST_IMG_SIZE * s + 0.5f);

        COLORREF bg = g_config.toast_bg_color;
        COLORREF accent = g_config.toast_accent_color;

        HBRUSH bgbrush = CreateSolidBrush(bg);
        FillRect(hdc, &rc, bgbrush);
        DeleteObject(bgbrush);

        RECT accent_rc = { 0, 0, accent_w, rc.bottom };
        HBRUSH ab = CreateSolidBrush(accent);
        FillRect(hdc, &accent_rc, ab);
        DeleteObject(ab);

        int text_left = (int)(18 * s + 0.5f);
        if (g_config.toast_image[0]) {
            int iy = (rc.bottom - img_size) / 2;
            toast_img_draw(hdc, g_config.toast_image, (int)(14 * s + 0.5f), iy, img_size, img_size);
            text_left = (int)((14 + TOAST_IMG_SIZE + 12) * s + 0.5f);
        }

        SetBkMode(hdc, TRANSPARENT);

        HFONT title_font = CreateFontW(title_size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
        HFONT body_font = CreateFontW(body_size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");

        HGDIOBJ old = SelectObject(hdc, title_font);
        SetTextColor(hdc, RGB(255, 255, 255));
        RECT tr = { text_left, pad, rc.right - (int)(36 * s + 0.5f), (int)(40 * s + 0.5f) };
        DrawTextW(hdc, g_toast_title, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(hdc, body_font);
        SetTextColor(hdc, RGB(210, 210, 210));
        RECT br = { text_left, (int)(44 * s + 0.5f), rc.right - pad, rc.bottom - (int)(10 * s + 0.5f) };
        DrawTextW(hdc, g_toast_body, -1, &br, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

        SelectObject(hdc, old);
        DeleteObject(title_font);
        DeleteObject(body_font);

        /* 右上角白色关闭按钮 */
        {
            RECT cr = toast_close_rect(h);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ old_pen = SelectObject(hdc, pen);
            MoveToEx(hdc, cr.left, cr.top, NULL);
            LineTo(hdc, cr.right, cr.bottom);
            MoveToEx(hdc, cr.right, cr.top, NULL);
            LineTo(hdc, cr.left, cr.bottom);
            SelectObject(hdc, old_pen);
            DeleteObject(pen);
        }

        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        toast_hide();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static bool toast_ensure_window(void) {
    if (g_toast_hwnd) return true;

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = toast_wnd_proc;
    wc.hInstance = instacne;
    wc.lpszClassName = TOAST_CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    int tw = (int)(TOAST_W * dpi_scale() + 0.5f);
    int th = (int)(TOAST_H * dpi_scale() + 0.5f);
    g_toast_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        TOAST_CLASS_NAME, L"",
        WS_POPUP,
        0, 0, tw, th,
        NULL, NULL, instacne, NULL);
    return g_toast_hwnd != NULL;
}

static void toast_show(const wchar_t* title, const wchar_t* body, bool sticky) {
    if (!toast_ensure_window()) return;

    wcsncpy_s(g_toast_title, _countof(g_toast_title), title ? title : L"", _TRUNCATE);
    wcsncpy_s(g_toast_body, _countof(g_toast_body), body ? body : L"", _TRUNCATE);

    toast_place();
    /* 强制置顶并重绘，避免被其它 TOPMOST 窗口（如全屏 OSD）盖住 */
    SetWindowPos(g_toast_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_toast_hwnd, NULL, TRUE);
    UpdateWindow(g_toast_hwnd);

    if (sticky) {
        /* 常驻：监控进行中不自动关闭，并取消之前遗留的自动关闭计时器 */
        KillTimer(g_toast_hwnd, TOAST_TIMER_ID);
    }
    else {
        SetTimer(g_toast_hwnd, TOAST_TIMER_ID, TOAST_SHOW_MS, NULL);
    }
}

/* 托盘右键菜单项(固定文字，不进配置) */
#define IDM_SETTINGS 0x1001  /* 设置... 完整面板 */
#define IDM_POSITION 0x1002  /* 配置    文字位置 */
#define IDM_ANALYSIS 0x1003  /* 分析    GDI+ 图表 */
#define IDM_EXIT     0x1004  /* 退出 */

/* NOTIFYICON_VERSION_4 通知事件（部分旧 SDK 未定义时兜底） */
#ifndef NIN_SELECT
#define NIN_SELECT     (WM_USER + 0)
#define NINF_KEY       0x1
#define NIN_KEYSELECT  (NIN_SELECT | NINF_KEY)
#endif
#ifndef NIN_POPUPMENU
#define NIN_POPUPMENU  (WM_USER + 6)
#endif

static void notice_render(void) {
    if (!hwnd) return;
    /* 仅通知模式或静音 30 分钟内：OSD 不画任何文字/图片 */
    if (!g_use_osd || is_muted()) {
        osd_render(hwnd, status_waiting, &g_config, NULL, 0.0f);
        return;
    }
    osd_render(hwnd, current_ui_state, &g_config, g_log_text, g_text_alpha);
}

void notice_refresh_osd(void) {
    notice_render();
}

void send_balloon_notification(const wchar_t* title, const wchar_t* text, bool sticky) {
    /* 静音 30 分钟内：不显示任何通知与文本 */
    if (is_muted()) return;

    /* 桌面 Toast：不依赖系统通知中心/专注助手，一定能看见 */
    toast_show(title, text, sticky);

    /* 提示音：自定义音频或系统默认；静默启动(--autostart)时不播 */
    if (!g_silent) toast_sound_play(g_config.toast_sound);

    /* 同时尝试托盘气泡（Win10/11 可能进通知中心，作补充） */
    if (!hwnd) return;
    NOTIFYICONDATA nid = { 0 };
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    if (title) wcscpy_s(nid.szInfoTitle, _countof(nid.szInfoTitle), title);
    if (text)  wcscpy_s(nid.szInfo, _countof(nid.szInfo), text);
    Shell_NotifyIcon(NIM_MODIFY, &nid);
}

static void show_tray_menu(void) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置...");
    AppendMenuW(menu, MF_STRING, IDM_POSITION, L"配置");
    AppendMenuW(menu, MF_STRING, IDM_ANALYSIS, L"分析");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);   /* 让弹出菜单正确关闭 */
    DestroyMenu(menu);

    switch (cmd) {
    case IDM_SETTINGS:
        config_panel_show(instacne, &g_config);
        notice_render();

        /* 刷新托盘提示文字 */
        {
            NOTIFYICONDATA nid = { 0 };
            nid.cbSize = sizeof(NOTIFYICONDATA);
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_TIP;
            wcscpy_s(nid.szTip, _countof(nid.szTip), g_config.tray_tip);
            Shell_NotifyIcon(NIM_MODIFY, &nid);
        }
        break;

    case IDM_POSITION:
        position_panel_show(instacne, &g_config);
        notice_render();
        break;

    case IDM_ANALYSIS:
        analysis_show(instacne);
        break;

    case IDM_EXIT:
        /* 销毁主窗口，让 WM_DESTROY 完成托盘/Toast 清理并 PostQuitMessage。
           否则全屏分层 OSD 窗口会残留，主线程退出阻塞时被 DWM 判定无响应，
           表现为整屏白屏 + “NoMoreMonitor.exe 无响应”。 */
        DestroyWindow(hwnd);
        break;
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_IPC_STATE: {
        int incoming_state = (int)wParam;

        if (incoming_state == status_log) {
            if (lParam != 0) {
                wcscpy_s(g_log_text, _countof(g_log_text), (const wchar_t*)lParam);
            }
        }
        else {
            current_ui_state = incoming_state;
            g_log_text[0] = L'\0';

            if (current_ui_state == status_start) {
                g_fading = false;
                g_text_alpha = 1.0f;
                KillTimer(hwnd, FADE_TIMER_ID);
                send_balloon_notification(g_config.start_title, g_config.start_body, false);
            }
            else if (current_ui_state == status_watching) {
                g_fading = false;
                g_text_alpha = 1.0f;
                KillTimer(hwnd, FADE_TIMER_ID);
                send_balloon_notification(g_config.watching_title, g_config.watching_body, true);
            }
            else if (current_ui_state == status_stop) {
                send_balloon_notification(g_config.stop_title, g_config.stop_body, false);
                g_fading = true;
                g_text_alpha = 1.0f;
                SetTimer(hwnd, FADE_TIMER_ID, 30, NULL);
            }
        }

        notice_render();
        return 0;
    }

    case WM_TIMER: {
        if (wParam == FADE_TIMER_ID && g_fading) {
            g_text_alpha -= FADE_STEP;
            if (g_text_alpha <= 0.0f) {
                g_text_alpha = 0.0f;
                g_fading = false;
                KillTimer(hwnd, FADE_TIMER_ID);
            }
            notice_render();
        }
        return 0;
    }

    case WM_APP + 2: {
        /* NOTIFYICON_VERSION_4：事件类型在 lParam 的 LOWORD 里。
           NIN_SELECT(WM_USER+0) 是鼠标悬停/单键选择，NIN_POPUPOPEN(WM_USER+6)
           是气泡打开，二者都不是“请求弹出菜单”，之前一并触发导致悬停即弹菜单
           且无法单击空白关闭。这里只在右键菜单(Wm_CONTEXTMENU)与键盘选中
           (NIN_KEYSELECT)时弹出。 */
        UINT evt = LOWORD(lParam);
        if (evt == WM_CONTEXTMENU || evt == NIN_KEYSELECT) {
            show_tray_menu();
        }
        return 0;
    }

    case WM_DESTROY: {
        KillTimer(hwnd, FADE_TIMER_ID);
        toast_sound_stop();
        toast_hide();
        toast_img_clear();
        if (g_toast_hwnd) {
            DestroyWindow(g_toast_hwnd);
            g_toast_hwnd = NULL;
        }
        NOTIFYICONDATA nid = { 0 };
        nid.cbSize = sizeof(NOTIFYICONDATA);
        nid.hWnd = hwnd;
        nid.uID = 1;
        Shell_NotifyIcon(NIM_DELETE, &nid);
        notice_cleanup_tray();
        hwnd = NULL;   /* 窗口已销毁，置空避免后续误用悬空句柄 */
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void notice_cleanup_tray(void) {
    if (!hwnd) return;
    NOTIFYICONDATA nid = { 0 };
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIcon(NIM_DELETE, &nid);
}

bool notice_window_init() {
    SetCurrentProcessExplicitAppUserModelID(L"Lilith.Awesome.Notice.App");

    if (!osd_init()) {
        con_printf(L"[!]can't init GDI+ (OSD renderer)\n");
        return false;
    }

    instacne = GetModuleHandle(NULL);
    WNDCLASSW window_class = { 0 };
    window_class.hInstance = instacne;
    window_class.lpfnWndProc = wnd_proc;
    window_class.lpszClassName = notice_class_name;
    window_class.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&window_class);

    hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        notice_class_name, notice_window_name,
        WS_POPUP | WS_VISIBLE,
        0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) - 1,
        NULL, NULL, instacne, NULL);

    if (hwnd == NULL) {
        con_printf(L"[!]can't create a window which are used to notice user\n");
        return false;
    }

    NOTIFYICONDATA nid = { 0 };
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP + 2;
    HICON hTrayIcon = (HICON)LoadImageW(
        instacne,
        MAKEINTRESOURCEW(IDI_ICON1),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR
    );

    if (hTrayIcon) {
        nid.hIcon = hTrayIcon;
    }
    else {
        nid.hIcon = LoadIcon(instacne, MAKEINTRESOURCE(IDI_ICON1));
    }

    wcscpy_s(nid.szTip, _countof(nid.szTip), g_config.tray_tip);

    if (!Shell_NotifyIcon(NIM_ADD, &nid)) {
        con_printf(L"[!]Shell_NotifyIcon NIM_ADD failed: %lu\n", GetLastError());
        return false;
    }

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_SETVERSION, &nid);

    notice_render();
    return true;
}
