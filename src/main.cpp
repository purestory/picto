// src/main.cpp : Main Entry Point and UI Window Procedure
// High-performance Multi-threaded Photo/Video Organizer
// Modularized into clean functional layers in src/

#include "framework.h"
#include "picto.h"
#include "common/utils.h"
#include "organizer/organizer_engine.h"
#include "dedup/dup_cleaner_dialog.h"
#include "resized/resized_cleaner_dialog.h"
#include "gps/gps_dialog.h"
#include "exif/exif_editor_dialog.h"

#include <shellapi.h>
#include <commctrl.h>
#include <shlwapi.h>

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Global application state definitions
HINSTANCE g_hInst = NULL;
HWND g_hMainWnd = NULL;
UINT g_currentDpi = 96;

extern std::atomic<bool> g_bIsProcessing;
extern std::thread g_scannerThread;

// -------------------------------------------------------------
// Main Window Procedure
// -------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        g_currentDpi = GetWindowDpi(hWnd);
        CreateMainControls(hWnd);
        break;

    case WM_DPICHANGED:
    {
        g_currentDpi = HIWORD(wParam);
        CreateDpiFonts(g_currentDpi);
        ApplyFontsToControls(hWnd);

        RECT* const prcNewWindow = (RECT*)lParam;
        SetWindowPos(hWnd, NULL,
            prcNewWindow->left, prcNewWindow->top,
            prcNewWindow->right - prcNewWindow->left,
            prcNewWindow->bottom - prcNewWindow->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        break;
    }

    case WM_SIZE:
        UpdateLayout(hWnd, LOWORD(lParam), HIWORD(lParam));
        break;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* pMMI = (MINMAXINFO*)lParam;
        UINT dpi = g_currentDpi;
        pMMI->ptMinTrackSize.x = ScaleDpi(720, dpi);
        pMMI->ptMinTrackSize.y = ScaleDpi(480, dpi);
        break;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_START) {
            ShowOrganizerDialog(hWnd);
            SetFocus(hWnd);
        }
        else if (id == IDC_BTN_DUP_CLEAN) {
            DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_DUP_CLEANER), hWnd, DupCleanerDlgProc, 0);
            SetFocus(hWnd);
        }
        else if (id == IDC_BTN_RESIZED_CLEAN) {
            DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_RESIZED_CLEANER), hWnd, ResizedCleanerDlgProc, 0);
            SetFocus(hWnd);
        }
        else if (id == IDC_BTN_GPS_MAP) {
            DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_GPSMAP), hWnd, GpsMapDlgProc, 0);
            SetFocus(hWnd);
        }
        else if (id == IDC_BTN_EXIF_EDIT) {
            ShowExifEditorDialog(hWnd, {});
            SetFocus(hWnd);
        }
        break;
    }

    case WM_CLOSE:
        if (g_bIsProcessing) {
            if (MessageBoxW(hWnd, L"파일 정리가 진행 중입니다. 정말 중단하고 종료하시겠습니까?",
                L"종료 확인", MB_ICONQUESTION | MB_YESNO) == IDYES) {
                CancelOrganizing();
                if (g_scannerThread.joinable()) {
                    g_scannerThread.join();
                }
                DestroyWindow(hWnd);
            }
        } else {
            DestroyWindow(hWnd);
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_PICTO));
    wcex.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcex.lpszClassName = L"PICTO_ORGANIZER";
    wcex.hIconSm = LoadIconW(wcex.hInstance, MAKEINTRESOURCEW(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    g_hInst = hInstance;

    WCHAR szTitle[MAX_LOADSTRING];
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);

    int initW = 860;
    int initH = 520;

    HWND hWnd = CreateWindowExW(
        0,
        L"PICTO_ORGANIZER",
        szTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        initW, initH,
        NULL, NULL, hInstance, NULL);

    if (!hWnd) return FALSE;

    g_hMainWnd = hWnd;
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    return TRUE;
}

// -------------------------------------------------------------
// WinMain Entry Point
// -------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR lpCmdLine,
                     _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    InitDpiAwareness();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"PictoOrganizerMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(L"PICTO_ORGANIZER", NULL);
        if (hExisting) {
            SetForegroundWindow(hExisting);
            if (IsIconic(hExisting)) ShowWindow(hExisting, SW_RESTORE);
        }
        CloseHandle(hMutex);
        return 0;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        CloseHandle(hMutex);
        return FALSE;
    }

    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow)) {
        CoUninitialize();
        CloseHandle(hMutex);
        return FALSE;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool bAutoRun = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--input") == 0 && i + 1 < argc) {
                g_currentOptions.inputFolder = argv[++i];
            } else if (_wcsicmp(argv[i], L"--output") == 0 && i + 1 < argc) {
                g_currentOptions.outputFolder = argv[++i];
            } else if (_wcsicmp(argv[i], L"--run") == 0) {
                bAutoRun = true;
            }
        }
        LocalFree(argv);
    }

    if (bAutoRun) {
        PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_START, BN_CLICKED), (LPARAM)g_hBtnStart);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CleanupGpsScanThread();

    CoUninitialize();
    CloseHandle(hMutex);

    return (int)msg.wParam;
}
