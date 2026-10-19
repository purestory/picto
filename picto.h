#pragma once

#include "framework.h"
#include "Resource.h"

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

struct OrganizerOptions {
    std::wstring inputFolder;
    std::wstring outputFolder;
    int threadCount = 4;
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

struct GpsPoint {
    std::wstring filePath;
    double latitude = 0.0;
    double longitude = 0.0;
};

// DPI and scaling helpers
void InitDpiAwareness();
UINT GetWindowDpi(HWND hWnd);
int ScaleDpi(int value, UINT dpi);

// Metadata and processing functions
bool ExtractDate(const FileItem& item, SYSTEMTIME& outTime);
bool ExtractExifDate(const std::wstring& filePath, SYSTEMTIME& outTime);
bool ExtractFilenameDate(const std::wstring& filename, SYSTEMTIME& outTime);
bool ExtractFileMetadataDate(const FileItem& item, SYSTEMTIME& outTime);
bool ExtractGps(const std::wstring& filePath, double& outLat, double& outLon);

// High-speed memory-optimized deduplication helpers
uint64_t CalculateFastSampleHash(const std::wstring& filePath, uint64_t fileSize);
bool AreFilesContentIdentical(const std::wstring& path1, const std::wstring& path2, uint64_t fileSize);
bool CalculateFileMD5(const std::wstring& filePath, std::string& outHash);

// Map generator
bool GenerateAndOpenLeafletMap(const std::vector<GpsPoint>& points);

// File helpers
bool MoveOrCopyFileToDateFolder(const std::wstring& filePath, const std::wstring& filename,
                               const SYSTEMTIME& dateTaken, const OrganizerOptions& options,
                               std::wstring& outDestination);

// Dialog and Window functions
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK GpsMapDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK ResizedCleanerDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK DupCleanerDlgProc(HWND, UINT, WPARAM, LPARAM);
