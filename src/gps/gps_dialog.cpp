#include "gps_dialog.h"
#include "gps_engine.h"
#include "common/utils.h"
#include "common/preview_renderer.h"
#include "Resource.h"
#include "picto.h"

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <filesystem>
#include <algorithm>

// -------------------------------------------------------------
// GPS Map Dialog State & Globals
static std::vector<GpsPoint> g_batchGpsData;
static std::atomic<bool> g_bGpsScanning{ false };
static std::atomic<bool> g_bGpsCancel{ false };
static std::thread g_gpsScanThread;

static std::chrono::steady_clock::time_point g_gpsScanStartTime;
static unsigned int g_gpsActiveThreads = 0;
static size_t g_gpsTotalScannedFiles = 0;

static ImagePreviewRenderer g_gpsPreview;
static GpsPoint g_currentGpsPreviewPoint;
static std::vector<std::wstring> g_selectedGpsFiles;

void CleanupGpsScanThread()
{
    g_bGpsCancel = true;
    if (g_gpsScanThread.joinable()) {
        g_gpsScanThread.join();
    }
}

static void UpdateGpsPreview(HWND hDlg, const std::wstring& filePath, double lat, double lon)
{
    g_currentGpsPreviewPoint.filePath = filePath;
    g_currentGpsPreviewPoint.latitude = lat;
    g_currentGpsPreviewPoint.longitude = lon;

    g_gpsPreview.SetPath(filePath);

    HWND hPic = GetDlgItem(hDlg, IDC_GPS_PREVIEW_PIC);
    RECT rcPic;
    GetClientRect(hPic, &rcPic);
    int pw = rcPic.right - rcPic.left;
    int ph = rcPic.bottom - rcPic.top;
    if (pw > 10 && ph > 10) {
        g_gpsPreview.Load(filePath, pw, ph);
    }

    wchar_t infoBuf[256];
    if (g_gpsPreview.m_origW > 0 && g_gpsPreview.m_origH > 0) {
        swprintf_s(infoBuf, L"해상도: %d x %d | 용량: %s",
            g_gpsPreview.m_origW, g_gpsPreview.m_origH, FormatFileSize(g_gpsPreview.m_fileSize).c_str());
    } else if (g_gpsPreview.m_fileSize > 0) {
        swprintf_s(infoBuf, L"용량: %s", FormatFileSize(g_gpsPreview.m_fileSize).c_str());
    } else {
        swprintf_s(infoBuf, L"해상도 / 용량: 정보 없음");
    }
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_INFO), infoBuf);

    wchar_t coordBuf[256];
    if (lat != 0.0 || lon != 0.0) {
        swprintf_s(coordBuf, L"📍 위도: %.6f°  경도: %.6f°", lat, lon);
        EnableWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_BTN_MAP), TRUE);
    } else {
        swprintf_s(coordBuf, L"⚠️ GPS 좌표: 없음");
        EnableWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_BTN_MAP), FALSE);
    }
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_COORD), coordBuf);

    std::filesystem::path p(filePath);
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PATH), p.filename().wstring().c_str());

    InvalidateRect(hPic, NULL, FALSE);
}

static unsigned int GetGpsSelectedThreads(HWND hDlg)
{
    HWND hCombo = GetDlgItem(hDlg, IDC_GPS_COMBO_THREADS);
    int curSel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    unsigned int chosen = (curSel >= 0) ? (unsigned int)SendMessageW(hCombo, CB_GETITEMDATA, curSel, 0) : 0;
    if (chosen == 0 || chosen == (unsigned int)CB_ERR) {
        chosen = std::thread::hardware_concurrency();
        if (chosen == 0) chosen = 4;
    }
    return chosen;
}

static void RunMultiThreadedGpsScan(HWND hDlg, std::vector<std::wstring> filesToScan, unsigned int chosenThreads)
{
    const size_t total = filesToScan.size();
    g_gpsTotalScannedFiles = total;
    g_gpsScanStartTime = std::chrono::steady_clock::now();

    unsigned int numThreads = chosenThreads;
    if (numThreads == 0) numThreads = 4;
    g_gpsActiveThreads = numThreads;

    std::atomic<size_t> currentIndex{ 0 };
    std::atomic<size_t> processedCount{ 0 };
    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < numThreads; ++t) {
        workers.emplace_back([&, hDlg, total]() {
            CoInitializeEx(NULL, COINIT_MULTITHREADED);
            while (!g_bGpsCancel) {
                size_t idx = currentIndex.fetch_add(1);
                if (idx >= total) break;

                double lat = 0.0, lon = 0.0;
                if (ExtractGps(filesToScan[idx], lat, lon)) {
                    GpsPoint* pt = new GpsPoint{ filesToScan[idx], lat, lon };
                    PostMessageW(hDlg, WM_APP_GPS_ITEM_FOUND, (WPARAM)pt, 0);
                }

                size_t done = processedCount.fetch_add(1) + 1;
                if (done % 10 == 0 || done == total) {
                    PostMessageW(hDlg, WM_APP_GPS_PROGRESS, (WPARAM)done, (LPARAM)total);
                }
            }
            CoUninitialize();
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    PostMessageW(hDlg, WM_APP_GPS_FINISHED, (WPARAM)(g_bGpsCancel ? 1 : 0), (LPARAM)total);
}

static void StartGpsBatchScan(HWND hDlg, std::vector<std::wstring> filesToScan)
{
    if (filesToScan.empty()) return;

    HWND hList = GetDlgItem(hDlg, IDC_GPS_BATCH_LIST);
    ListView_DeleteAllItems(hList);
    g_batchGpsData.clear();
    g_gpsPreview.Clear();
    g_currentGpsPreviewPoint = { L"", 0.0, 0.0 };
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_INFO), L"해상도 / 용량: 선택 안 됨");
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_COORD), L"GPS 좌표: 선택 안 됨");
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PATH), L"경로: -");
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_BTN_MAP), FALSE);
    InvalidateRect(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PIC), NULL, FALSE);

    g_bGpsCancel = true;
    if (g_gpsScanThread.joinable()) {
        g_gpsScanThread.join();
    }

    g_bGpsScanning = true;
    g_bGpsCancel = false;

    unsigned int chosenThreads = GetGpsSelectedThreads(hDlg);
    g_gpsActiveThreads = chosenThreads;
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_COMBO_THREADS), FALSE);

    EnableWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_BROWSE), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_BROWSE), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_STOP), TRUE);
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_BTN_CLEAR), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), FALSE);

    SendMessageW(GetDlgItem(hDlg, IDC_GPS_BATCH_PROGRESS), PBM_SETPOS, 0, 0);

    wchar_t statusBuf[256];
    swprintf_s(statusBuf, L"멀티스레드 GPS 검색 시작: 총 %d개 사진 검사 중... (스레드 %u개)", (int)filesToScan.size(), chosenThreads);
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_STATUS), statusBuf);

    g_gpsScanThread = std::thread([hDlg, filesToScan = std::move(filesToScan), chosenThreads]() {
        RunMultiThreadedGpsScan(hDlg, std::move(filesToScan), chosenThreads);
    });
}

static void StartGpsFolderScan(HWND hDlg, std::wstring folderPath)
{
    if (folderPath.empty() || !PathIsDirectoryW(folderPath.c_str())) {
        MessageBoxW(hDlg, L"유효한 폴더 경로가 아닙니다.", L"경로 오류", MB_ICONWARNING);
        return;
    }

    HWND hList = GetDlgItem(hDlg, IDC_GPS_BATCH_LIST);
    ListView_DeleteAllItems(hList);
    g_batchGpsData.clear();
    g_gpsPreview.Clear();
    g_currentGpsPreviewPoint = { L"", 0.0, 0.0 };
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_INFO), L"해상도 / 용량: 선택 안 됨");
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_COORD), L"GPS 좌표: 선택 안 됨");
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PATH), L"경로: -");
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_BTN_MAP), FALSE);
    InvalidateRect(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PIC), NULL, FALSE);

    g_bGpsCancel = true;
    if (g_gpsScanThread.joinable()) {
        g_gpsScanThread.join();
    }

    g_bGpsScanning = true;
    g_bGpsCancel = false;

    unsigned int chosenThreads = GetGpsSelectedThreads(hDlg);
    g_gpsActiveThreads = chosenThreads;
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_COMBO_THREADS), FALSE);

    EnableWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_BROWSE), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_BROWSE), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_STOP), TRUE);
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_BTN_CLEAR), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), FALSE);

    SendMessageW(GetDlgItem(hDlg, IDC_GPS_BATCH_PROGRESS), PBM_SETPOS, 0, 0);
    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_STATUS), L"폴더 내 대상 사진 파일 목록을 검색하고 있습니다...");

    g_gpsScanThread = std::thread([hDlg, folderPath = std::move(folderPath), chosenThreads]() {
        std::vector<std::wstring> filesToScan;
        static const std::vector<std::wstring> imgExts = {
            L".jpg", L".jpeg", L".png", L".tiff", L".tif", L".heic", L".heif", L".webp", L".dng"
        };

        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                folderPath, std::filesystem::directory_options::skip_permission_denied)) {
                if (g_bGpsCancel) break;
                std::error_code ec;
                if (entry.is_regular_file(ec)) {
                    std::wstring ext = entry.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                    for (const auto& ve : imgExts) {
                        if (ext == ve) {
                            filesToScan.push_back(entry.path().wstring());
                            break;
                        }
                    }
                }
            }
        }
        catch (...) {}

        if (g_bGpsCancel || filesToScan.empty()) {
            PostMessageW(hDlg, WM_APP_GPS_FINISHED, (WPARAM)(g_bGpsCancel ? 1 : 0), (LPARAM)filesToScan.size());
            return;
        }

        RunMultiThreadedGpsScan(hDlg, std::move(filesToScan), chosenThreads);
    });
}

INT_PTR CALLBACK GpsMapDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
    {
        HWND hList = GetDlgItem(hDlg, IDC_GPS_BATCH_LIST);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        lvc.iSubItem = 0;
        lvc.pszText = (LPWSTR)L"파일명";
        lvc.cx = 150;
        ListView_InsertColumn(hList, 0, &lvc);

        lvc.iSubItem = 1;
        lvc.pszText = (LPWSTR)L"위도";
        lvc.cx = 95;
        ListView_InsertColumn(hList, 1, &lvc);

        lvc.iSubItem = 2;
        lvc.pszText = (LPWSTR)L"경도";
        lvc.cx = 95;
        ListView_InsertColumn(hList, 2, &lvc);

        lvc.iSubItem = 3;
        lvc.pszText = (LPWSTR)L"전체 경로";
        lvc.cx = 280;
        ListView_InsertColumn(hList, 3, &lvc);

        SendMessageW(GetDlgItem(hDlg, IDC_GPS_BATCH_PROGRESS), PBM_SETRANGE32, 0, 100);
        SendMessageW(GetDlgItem(hDlg, IDC_GPS_BATCH_PROGRESS), PBM_SETPOS, 0, 0);
        SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_STATUS), L"대기 중: '파일 선택 (다중)' 또는 '폴더 선택' 시 자동으로 GPS 스캔이 시작됩니다.");

        // Preview controls always visible
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_GRP), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PIC), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_INFO), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_COORD), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PATH), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_BTN_MAP), SW_SHOW);

        g_gpsPreview.Clear();
        g_selectedGpsFiles.clear();
        g_currentGpsPreviewPoint = { L"", 0.0, 0.0 };

        // Initialize Thread Selection Combobox
        HWND hCombo = GetDlgItem(hDlg, IDC_GPS_COMBO_THREADS);
        if (hCombo) {
            SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
            unsigned int hwThreads = std::thread::hardware_concurrency();
            if (hwThreads == 0) hwThreads = 4;

            std::vector<unsigned int> opts = { 1, 2, 4 };
            if (hwThreads >= 6) opts.push_back(6);
            if (hwThreads >= 8) opts.push_back(8);
            if (hwThreads >= 12) opts.push_back(12);
            if (hwThreads >= 16) opts.push_back(16);
            if (std::find(opts.begin(), opts.end(), hwThreads) == opts.end()) {
                opts.push_back(hwThreads);
            }
            std::sort(opts.begin(), opts.end());
            opts.erase(std::unique(opts.begin(), opts.end()), opts.end());

            int defIdx = -1;
            for (size_t i = 0; i < opts.size(); ++i) {
                wchar_t tbuf[32];
                if (opts[i] == hwThreads) {
                    swprintf_s(tbuf, L"%u개 (최대)", opts[i]);
                } else {
                    swprintf_s(tbuf, L"%u개", opts[i]);
                }
                int itemIdx = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)tbuf);
                SendMessageW(hCombo, CB_SETITEMDATA, itemIdx, (LPARAM)opts[i]);
                if (opts[i] == hwThreads) defIdx = itemIdx;
            }
            if (defIdx >= 0) {
                SendMessageW(hCombo, CB_SETCURSEL, defIdx, 0);
            } else {
                SendMessageW(hCombo, CB_SETCURSEL, (WPARAM)(opts.size() - 1), 0);
            }
        }

        RECT rcInit;
        GetClientRect(hDlg, &rcInit);
        SendMessageW(hDlg, WM_SIZE, 0, MAKELPARAM(rcInit.right, rcInit.bottom));

        return (INT_PTR)TRUE;
    }

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        if (pDIS->CtlID == IDC_GPS_PREVIEW_PIC) {
            g_gpsPreview.Draw(pDIS->hDC, pDIS->rcItem, L"사진 미리보기");
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_SIZE:
    {
        int clientW = LOWORD(lParam);
        int clientH = HIWORD(lParam);
        if (clientW <= 0 || clientH <= 0) break;

        // Dynamic 2-column calculation
        int previewW = (std::max)(240, (std::min)(440, (clientW * 38) / 100));
        int leftW = clientW - previewW - 20;
        if (leftW < 300) leftW = 300;

        // Top Controls
        int topY = 10;
        SetWindowPos(GetDlgItem(hDlg, IDC_STATIC), NULL, 10, topY + 2, 45, 18, SWP_NOZORDER);

        int btnFilesW = 105;
        int btnFolderW = 75;
        int buttonsTotalW = btnFilesW + btnFolderW + 8;
        int editW = leftW - 60 - buttonsTotalW;
        if (editW < 80) editW = 80;

        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_BATCH_EDIT), NULL, 58, topY, editW, 22, SWP_NOZORDER);

        int curBtnX = 58 + editW + 6;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_SINGLE_BROWSE), NULL, curBtnX, topY, btnFilesW, 24, SWP_NOZORDER);
        curBtnX += btnFilesW + 4;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_BATCH_BROWSE), NULL, curBtnX, topY, btnFolderW, 24, SWP_NOZORDER);

        // List Control
        int listY = topY + 28;
        int bottomSectionH = 72;
        int listH = (std::max)(80, clientH - listY - bottomSectionH - 8);
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_BATCH_LIST), NULL, 10, listY, leftW, listH, SWP_NOZORDER);

        // Bottom Controls
        int statusY = listY + listH + 4;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_BATCH_STATUS), NULL, 10, statusY, leftW, 16, SWP_NOZORDER);

        int progY = statusY + 18;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_BATCH_PROGRESS), NULL, 10, progY, leftW, 14, SWP_NOZORDER);

        int btnY = progY + 18;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_BATCH_STOP), NULL, 10, btnY, 70, 24, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_BTN_CLEAR), NULL, 85, btnY, 75, 24, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), NULL, 165, btnY, 135, 24, SWP_NOZORDER);

        int comboW = 75;
        int lblThW = 42;
        int comboX = 10 + leftW - comboW;
        int lblThX = comboX - 4 - lblThW;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_LBL_THREADS), NULL, lblThX, btnY + 4, lblThW, 18, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_COMBO_THREADS), NULL, comboX, btnY, comboW, 24, SWP_NOZORDER);

        // Right Preview Column
        int pX = leftW + 16;
        int pY = 7;
        int pH = clientH - 14;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_PREVIEW_GRP), NULL, pX, pY, previewW, pH, SWP_NOZORDER);

        int picY = pY + 22;
        int bottomInfoH = 120;
        int picH = (std::max)(80, pH - 24 - bottomInfoH);
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PIC), NULL, pX + 10, picY, previewW - 20, picH, SWP_NOZORDER);

        int curInfoY = picY + picH + 6;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_PREVIEW_INFO), NULL, pX + 10, curInfoY, previewW - 20, 16, SWP_NOZORDER);
        curInfoY += 18;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_PREVIEW_COORD), NULL, pX + 10, curInfoY, previewW - 20, 18, SWP_NOZORDER);
        curInfoY += 20;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PATH), NULL, pX + 10, curInfoY, previewW - 20, 18, SWP_NOZORDER);
        curInfoY += 22;
        SetWindowPos(GetDlgItem(hDlg, IDC_GPS_PREVIEW_BTN_MAP), NULL, pX + 10, curInfoY, previewW - 20, 28, SWP_NOZORDER);

        InvalidateRect(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PIC), NULL, FALSE);
        InvalidateRect(hDlg, NULL, TRUE);
        break;
    }

    case WM_NOTIFY:
    {
        NMHDR* pnm = (NMHDR*)lParam;
        if (pnm->idFrom == IDC_GPS_BATCH_LIST) {
            if (pnm->code == LVN_ITEMCHANGED) {
                NMLISTVIEW* pnmlv = (NMLISTVIEW*)lParam;
                if ((pnmlv->uChanged & LVIF_STATE) && (pnmlv->uNewState & LVIS_SELECTED)) {
                    int idx = pnmlv->iItem;
                    if (idx >= 0 && (size_t)idx < g_batchGpsData.size()) {
                        UpdateGpsPreview(hDlg, g_batchGpsData[idx].filePath, g_batchGpsData[idx].latitude, g_batchGpsData[idx].longitude);
                    }
                }
            }
            else if (pnm->code == NM_DBLCLK) {
                LPNMITEMACTIVATE pia = (LPNMITEMACTIVATE)lParam;
                if (pia->iItem >= 0 && (size_t)pia->iItem < g_batchGpsData.size()) {
                    OpenGoogleMap(g_batchGpsData[pia->iItem].latitude, g_batchGpsData[pia->iItem].longitude);
                }
            }
        }
        break;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_GPS_SINGLE_BROWSE) {
            std::vector<std::wstring> files;
            if (BrowseMultipleFiles(hDlg, files, L"이미지 파일 (*.jpg;*.jpeg;*.png;*.tiff;*.tif;*.heic;*.webp;*.dng)\0*.jpg;*.jpeg;*.png;*.tiff;*.tif;*.heic;*.webp;*.dng\0모든 파일 (*.*)\0*.*\0")) {
                if (files.size() == 1) {
                    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_EDIT), files[0].c_str());
                } else {
                    wchar_t selBuf[256];
                    swprintf_s(selBuf, L"선택된 파일: %d개 (%s 등)", (int)files.size(),
                        std::filesystem::path(files[0]).filename().wstring().c_str());
                    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_EDIT), selBuf);
                }
                StartGpsBatchScan(hDlg, std::move(files));
            }
        }
        else if (id == IDC_GPS_BATCH_BROWSE) {
            std::wstring folderPath;
            if (BrowseFolder(hDlg, folderPath)) {
                SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_EDIT), folderPath.c_str());
                StartGpsFolderScan(hDlg, folderPath);
            }
        }
        else if (id == IDC_GPS_BATCH_STOP) {
            g_bGpsCancel = true;
            SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_STATUS), L"스캔 중지 요청 중... 잠시 기다려주세요.");
            EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_STOP), FALSE);
        }
        else if (id == IDC_GPS_BTN_CLEAR) {
            ListView_DeleteAllItems(GetDlgItem(hDlg, IDC_GPS_BATCH_LIST));
            g_batchGpsData.clear();
            g_selectedGpsFiles.clear();
            g_gpsPreview.Clear();
            g_currentGpsPreviewPoint = { L"", 0.0, 0.0 };
            SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_EDIT), L"");
            SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_INFO), L"해상도 / 용량: 선택 안 됨");
            SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_COORD), L"GPS 좌표: 선택 안 됨");
            SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PATH), L"경로: -");
            EnableWindow(GetDlgItem(hDlg, IDC_GPS_PREVIEW_BTN_MAP), FALSE);
            InvalidateRect(GetDlgItem(hDlg, IDC_GPS_PREVIEW_PIC), NULL, FALSE);

            SendMessageW(GetDlgItem(hDlg, IDC_GPS_BATCH_PROGRESS), PBM_SETPOS, 0, 0);
            SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_STATUS), L"목록이 초기화되었습니다.");
            EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), FALSE);
        }
        else if (id == IDC_GPS_BATCH_VIEWALL) {
            HWND hList = GetDlgItem(hDlg, IDC_GPS_BATCH_LIST);
            int selectedCount = ListView_GetSelectedCount(hList);
            std::vector<GpsPoint> viewPoints;

            if (selectedCount > 0) {
                int itemIdx = -1;
                while ((itemIdx = ListView_GetNextItem(hList, itemIdx, LVNI_SELECTED)) != -1) {
                    if (itemIdx >= 0 && (size_t)itemIdx < g_batchGpsData.size()) {
                        viewPoints.push_back(g_batchGpsData[itemIdx]);
                    }
                }
            } else {
                viewPoints = g_batchGpsData;
            }

            if (!viewPoints.empty()) {
                OpenGoogleMapForPoints(viewPoints);
            }
        }
        else if (id == IDC_GPS_PREVIEW_BTN_MAP || id == IDC_GPS_SINGLE_VIEW) {
            if (g_currentGpsPreviewPoint.latitude != 0.0 || g_currentGpsPreviewPoint.longitude != 0.0) {
                OpenGoogleMap(g_currentGpsPreviewPoint.latitude, g_currentGpsPreviewPoint.longitude);
            }
        }
        else if (id == IDCANCEL) {
            SendMessageW(hDlg, WM_CLOSE, 0, 0);
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_APP_GPS_ITEM_FOUND:
    {
        GpsPoint* pt = (GpsPoint*)wParam;
        if (!pt) break;

        int newIdx = (int)g_batchGpsData.size();
        g_batchGpsData.push_back(*pt);

        HWND hList = GetDlgItem(hDlg, IDC_GPS_BATCH_LIST);
        std::filesystem::path p(pt->filePath);
        std::wstring fn = p.filename().wstring();

        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT;
        lvi.iItem = newIdx;
        lvi.iSubItem = 0;
        lvi.pszText = (LPWSTR)fn.c_str();
        ListView_InsertItem(hList, &lvi);

        wchar_t coordBuf[64];
        swprintf_s(coordBuf, L"%.6f", pt->latitude);
        ListView_SetItemText(hList, newIdx, 1, coordBuf);

        swprintf_s(coordBuf, L"%.6f", pt->longitude);
        ListView_SetItemText(hList, newIdx, 2, coordBuf);

        ListView_SetItemText(hList, newIdx, 3, (LPWSTR)pt->filePath.c_str());

        EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), TRUE);

        delete pt;
        break;
    }

    case WM_APP_GPS_PROGRESS:
    {
        int done = (int)wParam;
        int total = (int)lParam;
        int pct = total > 0 ? (int)(((double)done / (double)total) * 100.0) : 0;

        SendMessageW(GetDlgItem(hDlg, IDC_GPS_BATCH_PROGRESS), PBM_SETPOS, pct, 0);

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - g_gpsScanStartTime).count();
        double speed = (elapsed > 0.05) ? (done / elapsed) : 0.0;

        wchar_t statusBuf[256];
        swprintf_s(statusBuf, L"GPS 분석 중: %d / %d (%d%%) | %.1f 파일/초 (스레드 %u개, GPS %d개 발견)",
            done, total, pct, speed, g_gpsActiveThreads, (int)g_batchGpsData.size());
        SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_STATUS), statusBuf);
        break;
    }

    case WM_APP_GPS_FINISHED:
    {
        g_bGpsScanning = false;
        if (g_gpsScanThread.joinable()) {
            g_gpsScanThread.join();
        }

        EnableWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_BROWSE), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_BROWSE), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_STOP), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_GPS_BTN_CLEAR), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_GPS_COMBO_THREADS), TRUE);

        bool bCancelled = (wParam == 1);
        int total = (int)lParam;

        if (!bCancelled) {
            SendMessageW(GetDlgItem(hDlg, IDC_GPS_BATCH_PROGRESS), PBM_SETPOS, 100, 0);
        }

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - g_gpsScanStartTime).count();
        double speed = (elapsed > 0.05 && total > 0) ? (total / elapsed) : 0.0;

        wchar_t finBuf[256];
        if (bCancelled) {
            swprintf_s(finBuf, L"스캔 중지됨: 총 %d개 파일 중 %d개 GPS 사진 발견 (소요: %.1f초, 스레드 %u개)",
                total, (int)g_batchGpsData.size(), elapsed, g_gpsActiveThreads);
        } else if (g_batchGpsData.empty()) {
            swprintf_s(finBuf, L"스캔 완료: 총 %d개 사진 검사 완료 (소요: %.1f초, 평균 %.1f 파일/초, 스레드 %u개, GPS 없음)",
                total, elapsed, speed, g_gpsActiveThreads);
        } else {
            swprintf_s(finBuf, L"스캔 완료! 총 %d개 검사 (소요: %.1f초, 평균 %.1f 파일/초, 스레드 %u개) | GPS 사진 %d개 발견",
                total, elapsed, speed, g_gpsActiveThreads, (int)g_batchGpsData.size());
        }
        SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_STATUS), finBuf);

        if (!g_batchGpsData.empty()) {
            EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), TRUE);
            HWND hList = GetDlgItem(hDlg, IDC_GPS_BATCH_LIST);
            ListView_SetItemState(hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            UpdateGpsPreview(hDlg, g_batchGpsData[0].filePath, g_batchGpsData[0].latitude, g_batchGpsData[0].longitude);
        }
        break;
    }

    case WM_CLOSE:
        g_bGpsCancel = true;
        if (g_gpsScanThread.joinable()) {
            g_gpsScanThread.join();
        }
        g_gpsPreview.Clear();
        g_selectedGpsFiles.clear();
        EndDialog(hDlg, IDCANCEL);
        return (INT_PTR)TRUE;
    }

    return (INT_PTR)FALSE;
}
