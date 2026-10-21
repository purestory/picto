#pragma once

#include "framework.h"
#include "Resource.h"
#include <string>
#include <vector>
#include <atomic>

#define MAX_LOADSTRING 100
#define TIMER_STATS_UPDATE 1

// Custom window messages
#define WM_APP_LOG_MESSAGE         (WM_APP + 1)
#define WM_APP_SCAN_FINISHED       (WM_APP + 2)
#define WM_APP_WORK_FINISHED       (WM_APP + 3)
#define WM_APP_RESIZED_PROGRESS    (WM_APP + 10)
#define WM_APP_RESIZED_FINISHED    (WM_APP + 11)
#define WM_APP_DUP_PROGRESS        (WM_APP + 20)
#define WM_APP_DUP_FINISHED        (WM_APP + 21)
#define WM_APP_TRIGGER_SEARCH      (WM_APP + 30)
#define WM_APP_GPS_PROGRESS        (WM_APP + 40)
#define WM_APP_GPS_FINISHED        (WM_APP + 41)
#define WM_APP_GPS_ITEM_FOUND      (WM_APP + 42)

enum FolderFormatType {
    FOLDER_FORMAT_YEAR_ONLY = 0,        // 2023 폴더 (기본값)
    FOLDER_FORMAT_YEAR_MONTH = 1,       // 2023\05 폴더
    FOLDER_FORMAT_YEAR_YEARMONTH = 2,   // 2023\2023-05 폴더
    FOLDER_FORMAT_YEARMONTH_DASH = 3,   // 2023-05 폴더
    FOLDER_FORMAT_DATE_DASH = 4,        // 2023-05-14 폴더
    FOLDER_FORMAT_YEAR_MONTH_DAY = 5,   // 2023\05\14 폴더
    FOLDER_FORMAT_NONE = 6              // 출력 폴더 루트 (분류 없음)
};

struct OrganizerOptions {
    std::wstring inputFolder;
    std::wstring outputFolder;
    int threadCount = 0; // 0: Auto-detect CPU hardware concurrency
    FolderFormatType folderFormat = FOLDER_FORMAT_YEAR_ONLY;
    bool organizeByMonth = false;
    bool copyFiles = false;
    bool removeDuplicates = true;
    bool removeResized = true;
};

struct OrganizerStats {
    std::atomic<int> totalFiles{0};
    std::atomic<int> processedFiles{0};
    std::atomic<int> movedFiles{0};
    std::atomic<int> duplicatesRemoved{0};
    std::atomic<int> resizedRemoved{0};
    std::atomic<int> errors{0};
    double processingTimeSeconds{0.0};
};

struct FileItem {
    std::wstring filePath;
    std::wstring filename;
    uint64_t fileSize = 0;
    FILETIME creationTime = { 0, 0 };
    FILETIME lastWriteTime = { 0, 0 };
    bool isDuplicateCandidate = false;
};

// Global application state
extern HINSTANCE g_hInst;
extern HWND g_hMainWnd;
extern UINT g_currentDpi;

// Control Handles
extern HWND g_hEditInput;
extern HWND g_hEditOutput;
extern HWND g_hBtnStart;
extern HWND g_hBtnCancel;
extern HWND g_hBtnDupClean;
extern HWND g_hBtnResizedClean;
extern HWND g_hBtnGpsMap;
extern HWND g_hBtnExifEdit;
extern HWND g_hProgressBar;
extern HWND g_hLblStatus;
extern HWND g_hEditLog;

// Main Window Functions
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow);
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
