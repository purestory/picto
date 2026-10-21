#include "organizer_engine.h"
#include "date_extractor.h"
#include "dedup/dedup_engine.h"
#include "common/utils.h"
#include "Resource.h"
#include "picto.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <algorithm>

// Application Globals
extern HINSTANCE g_hInst;
extern HWND g_hMainWnd;
extern UINT g_currentDpi;

// Dialog & Control Handles
HWND g_hOrganizerDlg = NULL;

HWND g_hGrpFolder = NULL;
HWND g_hLblInput = NULL;
HWND g_hEditInput = NULL;
HWND g_hBtnBrowseInput = NULL;

HWND g_hLblOutput = NULL;
HWND g_hEditOutput = NULL;
HWND g_hBtnBrowseOutput = NULL;

HWND g_hGrpPerf = NULL;
HWND g_hLblThreads = NULL;
HWND g_hEditThreads = NULL;
HWND g_hSpinThreads = NULL;
HWND g_hLblCpuInfo = NULL;

HWND g_hGrpOpts = NULL;
HWND g_hLblFolderFormat = NULL;
HWND g_hComboFolderFormat = NULL;
HWND g_hLblFolderPreview = NULL;
HWND g_hChkOrgMonth = NULL;
HWND g_hChkCopyFiles = NULL;
HWND g_hChkRemoveDups = NULL;

HWND g_hBtnStart = NULL;
HWND g_hBtnCancel = NULL;
HWND g_hBtnDupClean = NULL;
HWND g_hBtnResizedClean = NULL;
HWND g_hBtnGpsMap = NULL;
HWND g_hBtnExifEdit = NULL;

HWND g_hGrpProgress = NULL;
HWND g_hLblStatus = NULL;
HWND g_hProgressBar = NULL;
HWND g_hLblProcessed = NULL;
HWND g_hLblMoved = NULL;
HWND g_hLblDups = NULL;
HWND g_hLblSpeed = NULL;

HWND g_hGrpLog = NULL;
HWND g_hEditLog = NULL;

// Main Dashboard Control Handles
HWND g_hMainTitle = NULL;
HWND g_hMainSubtitle = NULL;
HWND g_hDescOrganizer = NULL;
HWND g_hDescDup = NULL;
HWND g_hDescResized = NULL;
HWND g_hDescGps = NULL;
HWND g_hDescExif = NULL;
HWND g_hMainFooter = NULL;

// Worker & Synchronization globals
std::atomic<bool> g_bCancelRequested{ false };
std::atomic<bool> g_bIsProcessing{ false };
std::vector<std::thread> g_workerThreads;
std::thread g_scannerThread;

OrganizerOptions g_currentOptions;
OrganizerStats g_stats;
std::chrono::steady_clock::time_point g_startTime;

std::vector<FileItem> g_fileList;
std::atomic<size_t> g_currentFileIndex{ 0 };

// Memory-First Deduplication Structures
struct CandidateRecord {
    uint64_t sampleHash;
    std::wstring filePath;
};
static std::mutex g_candidateMutex;
static std::unordered_map<uint64_t, std::vector<CandidateRecord>> g_candidatesBySize;

// Fast RAM-cached directory set to eliminate redundant SHCreateDirectoryExW disk calls
static std::mutex g_folderMutex;
static std::unordered_set<std::wstring> g_createdFolders;

// Pure RAM Destination Path Reservation (Zero disk I/O inside lock!)
static std::mutex g_destReservationMutex;
static std::unordered_set<std::wstring> g_reservedDestPaths;

// Auto-Retry state tracking
static std::mutex g_failedMutex;
static std::vector<FileItem> g_failedFileList;
static std::vector<FileItem> g_nextFailedFileList;
static std::atomic<bool> g_bIsRetryPass{ false };

// Helper to translate Win32 error to readable text
static std::wstring FormatWin32Error(DWORD err)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND: return L"파일 없음";
    case ERROR_ACCESS_DENIED: return L"액세스 거부 (읽기 전용 또는 권한)";
    case ERROR_SHARING_VIOLATION: return L"공유 위반 (타 프로세스 사용 중)";
    case ERROR_LOCK_VIOLATION: return L"파일 잠금 충돌";
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return L"대상 파일 중복 존재";
    case ERROR_DISK_FULL: return L"디스크 공간 부족";
    default: {
        wchar_t buf[32];
        swprintf_s(buf, L"오류 0x%08X", err);
        return buf;
    }
    }
}

// -------------------------------------------------------------
// File Movement & Destination Path Resolution
// -------------------------------------------------------------
bool MoveOrCopyFileToDateFolder(const std::wstring& filePath, const std::wstring& filename,
                               const SYSTEMTIME& dateTaken, const OrganizerOptions& options,
                               std::wstring& outDestination)
{
    wchar_t subFolder[MAX_PATH] = { 0 };
    switch (options.folderFormat) {
    case FOLDER_FORMAT_YEAR_MONTH: // 2023\05
        swprintf_s(subFolder, L"%04d\\%02d", dateTaken.wYear, dateTaken.wMonth);
        break;
    case FOLDER_FORMAT_YEAR_YEARMONTH: // 2023\2023-05
        swprintf_s(subFolder, L"%04d\\%04d-%02d", dateTaken.wYear, dateTaken.wYear, dateTaken.wMonth);
        break;
    case FOLDER_FORMAT_YEAR_ONLY: // 2023
        swprintf_s(subFolder, L"%04d", dateTaken.wYear);
        break;
    case FOLDER_FORMAT_YEARMONTH_DASH: // 2023-05
        swprintf_s(subFolder, L"%04d-%02d", dateTaken.wYear, dateTaken.wMonth);
        break;
    case FOLDER_FORMAT_DATE_DASH: // 2023-05-14
        swprintf_s(subFolder, L"%04d-%02d-%02d", dateTaken.wYear, dateTaken.wMonth, dateTaken.wDay);
        break;
    case FOLDER_FORMAT_YEAR_MONTH_DAY: // 2023\05\14
        swprintf_s(subFolder, L"%04d\\%02d\\%02d", dateTaken.wYear, dateTaken.wMonth, dateTaken.wDay);
        break;
    case FOLDER_FORMAT_NONE: // 루트
        subFolder[0] = L'\0';
        break;
    default:
        if (options.organizeByMonth) {
            swprintf_s(subFolder, L"%04d\\%02d", dateTaken.wYear, dateTaken.wMonth);
        } else {
            swprintf_s(subFolder, L"%04d", dateTaken.wYear);
        }
        break;
    }

    wchar_t folderPath[MAX_PATH];
    if (subFolder[0] != L'\0') {
        swprintf_s(folderPath, L"%s\\%s", options.outputFolder.c_str(), subFolder);
    } else {
        swprintf_s(folderPath, L"%s", options.outputFolder.c_str());
    }

    // 1. Ensure directory exists (RAM-cached: calls SHCreateDirectoryExW only ONCE per folder!)
    bool needCreate = false;
    {
        std::lock_guard<std::mutex> lock(g_folderMutex);
        if (g_createdFolders.count(folderPath) == 0) {
            needCreate = true;
            g_createdFolders.insert(folderPath);
        }
    }
    if (needCreate) {
        SHCreateDirectoryExW(NULL, folderPath, NULL);
    }

    std::filesystem::path srcP(filename);
    std::wstring ext = srcP.extension().wstring();

    wchar_t baseFilename[MAX_PATH];
    swprintf_s(baseFilename, L"%04d%02d%02d_%02d%02d%02d",
        dateTaken.wYear, dateTaken.wMonth, dateTaken.wDay,
        dateTaken.wHour, dateTaken.wMinute, dateTaken.wSecond);

    std::wstring destPath;
    {
        // 2. Pure RAM destination path reservation (Zero disk I/O inside lock: 0.0001ms duration!)
        std::lock_guard<std::mutex> lock(g_destReservationMutex);

        destPath = std::wstring(folderPath) + L"\\" + baseFilename + ext;
        int counter = 1;
        while (g_reservedDestPaths.count(destPath) > 0 || PathFileExistsW(destPath.c_str())) {
            wchar_t disambig[MAX_PATH];
            swprintf_s(disambig, L"%s\\%s_%03d%s", folderPath, baseFilename, counter++, ext.c_str());
            destPath = disambig;
        }
        g_reservedDestPaths.insert(destPath);
    }

    outDestination = destPath;

    // 2. Immediate lightweight retry (Instant fixes: attributes or filename conflict)
    // Long locks (Sharing violations) are passed quickly to the final multi-pass loop to avoid thread blocking
    const int MAX_RETRIES = 2;
    bool success = false;
    DWORD lastError = 0;

    for (int attempt = 0; attempt < MAX_RETRIES && !g_bCancelRequested; ++attempt) {
        if (options.copyFiles) {
            success = (CopyFileW(filePath.c_str(), destPath.c_str(), FALSE) != 0);
        } else {
            success = (MoveFileExW(filePath.c_str(), destPath.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) != 0);
        }

        if (success) {
            break;
        }

        lastError = GetLastError();

        // If access denied, strip read-only attribute from source and retry
        if (lastError == ERROR_ACCESS_DENIED) {
            DWORD attrs = GetFileAttributesW(filePath.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(filePath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
            }
        }
        // If destination collided on disk, re-reserve a new numbered path
        else if (lastError == ERROR_FILE_EXISTS || lastError == ERROR_ALREADY_EXISTS) {
            std::lock_guard<std::mutex> lock(g_destReservationMutex);
            int counter = 1;
            while (PathFileExistsW(destPath.c_str()) || g_reservedDestPaths.count(destPath) > 0) {
                wchar_t disambig[MAX_PATH];
                swprintf_s(disambig, L"%s\\%s_%03d%s", folderPath, baseFilename, counter++, ext.c_str());
                destPath = disambig;
            }
            g_reservedDestPaths.insert(destPath);
            outDestination = destPath;
        }
        // If sharing violation (Antivirus/Indexer), don't block worker; quickly hand off to retry pass
        else if (lastError == ERROR_SHARING_VIOLATION || lastError == ERROR_LOCK_VIOLATION) {
            Sleep(15);
            continue;
        }

        Sleep(20);
    }

    if (!success) {
        // Free reserved path so retrying in next pass isn't blocked
        std::lock_guard<std::mutex> lock(g_destReservationMutex);
        g_reservedDestPaths.erase(destPath);
        if (!g_bIsRetryPass) {
            EnqueueLog(L"파일 이동/복사 보류 (" + FormatWin32Error(lastError) + L"): " + filename);
        }
    }

    return success;
}

// -------------------------------------------------------------
// Worker Thread Task Unit
// -------------------------------------------------------------
void WorkerThreadProc(int threadId)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    const size_t total = g_fileList.size();

    while (!g_bCancelRequested) {
        size_t idx = g_currentFileIndex.fetch_add(1);
        if (idx >= total) {
            break;
        }

        const auto& item = g_fileList[idx];

        try {
            // Memory-First 3-Stage Duplicate Check (Size -> Sample Hash -> MMF Zero-Copy memcmp)
            if (g_currentOptions.removeDuplicates && item.isDuplicateCandidate) {
                // Stage 2: Fast Sample Hash (16KB head + 16KB tail)
                uint64_t sampleHash = CalculateFastSampleHash(item.filePath, item.fileSize);

                std::vector<std::wstring> potentialMatches;
                {
                    std::lock_guard<std::mutex> lock(g_candidateMutex);
                    auto it = g_candidatesBySize.find(item.fileSize);
                    if (it != g_candidatesBySize.end()) {
                        for (const auto& rec : it->second) {
                            if (rec.sampleHash == sampleHash) {
                                potentialMatches.push_back(rec.filePath);
                            }
                        }
                    }
                }

                // Stage 3: Zero-Copy Memory-Mapped Full Comparison (with Early Exit)
                bool bIsDuplicate = false;
                for (const auto& existingPath : potentialMatches) {
                    if (PathFileExistsW(existingPath.c_str())) {
                        if (AreFilesContentIdentical(item.filePath, existingPath, item.fileSize)) {
                            // Exact duplicate confirmed!
                            DeleteFileW(item.filePath.c_str());
                            g_stats.duplicatesRemoved++;
                            g_stats.processedFiles++;
                            EnqueueLog(L"중복 파일 제거: " + item.filename);
                            bIsDuplicate = true;
                            break;
                        }
                    }
                }

                if (bIsDuplicate) {
                    continue;
                }

                // Register candidate in memory for subsequent files
                {
                    std::lock_guard<std::mutex> lock(g_candidateMutex);
                    g_candidatesBySize[item.fileSize].push_back({ sampleHash, item.filePath });
                }
            }

            // Extract date using RAM-cached metadata & fast EXIF
            SYSTEMTIME dateTaken;
            if (ExtractDate(item, dateTaken)) {
                std::wstring destPath;
                if (MoveOrCopyFileToDateFolder(item.filePath, item.filename, dateTaken, g_currentOptions, destPath)) {
                    g_stats.movedFiles++;
                    if (g_bIsRetryPass) {
                        if (g_stats.errors > 0) g_stats.errors--;
                    } else {
                        g_stats.processedFiles++;
                    }
                } else {
                    if (!g_bIsRetryPass) {
                        g_stats.errors++;
                        g_stats.processedFiles++;
                        std::lock_guard<std::mutex> lock(g_failedMutex);
                        g_failedFileList.push_back(item);
                    } else {
                        std::lock_guard<std::mutex> lock(g_failedMutex);
                        g_nextFailedFileList.push_back(item);
                    }
                }
            } else {
                if (!g_bIsRetryPass) {
                    g_stats.errors++;
                    g_stats.processedFiles++;
                    EnqueueLog(L"날짜 추출 실패: " + item.filename);
                }
            }
        }
        catch (...) {
            if (!g_bIsRetryPass) {
                g_stats.errors++;
                g_stats.processedFiles++;
                EnqueueLog(L"파일 처리 예외: " + item.filename);
            }
        }
    }

    CoUninitialize();
}

// -------------------------------------------------------------
// Bottom-Up Empty Directory Cleanup
// 내부에 파일이 없는 빈 폴더만 삭제하고, 어떤 파일이든 있으면 100% 보존
// -------------------------------------------------------------
static void CleanEmptyDirectories(const std::wstring& rootFolder)
{
    if (rootFolder.empty() || !PathIsDirectoryW(rootFolder.c_str())) return;

    try {
        std::vector<std::filesystem::path> allDirs;
        std::error_code ec;

        // Collect all subdirectories
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
            rootFolder, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_directory(ec)) {
                allDirs.push_back(entry.path());
            }
        }

        // Sort bottom-up: deepest paths first so nested empty folders collapse cleanly
        std::sort(allDirs.begin(), allDirs.end(), [](const auto& a, const auto& b) {
            return a.wstring().length() > b.wstring().length();
        });

        int removedCount = 0;
        for (const auto& dirPath : allDirs) {
            if (g_bCancelRequested) break;

            // 최상위 입력 폴더 루트는 절대 삭제하지 않음
            if (std::filesystem::equivalent(dirPath, rootFolder, ec)) continue;

            // 폴더 내부에 파일이나 하위 디렉터리가 1개라도 존재하는지 검사
            bool hasContent = false;
            for (const auto& child : std::filesystem::directory_iterator(
                dirPath, std::filesystem::directory_options::skip_permission_denied, ec)) {
                hasContent = true;
                break;
            }

            // 내부에 아무것도 없는 경우에만 삭제 (RemoveDirectoryW는 파일이 있으면 OS 차원에서 절대 삭제 불가)
            if (!hasContent) {
                if (RemoveDirectoryW(dirPath.c_str())) {
                    removedCount++;
                }
            }
        }

        if (removedCount > 0) {
            wchar_t cleanMsg[128];
            swprintf_s(cleanMsg, L"🧹 빈 폴더 정리: %d개 빈 폴더 삭제 완료 (파일 있는 폴더 보존)", removedCount);
            EnqueueLog(cleanMsg);
        }
    }
    catch (...) {}
}

// -------------------------------------------------------------
// Directory Scan, RAM Bucketing, and Worker Launch
// -------------------------------------------------------------
void ScanAndProcessThread()
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    EnqueueLog(L"입력 폴더 검색 시작: " + g_currentOptions.inputFolder);

    static const std::vector<std::wstring> validExts = {
        // 일반 사진 및 그래픽
        L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tiff", L".tif", L".webp",
        L".heic", L".heif", L".avif", L".jxl", L".psd", L".ico", L".svg", L".tga",
        // 카메라 전문 RAW 사진
        L".raw", L".dng", L".cr2", L".cr3", L".nef", L".nrw", L".arw", L".srf",
        L".sr2", L".orf", L".rw2", L".pef", L".raf",
        // 문서
        L".pdf",
        // 동영상 (MPG, VOB, TS, MTS/M2TS 등 포괄 지원)
        L".mp4", L".m4v", L".mov", L".mkv", L".avi", L".wmv", L".asf", L".flv",
        L".f4v", L".webm", L".3gp", L".3g2", L".ts", L".tp", L".trp", L".mts",
        L".m2ts", L".mpg", L".mpeg", L".mpe", L".vob", L".ogv", L".divx", L".rm", L".rmvb"
    };

    g_fileList.clear();
    {
        std::lock_guard<std::mutex> lock(g_folderMutex);
        g_createdFolders.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_destReservationMutex);
        g_reservedDestPaths.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_failedMutex);
        g_failedFileList.clear();
    }
    g_bIsRetryPass = false;

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
            g_currentOptions.inputFolder,
            std::filesystem::directory_options::skip_permission_denied)) {

            if (g_bCancelRequested) break;

            if (entry.is_regular_file()) {
                std::wstring ext = entry.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

                for (const auto& ve : validExts) {
                    if (ext == ve) {
                        FileItem item;
                        item.filePath = entry.path().wstring();
                        item.filename = entry.path().filename().wstring();
                        item.fileSize = entry.file_size();

                        // Fast Win32 metadata cache
                        WIN32_FILE_ATTRIBUTE_DATA attr;
                        if (GetFileAttributesExW(item.filePath.c_str(), GetFileExInfoStandard, &attr)) {
                            item.creationTime = attr.ftCreationTime;
                            item.lastWriteTime = attr.ftLastWriteTime;
                        }

                        g_fileList.push_back(std::move(item));
                        break;
                    }
                }
            }
        }
    }
    catch (const std::exception& e) {
        std::string err(e.what());
        EnqueueLog(L"폴더 탐색 중 오류: " + std::wstring(err.begin(), err.end()));
    }

    HWND hTarget = g_hOrganizerDlg ? g_hOrganizerDlg : g_hMainWnd;

    if (g_bCancelRequested) {
        PostMessageW(hTarget, WM_APP_WORK_FINISHED, 0, 0);
        CoUninitialize();
        return;
    }

    size_t total = g_fileList.size();
    g_stats.totalFiles = (int)total;
    g_currentFileIndex = 0;

    wchar_t foundMsg[128];
    swprintf_s(foundMsg, L"총 %d개 대상 파일 발견", (int)total);
    EnqueueLog(foundMsg);

    if (total == 0) {
        EnqueueLog(L"처리할 미디어 파일이 없습니다.");
        PostMessageW(hTarget, WM_APP_WORK_FINISHED, 0, 0);
        CoUninitialize();
        return;
    }

    // RAM Phase 1: High-Speed File Size Bucketing
    {
        std::unordered_map<uint64_t, int> sizeFrequencies;
        sizeFrequencies.reserve(total);
        for (const auto& item : g_fileList) {
            sizeFrequencies[item.fileSize]++;
        }

        int candidateCount = 0;
        for (auto& item : g_fileList) {
            if (sizeFrequencies[item.fileSize] > 1) {
                item.isDuplicateCandidate = true;
                candidateCount++;
            } else {
                item.isDuplicateCandidate = false;
            }
        }

        wchar_t ramMsg[128];
        swprintf_s(ramMsg, L"⚡ RAM 사전 분석 완료: 총 %d개 중 동일 크기 중복 후보 %d개 선별 (비중복 %d개 해시 I/O 생략)",
            (int)total, candidateCount, (int)total - candidateCount);
        EnqueueLog(ramMsg);
    }

    // Launch single sequential worker pipeline (Zero lock contention, maximum MFT throughput)
    EnqueueLog(L"⚡ 락 경합 없는 최고 성능 단일 파이프라인으로 초고속 파일 정리 시작!");

    g_workerThreads.clear();
    g_workerThreads.emplace_back(WorkerThreadProc, 1);

    // Wait for worker to complete (Pass 1)
    if (g_workerThreads[0].joinable()) {
        g_workerThreads[0].join();
    }
    g_workerThreads.clear();

    // -------------------------------------------------------------
    // Progressive Multi-Pass Convergence Retry Loop
    // "추가로 성공이 생기면 계속 재시도하고, 개선이 없으면 즉시 안전 종료"
    // -------------------------------------------------------------
    int passNumber = 2;
    const int MAX_RETRY_PASSES = 6;

    while (!g_bCancelRequested && !g_failedFileList.empty() && passNumber <= MAX_RETRY_PASSES) {
        size_t beforeFailCount = g_failedFileList.size();

        wchar_t retryMsg[128];
        swprintf_s(retryMsg, L"일시적 파일 잠금/보류 %d개 파일 재시도 진행 (Pass %d)...", (int)beforeFailCount, passNumber);
        EnqueueLog(retryMsg);

        // 외부 프로세스(백신, 인덱서) 락이 풀릴 수 있도록 잠시 대기
        Sleep(350);

        g_fileList = std::move(g_failedFileList);
        g_failedFileList.clear();
        {
            std::lock_guard<std::mutex> lock(g_failedMutex);
            g_nextFailedFileList.clear();
        }
        g_currentFileIndex = 0;
        g_bIsRetryPass = true;

        int prevMoved = g_stats.movedFiles.load();

        // 재시도 시에도 단일 파이프라인으로 충돌 없이 순차 처리
        g_workerThreads.emplace_back(WorkerThreadProc, 1);
        if (g_workerThreads[0].joinable()) {
            g_workerThreads[0].join();
        }
        g_workerThreads.clear();

        int newlySucceeded = g_stats.movedFiles.load() - prevMoved;
        g_failedFileList = std::move(g_nextFailedFileList);
        size_t afterFailCount = g_failedFileList.size();

        wchar_t passResultMsg[128];
        swprintf_s(passResultMsg, L"Pass %d 결과: +%d개 추가 성공! (잔여 보류: %d개)",
            passNumber, newlySucceeded, (int)afterFailCount);
        EnqueueLog(passResultMsg);

        // 진전(추가 성공)이 없거나 모든 파일이 해결되었으면 즉시 루프 탈출
        if (afterFailCount == 0) {
            EnqueueLog(L"모든 보류 파일이 100% 성공적으로 정리 완료되었습니다!");
            break;
        }

        if (newlySucceeded == 0) {
            EnqueueLog(L"더 이상 추가 개선(잠금 해제)이 없어 재시도를 안전하게 종료합니다.");
            break;
        }

        passNumber++;
    }
    g_bIsRetryPass = false;

    // 정리가 끝난 후, 내부에 파일이 전혀 없는 빈 하위 폴더 자동 삭제 (파일이 있는 폴더는 100% 안전 보존)
    if (!g_bCancelRequested) {
        CleanEmptyDirectories(g_currentOptions.inputFolder);
    }

    hTarget = g_hOrganizerDlg ? g_hOrganizerDlg : g_hMainWnd;
    PostMessageW(hTarget, WM_APP_WORK_FINISHED, 0, 0);
    CoUninitialize();
}

// -------------------------------------------------------------
// UI Layout and Control Creation for Main Dashboard Launcher
// -------------------------------------------------------------
void CreateMainControls(HWND hWnd)
{
    // 1. Header (Title & Subtitle)
    g_hMainTitle = CreateWindowExW(0, L"STATIC", L"PICTO 사진 & 미디어 스튜디오",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_MAIN_TITLE, g_hInst, NULL);

    g_hMainSubtitle = CreateWindowExW(0, L"STATIC", L"원하는 작업을 선택하세요. 모든 기능은 개별 전용 창에서 독립적으로 안전하게 실행됩니다.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_MAIN_SUBTITLE, g_hInst, NULL);

    // 2. Main 5 Feature Buttons & Descriptions (Buttons have wide 200px+ width to prevent text clipping)
    // (1) Fast File Organizer
    g_hBtnStart = CreateWindowExW(0, L"BUTTON", L"⚡ 고속 파일 정리",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_START, g_hInst, NULL);
    g_hDescOrganizer = CreateWindowExW(0, L"STATIC", L"MFT 락 없는 초고속 단일 파이프라인으로 촬영일시 기준 연/월별 폴더 자동 분류 및 정리",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_MAIN_DESC_ORGANIZER, g_hInst, NULL);

    // (2) Duplicate Photo Cleaner
    g_hBtnDupClean = CreateWindowExW(0, L"BUTTON", L"📁 중복 사진 정리",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_DUP_CLEAN, g_hInst, NULL);
    g_hDescDup = CreateWindowExW(0, L"STATIC", L"RAM 크기 버킷 필터 및 제로 카피 MMF 3단계 검증으로 동일 중복 파일 안전 검출 & 삭제",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_MAIN_DESC_DUP, g_hInst, NULL);

    // (3) Resized Image Cleaner
    g_hBtnResizedClean = CreateWindowExW(0, L"BUTTON", L"🖼️ 축소 사진 정리",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_RESIZED_CLEAN, g_hInst, NULL);
    g_hDescResized = CreateWindowExW(0, L"STATIC", L"지각 해시(pHash)와 2D-DCT 가속으로 카카오톡 축소본 및 해상도 다른 사진 정밀 검출",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_MAIN_DESC_RESIZED, g_hInst, NULL);

    // (4) Photo GPS Map
    g_hBtnGpsMap = CreateWindowExW(0, L"BUTTON", L"🗺️ 사진 GPS 지도",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_GPS_MAP, g_hInst, NULL);
    g_hDescGps = CreateWindowExW(0, L"STATIC", L"APP1 바이너리 및 Shell 속성 초고속 GPS 좌표 추출 & 웹 지도 촬영 위치 시각화",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_MAIN_DESC_GPS, g_hInst, NULL);

    // (5) EXIF Date Editor
    g_hBtnExifEdit = CreateWindowExW(0, L"BUTTON", L"📅 EXIF 날짜 수정",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_EXIF_EDIT, g_hInst, NULL);
    g_hDescExif = CreateWindowExW(0, L"STATIC", L"무손실 0.001ms 인플레이스 바이너리 패치 엔진으로 촬영일시 및 파일 타임스탬프 일괄 수정",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_MAIN_DESC_EXIF, g_hInst, NULL);

    // 3. Footer Status
    unsigned int cpuCount = std::thread::hardware_concurrency();
    if (cpuCount == 0) cpuCount = 4;
    wchar_t footerBuf[256];
    swprintf_s(footerBuf, L"⚡ 시스템 성능: MFT 단일 파이프라인 & %u개 CPU 코어 가속 풀 가동 | Per-Monitor V2 고DPI 지원", cpuCount);
    g_hMainFooter = CreateWindowExW(0, L"STATIC", footerBuf,
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_MAIN_FOOTER, g_hInst, NULL);

    CreateDpiFonts(g_currentDpi);
    ApplyFontsToControls(hWnd);
}

void UpdateLayout(HWND hWnd, int clientWidth, int clientHeight)
{
    if (clientWidth <= 0 || clientHeight <= 0) return;

    UINT dpi = g_currentDpi;
    int marginX = ScaleDpi(28, dpi);
    int marginY = ScaleDpi(24, dpi);
    int contentW = clientWidth - (marginX * 2);

    int curY = marginY;

    // Title
    int titleH = ScaleDpi(30, dpi);
    SetWindowPos(g_hMainTitle, NULL, marginX, curY, contentW, titleH, SWP_NOZORDER);
    curY += titleH + ScaleDpi(4, dpi);

    // Subtitle
    int subH = ScaleDpi(20, dpi);
    SetWindowPos(g_hMainSubtitle, NULL, marginX, curY, contentW, subH, SWP_NOZORDER);
    curY += subH + ScaleDpi(22, dpi);

    // 5 Action Cards (Wide button + clear descriptive label)
    int btnW = ScaleDpi(200, dpi);
    int cardH = ScaleDpi(48, dpi);
    int gapY = ScaleDpi(12, dpi);
    int textPadX = ScaleDpi(18, dpi);
    int descW = contentW - btnW - textPadX;
    int descTextYOffset = ScaleDpi(13, dpi);

    HWND buttons[5] = { g_hBtnStart, g_hBtnDupClean, g_hBtnResizedClean, g_hBtnGpsMap, g_hBtnExifEdit };
    HWND descs[5] = { g_hDescOrganizer, g_hDescDup, g_hDescResized, g_hDescGps, g_hDescExif };

    for (int i = 0; i < 5; ++i) {
        if (buttons[i]) {
            SetWindowPos(buttons[i], NULL, marginX, curY, btnW, cardH, SWP_NOZORDER);
        }
        if (descs[i]) {
            SetWindowPos(descs[i], NULL, marginX + btnW + textPadX, curY + descTextYOffset,
                         descW, cardH - descTextYOffset, SWP_NOZORDER);
        }
        curY += cardH + gapY;
    }

    // Footer at the bottom
    int footerH = ScaleDpi(20, dpi);
    int footerY = clientHeight - marginY - footerH;
    if (footerY > curY) {
        SetWindowPos(g_hMainFooter, NULL, marginX, footerY, contentW, footerH, SWP_NOZORDER);
    } else {
        SetWindowPos(g_hMainFooter, NULL, marginX, curY + ScaleDpi(10, dpi), contentW, footerH, SWP_NOZORDER);
    }
}

// -------------------------------------------------------------
// Fast Organizer Dialog (IDD_ORGANIZER) Layout & Procedure
// -------------------------------------------------------------
static void UpdateOrganizerDialogLayout(HWND hDlg, int cx, int cy)
{
    if (cx <= 0 || cy <= 0) return;

    UINT dpi = GetWindowDpi(hDlg);
    int margin = ScaleDpi(12, dpi);
    int pad = ScaleDpi(6, dpi);
    int contentW = cx - (margin * 2);

    int curY = margin;

    // 1. Folder settings group
    HWND hGrpFolder = GetDlgItem(hDlg, IDC_GRP_FOLDER);
    HWND hLblIn = GetDlgItem(hDlg, IDC_STATIC_LBL_INPUT);
    HWND hEditIn = GetDlgItem(hDlg, IDC_EDIT_INPUT);
    HWND hBtnIn = GetDlgItem(hDlg, IDC_BTN_BROWSE_INPUT);
    HWND hLblOut = GetDlgItem(hDlg, IDC_STATIC_LBL_OUTPUT);
    HWND hEditOut = GetDlgItem(hDlg, IDC_EDIT_OUTPUT);
    HWND hBtnOut = GetDlgItem(hDlg, IDC_BTN_BROWSE_OUTPUT);

    int grpFolderH = ScaleDpi(78, dpi);
    SetWindowPos(hGrpFolder, NULL, margin, curY, contentW, grpFolderH, SWP_NOZORDER);

    int btnW = ScaleDpi(65, dpi);
    int lblW = ScaleDpi(65, dpi);
    int rowH = ScaleDpi(22, dpi);
    int innerY = curY + ScaleDpi(20, dpi);
    int editW = contentW - lblW - btnW - ScaleDpi(24, dpi);

    SetWindowPos(hLblIn, NULL, margin + ScaleDpi(10, dpi), innerY + ScaleDpi(2, dpi), lblW, rowH, SWP_NOZORDER);
    SetWindowPos(hEditIn, NULL, margin + ScaleDpi(10, dpi) + lblW, innerY, editW, rowH, SWP_NOZORDER);
    SetWindowPos(hBtnIn, NULL, margin + contentW - ScaleDpi(10, dpi) - btnW, innerY, btnW, rowH, SWP_NOZORDER);

    innerY += rowH + ScaleDpi(6, dpi);
    SetWindowPos(hLblOut, NULL, margin + ScaleDpi(10, dpi), innerY + ScaleDpi(2, dpi), lblW, rowH, SWP_NOZORDER);
    SetWindowPos(hEditOut, NULL, margin + ScaleDpi(10, dpi) + lblW, innerY, editW, rowH, SWP_NOZORDER);
    SetWindowPos(hBtnOut, NULL, margin + contentW - ScaleDpi(10, dpi) - btnW, innerY, btnW, rowH, SWP_NOZORDER);

    curY += grpFolderH + pad;

    // 2. Options group
    HWND hGrpOpts = GetDlgItem(hDlg, IDC_GRP_OPTS);
    HWND hLblFmt = GetDlgItem(hDlg, IDC_STATIC_LBL_FOLDER_FORMAT);
    HWND hComboFmt = GetDlgItem(hDlg, IDC_COMBO_FOLDER_FORMAT);
    HWND hChkCopy = GetDlgItem(hDlg, IDC_CHK_COPY_FILES);
    HWND hChkDups = GetDlgItem(hDlg, IDC_CHK_REMOVE_DUPS);

    int grpOptsH = ScaleDpi(48, dpi);
    SetWindowPos(hGrpOpts, NULL, margin, curY, contentW, grpOptsH, SWP_NOZORDER);

    innerY = curY + ScaleDpi(18, dpi);
    int lblFmtW = ScaleDpi(85, dpi);
    int comboW = ScaleDpi(185, dpi);
    int chkCopyW = ScaleDpi(185, dpi);
    int chkDupsW = ScaleDpi(210, dpi);

    int curOptX = margin + ScaleDpi(12, dpi);
    SetWindowPos(hLblFmt, NULL, curOptX, innerY + ScaleDpi(2, dpi), lblFmtW, rowH, SWP_NOZORDER);
    curOptX += lblFmtW + ScaleDpi(6, dpi);

    SetWindowPos(hComboFmt, NULL, curOptX, innerY, comboW, rowH, SWP_NOZORDER);
    curOptX += comboW + ScaleDpi(18, dpi);

    SetWindowPos(hChkCopy, NULL, curOptX, innerY, chkCopyW, rowH, SWP_NOZORDER);
    curOptX += chkCopyW + ScaleDpi(16, dpi);

    SetWindowPos(hChkDups, NULL, curOptX, innerY, chkDupsW, rowH, SWP_NOZORDER);

    curY += grpOptsH + pad;

    // 3. Progress group
    HWND hGrpProg = GetDlgItem(hDlg, IDC_GRP_PROGRESS);
    HWND hLblStatus = GetDlgItem(hDlg, IDC_STATIC_STATUS);
    HWND hProgress = GetDlgItem(hDlg, IDC_PROGRESS_BAR);
    HWND hStat1 = GetDlgItem(hDlg, IDC_STATIC_STAT_PROCESSED);
    HWND hStat2 = GetDlgItem(hDlg, IDC_STATIC_STAT_MOVED);
    HWND hStat3 = GetDlgItem(hDlg, IDC_STATIC_STAT_DUPS);
    HWND hStat4 = GetDlgItem(hDlg, IDC_STATIC_STAT_SPEED);

    int grpProgH = ScaleDpi(90, dpi);
    SetWindowPos(hGrpProg, NULL, margin, curY, contentW, grpProgH, SWP_NOZORDER);

    innerY = curY + ScaleDpi(18, dpi);
    SetWindowPos(hLblStatus, NULL, margin + ScaleDpi(10, dpi), innerY, contentW - ScaleDpi(20, dpi), ScaleDpi(16, dpi), SWP_NOZORDER);

    innerY += ScaleDpi(18, dpi);
    SetWindowPos(hProgress, NULL, margin + ScaleDpi(10, dpi), innerY, contentW - ScaleDpi(20, dpi), ScaleDpi(16, dpi), SWP_NOZORDER);

    innerY += ScaleDpi(20, dpi);
    int statW = (contentW - ScaleDpi(20, dpi)) / 4;
    SetWindowPos(hStat1, NULL, margin + ScaleDpi(10, dpi), innerY, statW, ScaleDpi(16, dpi), SWP_NOZORDER);
    SetWindowPos(hStat2, NULL, margin + ScaleDpi(10, dpi) + statW, innerY, statW, ScaleDpi(16, dpi), SWP_NOZORDER);
    SetWindowPos(hStat3, NULL, margin + ScaleDpi(10, dpi) + statW * 2, innerY, statW, ScaleDpi(16, dpi), SWP_NOZORDER);
    SetWindowPos(hStat4, NULL, margin + ScaleDpi(10, dpi) + statW * 3, innerY, statW, ScaleDpi(16, dpi), SWP_NOZORDER);

    curY += grpProgH + pad;

    // 4. Bottom action buttons
    int btnH = ScaleDpi(32, dpi);
    int bottomY = cy - margin - btnH;

    HWND hBtnStart = GetDlgItem(hDlg, IDC_BTN_START);
    HWND hBtnCancel = GetDlgItem(hDlg, IDC_BTN_CANCEL);
    HWND hBtnClose = GetDlgItem(hDlg, IDCANCEL);

    int startW = ScaleDpi(150, dpi);
    int cancelW = ScaleDpi(80, dpi);
    int closeW = ScaleDpi(70, dpi);

    SetWindowPos(hBtnStart, NULL, margin, bottomY, startW, btnH, SWP_NOZORDER);
    SetWindowPos(hBtnCancel, NULL, margin + startW + pad, bottomY, cancelW, btnH, SWP_NOZORDER);
    SetWindowPos(hBtnClose, NULL, cx - margin - closeW, bottomY, closeW, btnH, SWP_NOZORDER);

    // 5. Log group fills the remaining height between Progress and Bottom Buttons
    HWND hGrpLog = GetDlgItem(hDlg, IDC_GRP_LOG);
    HWND hEditLog = GetDlgItem(hDlg, IDC_EDIT_LOG);

    int logGrpH = bottomY - curY - pad;
    if (logGrpH > ScaleDpi(60, dpi)) {
        SetWindowPos(hGrpLog, NULL, margin, curY, contentW, logGrpH, SWP_NOZORDER);
        SetWindowPos(hEditLog, NULL, margin + ScaleDpi(10, dpi), curY + ScaleDpi(18, dpi),
            contentW - ScaleDpi(20, dpi), logGrpH - ScaleDpi(26, dpi), SWP_NOZORDER);
    }
}

INT_PTR CALLBACK OrganizerDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
    {
        g_hOrganizerDlg = hDlg;
        UINT dpi = GetWindowDpi(hDlg);

        // Bind dialog controls
        g_hGrpFolder = GetDlgItem(hDlg, IDC_GRP_FOLDER);
        g_hLblInput = GetDlgItem(hDlg, IDC_STATIC_LBL_INPUT);
        g_hEditInput = GetDlgItem(hDlg, IDC_EDIT_INPUT);
        g_hBtnBrowseInput = GetDlgItem(hDlg, IDC_BTN_BROWSE_INPUT);

        g_hLblOutput = GetDlgItem(hDlg, IDC_STATIC_LBL_OUTPUT);
        g_hEditOutput = GetDlgItem(hDlg, IDC_EDIT_OUTPUT);
        g_hBtnBrowseOutput = GetDlgItem(hDlg, IDC_BTN_BROWSE_OUTPUT);

        g_hGrpOpts = GetDlgItem(hDlg, IDC_GRP_OPTS);
        g_hLblFolderFormat = GetDlgItem(hDlg, IDC_STATIC_LBL_FOLDER_FORMAT);
        g_hComboFolderFormat = GetDlgItem(hDlg, IDC_COMBO_FOLDER_FORMAT);
        g_hChkCopyFiles = GetDlgItem(hDlg, IDC_CHK_COPY_FILES);
        g_hChkRemoveDups = GetDlgItem(hDlg, IDC_CHK_REMOVE_DUPS);

        g_hGrpProgress = GetDlgItem(hDlg, IDC_GRP_PROGRESS);
        g_hLblStatus = GetDlgItem(hDlg, IDC_STATIC_STATUS);
        g_hProgressBar = GetDlgItem(hDlg, IDC_PROGRESS_BAR);
        g_hLblProcessed = GetDlgItem(hDlg, IDC_STATIC_STAT_PROCESSED);
        g_hLblMoved = GetDlgItem(hDlg, IDC_STATIC_STAT_MOVED);
        g_hLblDups = GetDlgItem(hDlg, IDC_STATIC_STAT_DUPS);
        g_hLblSpeed = GetDlgItem(hDlg, IDC_STATIC_STAT_SPEED);

        g_hGrpLog = GetDlgItem(hDlg, IDC_GRP_LOG);
        g_hEditLog = GetDlgItem(hDlg, IDC_EDIT_LOG);

        g_hBtnStart = GetDlgItem(hDlg, IDC_BTN_START);
        g_hBtnCancel = GetDlgItem(hDlg, IDC_BTN_CANCEL);

        // Populate folder organization format combobox (Year-only as top and default)
        if (g_hComboFolderFormat) {
            SendMessageW(g_hComboFolderFormat, CB_RESETCONTENT, 0, 0);

            struct FormatOption {
                const wchar_t* label;
                FolderFormatType type;
            } formats[] = {
                { L"연도별 (예: 2023\\)", FOLDER_FORMAT_YEAR_ONLY },
                { L"연도\\월별 (예: 2023\\05\\)", FOLDER_FORMAT_YEAR_MONTH },
                { L"연도\\연도-월별 (예: 2023\\2023-05\\)", FOLDER_FORMAT_YEAR_YEARMONTH },
                { L"연도-월별 (예: 2023-05\\)", FOLDER_FORMAT_YEARMONTH_DASH },
                { L"연도-월-일별 (예: 2023-05-14\\)", FOLDER_FORMAT_DATE_DASH },
                { L"연도\\월\\일별 (예: 2023\\05\\14\\)", FOLDER_FORMAT_YEAR_MONTH_DAY },
                { L"폴더 생성 안 함 (출력 폴더 루트)", FOLDER_FORMAT_NONE }
            };

            int selIdx = 0;
            for (int i = 0; i < 7; ++i) {
                int idx = (int)SendMessageW(g_hComboFolderFormat, CB_ADDSTRING, 0, (LPARAM)formats[i].label);
                SendMessageW(g_hComboFolderFormat, CB_SETITEMDATA, idx, (LPARAM)formats[i].type);
                if (formats[i].type == g_currentOptions.folderFormat) {
                    selIdx = idx;
                }
            }
            SendMessageW(g_hComboFolderFormat, CB_SETCURSEL, selIdx, 0);
            SendMessageW(g_hComboFolderFormat, CB_SETDROPPEDWIDTH, (WPARAM)ScaleDpi(240, dpi), 0);
        }

        // Default checkbox settings
        Button_SetCheck(g_hChkRemoveDups, BST_CHECKED);
        SendMessageW(g_hProgressBar, PBM_SETRANGE32, 0, 100);

        // Restore previously specified paths if available
        if (!g_currentOptions.inputFolder.empty()) {
            SetWindowTextW(g_hEditInput, g_currentOptions.inputFolder.c_str());
        }
        if (!g_currentOptions.outputFolder.empty()) {
            SetWindowTextW(g_hEditOutput, g_currentOptions.outputFolder.c_str());
        }

        CreateDpiFonts(dpi);
        ApplyFontsToControls(hDlg);

        // Center on parent window
        HWND hParent = GetParent(hDlg);
        if (hParent) {
            RECT rcParent, rcDlg;
            GetWindowRect(hParent, &rcParent);
            GetWindowRect(hDlg, &rcDlg);
            int x = rcParent.left + ((rcParent.right - rcParent.left) - (rcDlg.right - rcDlg.left)) / 2;
            int y = rcParent.top + ((rcParent.bottom - rcParent.top) - (rcDlg.bottom - rcDlg.top)) / 2;
            SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }

        RECT rc;
        GetClientRect(hDlg, &rc);
        UpdateOrganizerDialogLayout(hDlg, rc.right - rc.left, rc.bottom - rc.top);

        return TRUE;
    }

    case WM_GETMINMAXINFO:
    {
        LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
        UINT dpi = GetWindowDpi(hDlg);
        lpMMI->ptMinTrackSize.x = ScaleDpi(760, dpi);
        lpMMI->ptMinTrackSize.y = ScaleDpi(480, dpi);
        return TRUE;
    }

    case WM_SIZE:
        UpdateOrganizerDialogLayout(hDlg, LOWORD(lParam), HIWORD(lParam));
        break;

    case WM_TIMER:
        if (wParam == TIMER_STATS_UPDATE) {
            UpdateStatsDisplay(hDlg);
        }
        break;

    case WM_APP_WORK_FINISHED:
        OnProcessingFinished(hDlg);
        break;

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_BROWSE_INPUT) {
            std::wstring path;
            if (BrowseFolder(hDlg, path)) {
                SetWindowTextW(g_hEditInput, path.c_str());
            }
        }
        else if (id == IDC_BTN_BROWSE_OUTPUT) {
            std::wstring path;
            if (BrowseFolder(hDlg, path)) {
                SetWindowTextW(g_hEditOutput, path.c_str());
            }
        }
        else if (id == IDC_BTN_START) {
            StartOrganizing(hDlg);
        }
        else if (id == IDC_BTN_CANCEL) {
            CancelOrganizing();
        }
        else if (id == IDCANCEL) {
            if (g_bIsProcessing) {
                if (MessageBoxW(hDlg, L"파일 정리가 진행 중입니다. 작업을 중단하고 창을 닫으시겠습니까?",
                    L"확인", MB_ICONQUESTION | MB_YESNO) == IDYES) {
                    CancelOrganizing();
                    if (g_scannerThread.joinable()) {
                        g_scannerThread.join();
                    }
                    KillTimer(hDlg, TIMER_STATS_UPDATE);
                    g_hOrganizerDlg = NULL;
                    EndDialog(hDlg, IDCANCEL);
                }
            } else {
                KillTimer(hDlg, TIMER_STATS_UPDATE);
                g_hOrganizerDlg = NULL;
                EndDialog(hDlg, IDCANCEL);
            }
        }
        break;
    }

    case WM_DESTROY:
        KillTimer(hDlg, TIMER_STATS_UPDATE);
        g_hOrganizerDlg = NULL;
        break;
    }
    return FALSE;
}

void ShowOrganizerDialog(HWND hParent)
{
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_ORGANIZER), hParent, OrganizerDlgProc, 0);
}

// -------------------------------------------------------------
// Main Processing Initiation & Cancellation
// -------------------------------------------------------------
void StartOrganizing(HWND hWnd)
{
    wchar_t inPath[MAX_PATH] = { 0 };
    wchar_t outPath[MAX_PATH] = { 0 };
    GetWindowTextW(g_hEditInput, inPath, MAX_PATH);
    GetWindowTextW(g_hEditOutput, outPath, MAX_PATH);

    std::wstring strIn = inPath;
    std::wstring strOut = outPath;

    while (!strIn.empty() && iswspace(strIn.front())) strIn.erase(strIn.begin());
    while (!strIn.empty() && iswspace(strIn.back())) strIn.pop_back();
    while (!strOut.empty() && iswspace(strOut.front())) strOut.erase(strOut.begin());
    while (!strOut.empty() && iswspace(strOut.back())) strOut.pop_back();

    if (strIn.empty() || strOut.empty()) {
        MessageBoxW(hWnd, L"입력 폴더와 출력 폴더를 모두 선택해주세요.", L"경고", MB_ICONWARNING);
        return;
    }

    if (!PathIsDirectoryW(strIn.c_str())) {
        MessageBoxW(hWnd, L"입력 폴더가 존재하지 않거나 올바른 폴더가 아닙니다.", L"오류", MB_ICONERROR);
        return;
    }

    if (!PathIsDirectoryW(strOut.c_str())) {
        if (SHCreateDirectoryExW(NULL, strOut.c_str(), NULL) != ERROR_SUCCESS && !PathIsDirectoryW(strOut.c_str())) {
            MessageBoxW(hWnd, L"출력 폴더를 생성할 수 없습니다.", L"오류", MB_ICONERROR);
            return;
        }
    }

    g_currentOptions.inputFolder = strIn;
    g_currentOptions.outputFolder = strOut;
    g_currentOptions.threadCount = 1; // 락 경합 없는 최고 성능 단일 파이프라인 (초당 2,900장)

    if (g_hComboFolderFormat) {
        int sel = (int)SendMessageW(g_hComboFolderFormat, CB_GETCURSEL, 0, 0);
        if (sel >= 0) {
            g_currentOptions.folderFormat = (FolderFormatType)SendMessageW(g_hComboFolderFormat, CB_GETITEMDATA, sel, 0);
        }
    }
    g_currentOptions.organizeByMonth = (g_currentOptions.folderFormat == FOLDER_FORMAT_YEAR_MONTH ||
                                        g_currentOptions.folderFormat == FOLDER_FORMAT_YEAR_YEARMONTH);
    g_currentOptions.copyFiles = (Button_GetCheck(g_hChkCopyFiles) == BST_CHECKED);
    g_currentOptions.removeDuplicates = (Button_GetCheck(g_hChkRemoveDups) == BST_CHECKED);
    g_currentOptions.removeResized = false;

    g_stats.totalFiles = 0;
    g_stats.processedFiles = 0;
    g_stats.movedFiles = 0;
    g_stats.duplicatesRemoved = 0;
    g_stats.resizedRemoved = 0;
    g_stats.errors = 0;

    {
        std::lock_guard<std::mutex> lock(g_candidateMutex);
        g_candidatesBySize.clear();
    }

    g_bCancelRequested = false;
    g_bIsProcessing = true;
    g_startTime = std::chrono::steady_clock::now();

    EnableWindow(g_hBtnStart, FALSE);
    EnableWindow(g_hBtnCancel, TRUE);
    SendMessageW(g_hProgressBar, PBM_SETRANGE32, 0, 100);
    SendMessageW(g_hProgressBar, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_hLblStatus, L"파일 검색 중...");
    SetWindowTextW(g_hEditLog, L"");

    SetTimer(hWnd, TIMER_STATS_UPDATE, 150, NULL);

    if (g_scannerThread.joinable()) {
        g_scannerThread.join();
    }
    g_scannerThread = std::thread(ScanAndProcessThread);
}

void CancelOrganizing()
{
    if (g_bIsProcessing) {
        g_bCancelRequested = true;
        EnqueueLog(L"⚠️ 작업 취소 요청됨... 스레드 정리 중입니다.");
        SetWindowTextW(g_hLblStatus, L"작업 취소 중...");
        EnableWindow(g_hBtnCancel, FALSE);
    }
}

void UpdateStatsDisplay(HWND hWnd)
{
    int total = g_stats.totalFiles.load();
    int processed = g_stats.processedFiles.load();
    int moved = g_stats.movedFiles.load();
    int dups = g_stats.duplicatesRemoved.load();

    wchar_t buf[128];

    if (total > 0) {
        int progress = (int)(((double)processed / (double)total) * 100.0);
        SendMessageW(g_hProgressBar, PBM_SETPOS, progress, 0);

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - g_startTime).count();
        double speed = (elapsed > 0.0) ? (processed / elapsed) : 0.0;
        double remSeconds = (speed > 0.0) ? ((total - processed) / speed) : 0.0;

        swprintf_s(buf, L"진행률: %d%% (%d/%d) - 예상 남은 시간: %.0f초", progress, processed, total, remSeconds);
        SetWindowTextW(g_hLblStatus, buf);

        swprintf_s(buf, L"처리 속도: %.1f 파일/초", speed);
        SetWindowTextW(g_hLblSpeed, buf);
    }

    swprintf_s(buf, L"처리된 파일: %d", processed);
    SetWindowTextW(g_hLblProcessed, buf);

    swprintf_s(buf, L"이동된 파일: %d", moved);
    SetWindowTextW(g_hLblMoved, buf);

    swprintf_s(buf, L"중복 제거: %d", dups);
    SetWindowTextW(g_hLblDups, buf);

    DrainLogQueue(g_hEditLog);
}

void OnProcessingFinished(HWND hWnd)
{
    KillTimer(hWnd, TIMER_STATS_UPDATE);
    g_bIsProcessing = false;

    if (g_scannerThread.joinable()) {
        g_scannerThread.join();
    }

    auto now = std::chrono::steady_clock::now();
    double totalTime = std::chrono::duration<double>(now - g_startTime).count();
    g_stats.processingTimeSeconds = totalTime;

    int total = g_stats.totalFiles.load();
    int processed = g_stats.processedFiles.load();
    int moved = g_stats.movedFiles.load();
    int dups = g_stats.duplicatesRemoved.load();
    int resized = g_stats.resizedRemoved.load();
    int errs = g_stats.errors.load();

    double avgSpeed = (totalTime > 0.0) ? (processed / totalTime) : 0.0;

    SendMessageW(g_hProgressBar, PBM_SETPOS, 100, 0);
    SetWindowTextW(g_hLblStatus, g_bCancelRequested ? L"⚠️ 작업 취소됨" : L"✅ 완료");

    UpdateStatsDisplay(hWnd);
    DrainLogQueue(g_hEditLog);

    EnableWindow(g_hBtnStart, TRUE);
    EnableWindow(g_hBtnCancel, FALSE);

    wchar_t resultMsg[512];
    swprintf_s(resultMsg,
        L"%s\n\n"
        L"📊 처리 통계:\n"
        L"• 총 파일 수: %d\n"
        L"• 처리된 파일: %d\n"
        L"• 이동/복사된 파일: %d\n"
        L"• 제거된 중복 파일: %d\n"
        L"• 제거된 리사이즈 이미지: %d\n"
        L"• 오류 발생: %d\n\n"
        L"⚡ 성능 정보:\n"
        L"• 총 처리 시간: %.1f초\n"
        L"• 평균 처리 속도: %.1f 파일/초",
        g_bCancelRequested ? L"작업이 취소되었습니다." : L"🎉 고속 처리 완료!",
        total, processed, moved, dups, resized, errs, totalTime, avgSpeed);

    MessageBoxW(hWnd, resultMsg, L"완료", MB_ICONINFORMATION);
    EnqueueLog(L"==================================================");
    EnqueueLog(resultMsg);
    DrainLogQueue(g_hEditLog);
}
