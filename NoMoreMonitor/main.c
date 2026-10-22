#include <stdio.h>
#include <Windows.h>
#include <string.h>
#include <stdlib.h>
#include "notice.h"
#include "messager.h"
#include "injector.h"
#include "resource.h"
#include "config.h"
#include "osd_render.h"
#include "toast_sound.h"
#include <tchar.h>
#include <shlobj.h>

#include <psapi.h>
#pragma comment(lib, "psapi.lib")

void set_console_icon() {
    HWND hConsole = GetConsoleWindow();
    if (hConsole != NULL) {
        HINSTANCE hInstance = GetModuleHandle(NULL);
        HICON hIconBig = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));

        HICON hIconSmall = (HICON)LoadImageW(
            hInstance,
            MAKEINTRESOURCEW(IDI_ICON1),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR
        );

        SendMessage(hConsole, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
        SendMessage(hConsole, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
    }
}

void SetupLogWindow() {
    HWND hwndConsole = GetConsoleWindow();
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hwndConsole == NULL || hOut == INVALID_HANDLE_VALUE) {
        con_printf(L"[!]can't get the console handle\n");
        return;
    }

    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

    int windowWidth = (workArea.right - workArea.left) / 5;
    int windowHeight = (workArea.bottom - workArea.top) / 5;

    /* 先把屏幕缓冲区按字体大小缩到目标行列，否则 SetWindowPos 缩不动窗口（只会出滚动条） */
    CONSOLE_FONT_INFO cfi = { 0 };
    if (GetCurrentConsoleFont(hOut, FALSE, &cfi)) {
        COORD fs = GetConsoleFontSize(hOut, cfi.nFont);
        if (fs.X > 0 && fs.Y > 0) {
            int cols = windowWidth / fs.X;
            int rows = windowHeight / fs.Y;
            if (cols < 10) cols = 10;
            if (rows < 5) rows = 5;
            COORD sz = { (SHORT)cols, (SHORT)rows };
            SetConsoleScreenBufferSize(hOut, sz);
        }
    }

    /* 放到工作区右上角 */
    int x = workArea.right - windowWidth;
    int y = workArea.top;
    SetWindowPos(hwndConsole,
        HWND_NOTOPMOST,
        x,
        y,
        windowWidth,
        windowHeight,
        SWP_SHOWWINDOW);
}

BOOL WINAPI console_ctrl_handler(DWORD signal) {
    switch (signal) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        /* 日志控制台里的 Ctrl+C：忽略，不退出后台监控 */
        return TRUE;
    case CTRL_CLOSE_EVENT:
        /* 用户点了控制台关闭按钮：清理后真正退出进程，否则残留后台僵尸进程
           会一直占着程序所在目录（CWD），导致整个文件夹删不掉 */
        notice_cleanup_tray();
        toast_sound_stop();
        messager_uninit();
        osd_shutdown();
        ExitProcess(0);
        return TRUE;
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        notice_cleanup_tray();
        toast_sound_stop();
        messager_uninit();
        osd_shutdown();
        return FALSE;   /* 让系统继续注销/关机 */
    }
    return FALSE;
}


DWORD FindProcessByEnum(const TCHAR* processName)
{
    DWORD pids[1024], cbNeeded;
    if (!EnumProcesses(pids, sizeof(pids), &cbNeeded)) return 0;

    DWORD count = cbNeeded / sizeof(DWORD);
    for (DWORD i = 0; i < count; i++) {
        if (pids[i] == 0) continue;
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pids[i]);
        if (hProc) {
            TCHAR name[MAX_PATH] = { 0 };
            if (GetModuleBaseName(hProc, NULL, name, MAX_PATH)) {
                if (_tcsicmp(name, processName) == 0) {
                    CloseHandle(hProc);
                    return pids[i];
                }
            }
            CloseHandle(hProc);
        }
    }
    return 0;
}

/* 可靠的“是否已提权”判断（TokenElevation，比 IsUserAnAdmin 更准确） */
static bool process_is_elevated(void) {
    bool elevated = false;
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elev = { 0 };
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size)) {
            elevated = (elev.TokenIsElevated != 0);
        }
        CloseHandle(token);
    }
    return elevated;
}

int main(int argc, char** argv) {
    /* 感知系统 DPI：否则高分辨率屏上会被 Windows 位图拉伸，文字呈像素风/模糊 */
    SetProcessDPIAware();

    /* 不要占住程序所在目录（默认 CWD），否则运行期间整个文件夹无法删除/重命名。
       配置与 DLL 路径均用 GetModuleFileNameW 的绝对路径，不依赖 CWD。 */
    SetCurrentDirectoryW(L"C:\\");

    config_load(&g_config, NULL);
    {
        /* 首次运行生成一份默认配置文件，方便用户手动编辑 */
        wchar_t cfg_path[MAX_PATH] = { 0 };
        config_get_path(cfg_path, MAX_PATH, NULL);
        if (GetFileAttributesW(cfg_path) == INVALID_FILE_ATTRIBUTES) {
            config_save(&g_config, NULL);
        }
    }
    /* exe 编译为 GUI 子系统，默认不再自动弹控制台；仅当配置启用时才分配，
       从而避免启动瞬间闪过黑框。 */
    if (g_config.show_console) {
        AllocConsole();
    }
    con_init();
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    /* 控制台：标题 + 显示/隐藏 + 颜色（集中到 config_apply_console） */
    config_apply_console();
    if (g_config.show_console) {
        SetupLogWindow();
    }

    /* 诊断：确认当前进程是否真的以管理员（提权）运行 */
    con_printf(L"[*]Process elevated (admin): %s\n", process_is_elevated() ? L"YES" : L"NO");

    /* --autostart：静默启动，不弹窗询问，直接进入仅通知模式 */
    bool autostart = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--autostart") == 0) {
            autostart = true;
            break;
        }
    }

    if (autostart) {
        g_use_osd = false;
        g_silent = true;   /* 静默启动：通知不播放提示音 */
        con_printf(L"[-]%s\n", g_config.console_mode_off);
    }
    else {
        int choice = MessageBoxW(NULL, L"是否以巨幅文本+通知的形式来提示（若选否则只以通知形式）", L"Lilith is all you need", MB_YESNO | MB_ICONQUESTION);
        if (choice == IDYES) {
            g_use_osd = true;
            con_printf(L"[-]%s\n", g_config.console_mode_on);
        }
        else {
            g_use_osd = false;
            con_printf(L"[-]%s\n", g_config.console_mode_off);
        }
    }
    if (!messager_init()) { con_printf(L"[!]messager init failed\n"); goto MAIN_FAILED; }
    if (!notice_window_init()) { con_printf(L"[!]notice init failed\n"); messager_uninit(); goto MAIN_FAILED; }
    /* 模式选定后立刻弹一次桌面 Toast，两种模式都要能看见；静默启动不弹 */
    if (!autostart) {
        send_balloon_notification(
            g_config.tray_tip,
            g_use_osd ? L"巨幅文本 + 通知模式已就绪" : L"仅通知模式已就绪",
            false);
    }
    while (1) {
        con_printf(L"[-]%s\n", g_config.console_waiting);
        DWORD pid = 0;
        while ((pid = FindProcessByEnum(L"media_capture.exe")) == 0) {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    con_printf(L"[!]Window closed before target process started.\n");
                    goto EXIT_CLEANUP;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            Sleep(50);
        }

        con_printf(L"[*]%s\n", g_config.console_target_appeared);
        HANDLE target_process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!target_process) {
            /* 目标可能刚退出（竞态），重试而不是退出整个监控 */
            con_printf(L"[!]OpenProcess failed (%lu), retrying...\n", GetLastError());
            continue;
        }
        WaitForInputIdle(target_process, 5000);
        wchar_t full_path[MAX_PATH] = { 0 };
        if (GetModuleFileNameW(NULL, full_path, MAX_PATH) == 0) {
            con_printf(L"[!]GetModuleFileNameW failed: %lu\n", GetLastError());
            CloseHandle(target_process);
            goto MAIN_FAILED;
        }

        wchar_t* last_slash = wcsrchr(full_path, L'\\');
        if (last_slash) {
            size_t remaining_space = MAX_PATH - (last_slash - full_path) - 1;
            wcscpy_s(last_slash + 1, remaining_space, L"NoMoreMonitor_Dll.dll");
        }
        else {
            con_printf(L"[!]Invalid execution path\n");
            CloseHandle(target_process);
            goto MAIN_FAILED;
        }

        int eject_retries = 10;
        while (ejector(target_process, L"NoMoreMonitor_Dll.dll") && eject_retries-- > 0) {
            con_printf(L"[*]Found existing DLL in target process, ejecting...\n");
            Sleep(100);
        }

        if (!injector(target_process, full_path)) {
            con_printf(L"[!] inject failed\n");
            CloseHandle(target_process);
            continue;
        }

        con_printf(L"[-]%s\n", g_config.console_running);
        bool process_alive = true;
        while (process_alive) {
            DWORD wait_res = MsgWaitForMultipleObjects(1, &target_process, FALSE, INFINITE, QS_ALLINPUT);

            if (wait_res == WAIT_OBJECT_0) {
                con_printf(L"[*]%s\n", g_config.console_target_closed);
                PostMessage(hwnd, WM_IPC_STATE, status_waiting, 0);
                process_alive = false;
            }
            else if (wait_res == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        CloseHandle(target_process);
                        goto EXIT_CLEANUP;
                    }
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
        }

        CloseHandle(target_process);
    }

EXIT_CLEANUP:
    con_printf(L"[*]%s\n", g_config.console_mission_completed);
    toast_sound_stop();
    messager_uninit();
    osd_shutdown();
    return 0;

MAIN_FAILED:
    toast_sound_stop();
    messager_uninit();
    osd_shutdown();
    return -1;
}