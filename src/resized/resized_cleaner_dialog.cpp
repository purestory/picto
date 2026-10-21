#include "resized_cleaner_dialog.h"
#include "common/utils.h"
#include "common/preview_renderer.h"
#include "resized/perceptual_hash.h"
#include "Resource.h"
#include "picto.h"

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <imm.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "imm32.lib")

extern HINSTANCE g_hInst;
extern HWND g_hEditInput;

// -------------------------------------------------------------
// Resized Image Cleaner State
// -------------------------------------------------------------
static std::vector<ResizedPairItem> g_resizedMatches;
static std::atomic<bool> g_bResizedScanning{ false };
static std::atomic<bool> g_bResizedCancel{ false };
static std::thread g_resizedScanThread;
static int g_resizedSortCol = 0;
static bool g_resizedSortAsc = true;
static bool g_bResizedUpdating = false;
static ImagePreviewRenderer g_resizedPreviewResized;
static ImagePreviewRenderer g_resizedPreviewOrig;

static std::vector<int> g_resizedVisibleIndices;

static std::chrono::steady_clock::time_point g_resizedScanStartTime;
static unsigned int g_resizedActiveThreads = 0;
static size_t g_resizedTotalScannedImages = 0;

static void UpdateResizedSummary(HWND hDlg)
{
    int totalVisible = (int)g_resizedVisibleIndices.size();
    int totalAll = (int)g_resizedMatches.size();
    int checkedCount = 0;
    uint64_t savedBytes = 0;

    for (int idx : g_resizedVisibleIndices) {
        if (g_resizedMatches[idx].isChecked) {
            checkedCount++;
            savedBytes += g_resizedMatches[idx].resizedSize;
        }
    }

    wchar_t buf[256];
    if (totalVisible == totalAll) {
        swprintf_s(buf, L"발견된 축소본: %d개 | 선택됨: %d개 (절약 가능 공간: %s)",
            totalAll, checkedCount, FormatFileSize(savedBytes).c_str());
    } else {
        swprintf_s(buf, L"표시된 축소본: %d개 (전체 %d개, 제외 %d개) | 선택됨: %d개 (절약 공간: %s)",
            totalVisible, totalAll, totalAll - totalVisible, checkedCount, FormatFileSize(savedBytes).c_str());
    }
    SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_STATIC_SUMMARY), buf);

    EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_BTN_DELETE), checkedCount > 0);
}

static void RefreshResizedListView(HWND hDlg)
{
    // Force-commit any active IME composition before reading edit texts
    HWND hExEdit = GetDlgItem(hDlg, IDC_RESIZED_EDIT_EXCLUDE);
    if (hExEdit) {
        HIMC hImc = ImmGetContext(hExEdit);
        if (hImc) {
            ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
            ImmReleaseContext(hExEdit, hImc);
        }
    }
    HWND hFilterEdit = GetDlgItem(hDlg, IDC_RESIZED_EDIT_FILTER);
    if (hFilterEdit) {
        HIMC hImc = ImmGetContext(hFilterEdit);
        if (hImc) {
            ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
            ImmReleaseContext(hFilterEdit, hImc);
        }
    }

    wchar_t filterBuf[256] = { 0 };
    GetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_EDIT_FILTER), filterBuf, 256);
    std::wstring filter = filterBuf;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::towlower);

    wchar_t exBuf[512] = { 0 };
    GetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_EDIT_EXCLUDE), exBuf, 512);
    std::vector<std::wstring> excludeTokens = ParseExcludeTokens(exBuf);

    std::vector<int> visibleIndices;
    visibleIndices.reserve(g_resizedMatches.size());
    for (size_t i = 0; i < g_resizedMatches.size(); ++i) {
        const auto& item = g_resizedMatches[i];
        if (!filter.empty() && item.lowerFilterKey.find(filter) == std::wstring::npos) {
            continue;
        }

        if (!excludeTokens.empty()) {
            std::wstring lowerResPath = item.resizedPath;
            std::transform(lowerResPath.begin(), lowerResPath.end(), lowerResPath.begin(), ::towlower);
            for (auto& ch : lowerResPath) { if (ch == L'/') ch = L'\\'; }

            std::wstring lowerOrigPath = item.origPath;
            std::transform(lowerOrigPath.begin(), lowerOrigPath.end(), lowerOrigPath.begin(), ::towlower);
            for (auto& ch : lowerOrigPath) { if (ch == L'/') ch = L'\\'; }

            bool isExcluded = false;
            for (const auto& tok : excludeTokens) {
                if (lowerResPath.find(tok) != std::wstring::npos || lowerOrigPath.find(tok) != std::wstring::npos) {
                    isExcluded = true;
                    break;
                }
            }
            if (isExcluded) continue;
        }

        visibleIndices.push_back((int)i);
    }
    g_resizedVisibleIndices = visibleIndices;

    std::sort(visibleIndices.begin(), visibleIndices.end(), [](int a, int b) {
        const auto& itemA = g_resizedMatches[a];
        const auto& itemB = g_resizedMatches[b];
        int cmp = 0;
        switch (g_resizedSortCol) {
        case 0: cmp = _wcsicmp(itemA.resizedName.c_str(), itemB.resizedName.c_str()); break;
        case 1: {
            uint64_t pixA = (uint64_t)itemA.resizedW * itemA.resizedH;
            uint64_t pixB = (uint64_t)itemB.resizedW * itemB.resizedH;
            if (pixA < pixB) cmp = -1; else if (pixA > pixB) cmp = 1;
            break;
        }
        case 2:
            if (itemA.resizedSize < itemB.resizedSize) cmp = -1;
            else if (itemA.resizedSize > itemB.resizedSize) cmp = 1;
            break;
        case 3: cmp = _wcsicmp(itemA.origName.c_str(), itemB.origName.c_str()); break;
        case 4: {
            uint64_t pixA = (uint64_t)itemA.origW * itemA.origH;
            uint64_t pixB = (uint64_t)itemB.origW * itemB.origH;
            if (pixA < pixB) cmp = -1; else if (pixA > pixB) cmp = 1;
            break;
        }
        case 5:
            if (itemA.origSize < itemB.origSize) cmp = -1;
            else if (itemA.origSize > itemB.origSize) cmp = 1;
            break;
        case 6:
            if (itemA.similarityPct < itemB.similarityPct) cmp = -1;
            else if (itemA.similarityPct > itemB.similarityPct) cmp = 1;
            break;
        case 7: cmp = _wcsicmp(itemA.resizedDir.c_str(), itemB.resizedDir.c_str()); break;
        case 8: cmp = _wcsicmp(itemA.origDir.c_str(), itemB.origDir.c_str()); break;
        default: break;
        }
        return g_resizedSortAsc ? (cmp < 0) : (cmp > 0);
    });

    HWND hList = GetDlgItem(hDlg, IDC_RESIZED_LIST);
    g_bResizedUpdating = true;
    SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hList);

    for (size_t row = 0; row < visibleIndices.size(); ++row) {
        int idx = visibleIndices[row];
        const auto& item = g_resizedMatches[idx];

        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = (int)row;
        lvi.iSubItem = 0;
        lvi.pszText = (LPWSTR)item.resizedName.c_str();
        lvi.lParam = (LPARAM)idx;
        ListView_InsertItem(hList, &lvi);

        ListView_SetItemText(hList, (int)row, 1, (LPWSTR)item.strResDim.c_str());
        ListView_SetItemText(hList, (int)row, 2, (LPWSTR)item.strResSize.c_str());
        ListView_SetItemText(hList, (int)row, 3, (LPWSTR)item.origName.c_str());
        ListView_SetItemText(hList, (int)row, 4, (LPWSTR)item.strOrigDim.c_str());
        ListView_SetItemText(hList, (int)row, 5, (LPWSTR)item.strOrigSize.c_str());
        ListView_SetItemText(hList, (int)row, 6, (LPWSTR)item.strSimilarity.c_str());
        ListView_SetItemText(hList, (int)row, 7, (LPWSTR)item.resizedDir.c_str());
        ListView_SetItemText(hList, (int)row, 8, (LPWSTR)item.origDir.c_str());

        ListView_SetCheckState(hList, (int)row, item.isChecked ? TRUE : FALSE);
    }

    SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, TRUE);
    g_bResizedUpdating = false;

    UpdateResizedSummary(hDlg);
}

INT_PTR CALLBACK ResizedCleanerDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
    {
        HICON hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_PICTO));
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);

        wchar_t inPath[MAX_PATH] = { 0 };
        if (g_hEditInput) {
            GetWindowTextW(g_hEditInput, inPath, MAX_PATH);
        }
        if (inPath[0] != L'\0' && PathIsDirectoryW(inPath)) {
            SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_EDIT_FOLDER), inPath);
        }

        HWND hList = GetDlgItem(hDlg, IDC_RESIZED_LIST);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        lvc.iSubItem = 0; lvc.pszText = (LPWSTR)L"축소본 파일명"; lvc.cx = 150;
        ListView_InsertColumn(hList, 0, &lvc);

        lvc.iSubItem = 1; lvc.pszText = (LPWSTR)L"축소본 해상도"; lvc.cx = 95;
        ListView_InsertColumn(hList, 1, &lvc);

        lvc.iSubItem = 2; lvc.pszText = (LPWSTR)L"축소본 용량"; lvc.cx = 85;
        ListView_InsertColumn(hList, 2, &lvc);

        lvc.iSubItem = 3; lvc.pszText = (LPWSTR)L"원본 파일명"; lvc.cx = 150;
        ListView_InsertColumn(hList, 3, &lvc);

        lvc.iSubItem = 4; lvc.pszText = (LPWSTR)L"원본 해상도"; lvc.cx = 95;
        ListView_InsertColumn(hList, 4, &lvc);

        lvc.iSubItem = 5; lvc.pszText = (LPWSTR)L"원본 용량"; lvc.cx = 85;
        ListView_InsertColumn(hList, 5, &lvc);

        lvc.iSubItem = 6; lvc.pszText = (LPWSTR)L"유사도"; lvc.cx = 70;
        ListView_InsertColumn(hList, 6, &lvc);

        lvc.iSubItem = 7; lvc.pszText = (LPWSTR)L"축소본 위치"; lvc.cx = 180;
        ListView_InsertColumn(hList, 7, &lvc);

        lvc.iSubItem = 8; lvc.pszText = (LPWSTR)L"원본 위치"; lvc.cx = 180;
        ListView_InsertColumn(hList, 8, &lvc);

        SetWindowSubclass(GetDlgItem(hDlg, IDC_RESIZED_EDIT_FILTER), FilterEditSubclassProc, 0, (DWORD_PTR)hDlg);
        SetWindowSubclass(GetDlgItem(hDlg, IDC_RESIZED_EDIT_EXCLUDE), FilterEditSubclassProc, 1, (DWORD_PTR)hDlg);
        SetWindowSubclass(GetDlgItem(hDlg, IDC_RESIZED_EDIT_FOLDER), FilterEditSubclassProc, 2, (DWORD_PTR)hDlg);

        SendMessageW(GetDlgItem(hDlg, IDC_RESIZED_EDIT_FILTER), EM_SETCUEBANNER, TRUE, (LPARAM)L"파일명/경로 검색어 입력 후 Enter");
        SendMessageW(GetDlgItem(hDlg, IDC_RESIZED_EDIT_EXCLUDE), EM_SETCUEBANNER, TRUE, (LPARAM)L"제외할 폴더명 입력 (예: temp, backup) - 콤마(,)로 여러 개 구분");

        g_resizedPreviewResized.Clear();
        g_resizedPreviewOrig.Clear();
        SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_LBL_RESIZED_INFO), L"축소본: 선택 안 됨");
        SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_LBL_ORIG_INFO), L"원본: 선택 안 됨");

        // Initialize Thread Selection Combobox
        HWND hCombo = GetDlgItem(hDlg, IDC_RESIZED_COMBO_THREADS);
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

        UpdateResizedSummary(hDlg);
        return (INT_PTR)TRUE;
    }

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* pMMI = (MINMAXINFO*)lParam;
        pMMI->ptMinTrackSize.x = 720;
        pMMI->ptMinTrackSize.y = 440;
        return (INT_PTR)TRUE;
    }

    case WM_SIZE:
    {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        if (w <= 0 || h <= 0) break;

        int margin = 12;
        int pad = 6;
        int btnW = 75;
        int rowH = 24;

        // Two-pane split: Left pane (List & Actions), Right pane (Previews)
        int previewW = (std::max)(220, (std::min)(480, (w * 34) / 100));
        int leftW = w - previewW - (margin * 2) - pad;
        if (leftW < 300) leftW = 300;
        int previewX = margin + leftW + pad;

        // Left Pane: Folder row
        int editW = leftW - 75 - (btnW * 2) - pad * 2;
        if (editW < 80) editW = 80;
        SetWindowPos(GetDlgItem(hDlg, IDC_STATIC), NULL, margin, 14, 70, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_EDIT_FOLDER), NULL, margin + 72, 12, editW, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_BROWSE), NULL, margin + 72 + editW + pad, 11, btnW, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_SCAN), NULL, margin + 72 + editW + pad + btnW + pad, 11, btnW, rowH, SWP_NOZORDER);

        // Status row & Progress (Status Text, Thread Label, Thread Combo, Stop Button)
        int comboW = 82;
        int lblThW = 42;
        int stopX = margin + leftW - btnW;
        int comboX = stopX - pad - comboW;
        int lblThX = comboX - pad - lblThW;
        int statusW = lblThX - margin - pad;
        if (statusW < 100) statusW = 100;

        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_STATIC_STATUS), NULL, margin, 40, statusW, 18, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_LBL_THREADS), NULL, lblThX, 40, lblThW, 18, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_COMBO_THREADS), NULL, comboX, 37, comboW, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_STOP), NULL, stopX, 37, btnW, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_PROGRESS), NULL, margin, 62, leftW, 14, SWP_NOZORDER);

        // Filter search row
        int searchBtnW = 46;
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_LBL_FILTER), NULL, margin, 82, 70, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_EDIT_FILTER), NULL, margin + 72, 80, leftW - 72 - searchBtnW - pad, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_SEARCH), NULL, margin + leftW - searchBtnW, 80, searchBtnW, rowH, SWP_NOZORDER);

        // Exclude Folder row
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_LBL_EXCLUDE), NULL, margin, 108, 70, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_EDIT_EXCLUDE), NULL, margin + 72, 106, leftW - 72, rowH, SWP_NOZORDER);

        // Listview
        int bottomReserve = 66;
        int listTop = 134;
        int listH = h - listTop - bottomReserve;
        if (listH > 50) {
            SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_LIST), NULL, margin, listTop, leftW, listH, SWP_NOZORDER);
        }

        // Summary label
        int summaryY = h - 56;
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_STATIC_SUMMARY), NULL, margin, summaryY, leftW, 18, SWP_NOZORDER);

        // Action Buttons row
        int btnY = h - 34;
        int actBtnH = 26;
        int x = margin;
        int subBtnW = 60;
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_SELECT_ALL), NULL, x, btnY, subBtnW, actBtnH, SWP_NOZORDER);
        x += subBtnW + pad;
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_DESELECT), NULL, x, btnY, subBtnW, actBtnH, SWP_NOZORDER);
        x += subBtnW + pad;
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_OPEN_RESIZED), NULL, x, btnY, 72, actBtnH, SWP_NOZORDER);
        x += 72 + pad;
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_OPEN_ORIGINAL), NULL, x, btnY, 65, actBtnH, SWP_NOZORDER);
        x += 65 + pad;

        int closeW = 50;
        int fsBtnW = 85;
        int delW = (std::max)(120, margin + leftW - x - pad - fsBtnW - pad - closeW);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_DELETE), NULL, margin + leftW - closeW - pad - fsBtnW - pad - delW, btnY, delW, actBtnH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_FULLSCREEN), NULL, margin + leftW - closeW - pad - fsBtnW, btnY, fsBtnW, actBtnH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDCANCEL), NULL, margin + leftW - closeW, btnY, closeW, actBtnH, SWP_NOZORDER);

        if (IsZoomed(hDlg)) {
            SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_BTN_FULLSCREEN), L"🗗 창 복원");
        } else {
            SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_BTN_FULLSCREEN), L"🗖 전체화면");
        }

        // Right Pane: Previews
        int grpPreviewH = h - (margin * 2);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_GRP), NULL, previewX, margin, previewW, grpPreviewH, SWP_NOZORDER);

        int pBoxX = previewX + 10;
        int pBoxW = previewW - 20;
        int pAvailH = grpPreviewH - 24 - 10 - 40;
        int pBoxH = (std::max)(50, pAvailH / 2);

        int curPY = margin + 22;
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_RESIZED), NULL, pBoxX, curPY, pBoxW, pBoxH, SWP_NOZORDER);
        curPY += pBoxH + 4;
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_LBL_RESIZED_INFO), NULL, pBoxX, curPY, pBoxW, 16, SWP_NOZORDER);
        curPY += 20;

        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_ORIG), NULL, pBoxX, curPY, pBoxW, pBoxH, SWP_NOZORDER);
        curPY += pBoxH + 4;
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_LBL_ORIG_INFO), NULL, pBoxX, curPY, pBoxW, 16, SWP_NOZORDER);

        InvalidateRect(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_RESIZED), NULL, FALSE);
        InvalidateRect(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_ORIG), NULL, FALSE);
        break;
    }

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        if (pDIS->CtlID == IDC_RESIZED_PREVIEW_RESIZED) {
            g_resizedPreviewResized.Draw(pDIS->hDC, pDIS->rcItem, L"축소본 미리보기");
            return (INT_PTR)TRUE;
        } else if (pDIS->CtlID == IDC_RESIZED_PREVIEW_ORIG) {
            g_resizedPreviewOrig.Draw(pDIS->hDC, pDIS->rcItem, L"원본 미리보기");
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_NOTIFY:
    {
        NMHDR* pnm = (NMHDR*)lParam;
        if (pnm->idFrom == IDC_RESIZED_LIST) {
            if (pnm->code == LVN_ITEMCHANGED) {
                if (g_bResizedUpdating) break;

                NMLISTVIEW* pnmlv = (NMLISTVIEW*)lParam;
                if ((pnmlv->uChanged & LVIF_STATE) &&
                    ((pnmlv->uOldState & LVIS_STATEIMAGEMASK) != (pnmlv->uNewState & LVIS_STATEIMAGEMASK))) {
                    HWND hList = GetDlgItem(hDlg, IDC_RESIZED_LIST);
                    int itemIdx = pnmlv->iItem;
                    if (itemIdx >= 0) {
                        LVITEMW lvi = { 0 };
                        lvi.mask = LVIF_PARAM;
                        lvi.iItem = itemIdx;
                        if (ListView_GetItem(hList, &lvi)) {
                            int idx = (int)lvi.lParam;
                            if (idx >= 0 && (size_t)idx < g_resizedMatches.size()) {
                                g_resizedMatches[idx].isChecked = (ListView_GetCheckState(hList, itemIdx) != 0);
                            }
                        }
                    }
                    UpdateResizedSummary(hDlg);
                }
                if ((pnmlv->uNewState & LVIS_SELECTED) || (pnmlv->uOldState & LVIS_SELECTED)) {
                    HWND hList = GetDlgItem(hDlg, IDC_RESIZED_LIST);
                    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                    if (sel >= 0) {
                        LVITEMW lvi = { 0 };
                        lvi.mask = LVIF_PARAM;
                        lvi.iItem = sel;
                        if (ListView_GetItem(hList, &lvi)) {
                            int idx = (int)lvi.lParam;
                            if (idx >= 0 && (size_t)idx < g_resizedMatches.size()) {
                                const auto& item = g_resizedMatches[idx];
                                g_resizedPreviewResized.SetPath(item.resizedPath);
                                g_resizedPreviewOrig.SetPath(item.origPath);

                                wchar_t rInfo[256], oInfo[256];
                                swprintf_s(rInfo, L"축소본: %s (%s)", item.strResSize.c_str(), item.strResDim.c_str());
                                swprintf_s(oInfo, L"원본: %s (%s)", item.strOrigSize.c_str(), item.strOrigDim.c_str());
                                SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_LBL_RESIZED_INFO), rInfo);
                                SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_LBL_ORIG_INFO), oInfo);

                                InvalidateRect(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_RESIZED), NULL, FALSE);
                                InvalidateRect(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_ORIG), NULL, FALSE);
                            }
                        }
                    } else {
                        g_resizedPreviewResized.Clear();
                        g_resizedPreviewOrig.Clear();
                        SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_LBL_RESIZED_INFO), L"축소본: 선택 안 됨");
                        SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_LBL_ORIG_INFO), L"원본: 선택 안 됨");
                        InvalidateRect(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_RESIZED), NULL, FALSE);
                        InvalidateRect(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_ORIG), NULL, FALSE);
                    }
                }
            } else if (pnm->code == LVN_COLUMNCLICK) {
                NMLISTVIEW* pnmv = (NMLISTVIEW*)lParam;
                if (g_resizedSortCol == pnmv->iSubItem) {
                    g_resizedSortAsc = !g_resizedSortAsc;
                } else {
                    g_resizedSortCol = pnmv->iSubItem;
                    g_resizedSortAsc = true;
                }
                RefreshResizedListView(hDlg);
            } else if (pnm->code == NM_DBLCLK) {
                NMITEMACTIVATE* pnmia = (NMITEMACTIVATE*)lParam;
                if (pnmia->iItem >= 0) {
                    HWND hList = GetDlgItem(hDlg, IDC_RESIZED_LIST);
                    LVITEMW lvi = { 0 };
                    lvi.mask = LVIF_PARAM;
                    lvi.iItem = pnmia->iItem;
                    if (ListView_GetItem(hList, &lvi)) {
                        int idx = (int)lvi.lParam;
                        if (idx >= 0 && (size_t)idx < g_resizedMatches.size()) {
                            ShellExecuteW(hDlg, L"open", g_resizedMatches[idx].resizedPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                        }
                    }
                }
            }
        }
        break;
    }

    case WM_APP_RESIZED_PROGRESS:
    {
        int current = (int)wParam;
        int total = (int)lParam;
        if (total > 0) {
            int pct = (int)(((double)current / total) * 100.0);
            SendMessageW(GetDlgItem(hDlg, IDC_RESIZED_PROGRESS), PBM_SETPOS, pct, 0);

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - g_resizedScanStartTime).count();
            double speed = (elapsed > 0.05) ? (current / elapsed) : 0.0;

            wchar_t sbuf[256];
            swprintf_s(sbuf, L"이미지 분석 중: %d / %d (%d%%) | %.1f 파일/초 (스레드 %u개)",
                       current, total, pct, speed, g_resizedActiveThreads);
            SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_STATIC_STATUS), sbuf);
        }
        break;
    }

    case WM_APP_RESIZED_FINISHED:
    {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_BTN_SCAN), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_BTN_STOP), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_COMBO_THREADS), TRUE);
        SendMessageW(GetDlgItem(hDlg, IDC_RESIZED_PROGRESS), PBM_SETPOS, 100, 0);

        std::vector<ResizedPairItem>* pResults = (std::vector<ResizedPairItem>*)wParam;
        if (pResults) {
            g_resizedMatches = std::move(*pResults);
            delete pResults;
        }

        for (size_t i = 0; i < g_resizedMatches.size(); ++i) {
            auto& item = g_resizedMatches[i];
            item.id = (int)i;
            item.isChecked = true;
            item.resizedDir = std::filesystem::path(item.resizedPath).parent_path().wstring();
            item.origDir = std::filesystem::path(item.origPath).parent_path().wstring();
            item.strResDim = std::to_wstring(item.resizedW) + L"x" + std::to_wstring(item.resizedH);
            item.strResSize = FormatFileSize(item.resizedSize);
            item.strOrigDim = std::to_wstring(item.origW) + L"x" + std::to_wstring(item.origH);
            item.strOrigSize = FormatFileSize(item.origSize);
            wchar_t simBuf[32];
            swprintf_s(simBuf, L"%.1f%%", item.similarityPct);
            item.strSimilarity = simBuf;

            std::wstring lk = item.resizedName + L" " + item.origName + L" " + item.resizedPath + L" " + item.origPath;
            std::transform(lk.begin(), lk.end(), lk.begin(), ::towlower);
            item.lowerFilterKey = std::move(lk);
        }

        RefreshResizedListView(hDlg);

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - g_resizedScanStartTime).count();
        double speed = (elapsed > 0.05 && g_resizedTotalScannedImages > 0) ? (g_resizedTotalScannedImages / elapsed) : 0.0;

        wchar_t statusBuf[256];
        if (elapsed > 0.05 && g_resizedTotalScannedImages > 0) {
            swprintf_s(statusBuf, L"분석 완료: %d쌍 축소본 발견 (총 %zu장, %.1f초, 평균 %.1f 파일/초, 스레드 %u개)",
                       (int)g_resizedMatches.size(), g_resizedTotalScannedImages, elapsed, speed, g_resizedActiveThreads);
        } else {
            swprintf_s(statusBuf, L"탐색 완료: %d쌍의 축소본이 감지되었습니다.", (int)g_resizedMatches.size());
        }
        SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_STATIC_STATUS), statusBuf);
        break;
    }

    case WM_APP_TRIGGER_SEARCH:
    {
        if (g_resizedMatches.empty() && !g_bResizedScanning) {
            SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_RESIZED_BTN_SCAN, BN_CLICKED), (LPARAM)GetDlgItem(hDlg, IDC_RESIZED_BTN_SCAN));
        } else {
            RefreshResizedListView(hDlg);
        }
        return (INT_PTR)TRUE;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_RESIZED_BTN_FULLSCREEN) {
            if (IsZoomed(hDlg)) {
                ShowWindow(hDlg, SW_RESTORE);
            } else {
                ShowWindow(hDlg, SW_MAXIMIZE);
            }
        }
        else if (id == IDC_RESIZED_BTN_SEARCH) {
            if (g_resizedMatches.empty() && !g_bResizedScanning) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_RESIZED_BTN_SCAN, BN_CLICKED), (LPARAM)GetDlgItem(hDlg, IDC_RESIZED_BTN_SCAN));
            } else {
                RefreshResizedListView(hDlg);
            }
        }
        else if (id == IDOK) {
            HWND hFocus = GetFocus();
            if (hFocus == GetDlgItem(hDlg, IDC_RESIZED_LIST)) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_RESIZED_BTN_OPEN_RESIZED, BN_CLICKED), 0);
                return (INT_PTR)TRUE;
            }
            if (g_resizedMatches.empty() && !g_bResizedScanning) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_RESIZED_BTN_SCAN, BN_CLICKED), (LPARAM)GetDlgItem(hDlg, IDC_RESIZED_BTN_SCAN));
            } else {
                RefreshResizedListView(hDlg);
            }
            return (INT_PTR)TRUE;
        }
        else if (code == EN_KILLFOCUS) {
            if (id == IDC_RESIZED_EDIT_FILTER || id == IDC_RESIZED_EDIT_EXCLUDE) {
                if (!g_resizedMatches.empty()) {
                    RefreshResizedListView(hDlg);
                }
            }
        }
        else if (id == IDC_RESIZED_BTN_BROWSE) {
            std::wstring folderPath;
            if (BrowseFolder(hDlg, folderPath)) {
                SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_EDIT_FOLDER), folderPath.c_str());
            }
        }
        else if (id == IDC_RESIZED_BTN_SCAN) {
            wchar_t folderPath[MAX_PATH];
            GetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_EDIT_FOLDER), folderPath, MAX_PATH);
            if (folderPath[0] == L'\0' || !PathIsDirectoryW(folderPath)) {
                MessageBoxW(hDlg, L"탐색할 대상 폴더를 먼저 선택해주세요.", L"경고", MB_ICONWARNING);
                break;
            }

            if (g_resizedScanThread.joinable()) {
                g_bResizedCancel = true;
                g_resizedScanThread.join();
            }

            HWND hList = GetDlgItem(hDlg, IDC_RESIZED_LIST);
            ListView_DeleteAllItems(hList);
            g_resizedMatches.clear();
            g_resizedPreviewResized.Clear();
            g_resizedPreviewOrig.Clear();
            SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_LBL_RESIZED_INFO), L"축소본: 선택 안 됨");
            SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_LBL_ORIG_INFO), L"원본: 선택 안 됨");
            InvalidateRect(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_RESIZED), NULL, FALSE);
            InvalidateRect(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_ORIG), NULL, FALSE);
            UpdateResizedSummary(hDlg);

            EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_BTN_SCAN), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_BTN_STOP), TRUE);
            EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_BTN_DELETE), FALSE);

            HWND hCombo = GetDlgItem(hDlg, IDC_RESIZED_COMBO_THREADS);
            int curSel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
            unsigned int chosenThreads = (curSel >= 0) ? (unsigned int)SendMessageW(hCombo, CB_GETITEMDATA, curSel, 0) : 0;
            if (chosenThreads == 0 || chosenThreads == (unsigned int)CB_ERR) {
                chosenThreads = std::thread::hardware_concurrency();
                if (chosenThreads == 0) chosenThreads = 4;
            }
            g_resizedActiveThreads = chosenThreads;
            EnableWindow(hCombo, FALSE);

            SetCursor(LoadCursor(NULL, IDC_WAIT));
            SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_STATIC_STATUS), L"폴더 내 이미지 파일 탐색 중...");
            SendMessageW(GetDlgItem(hDlg, IDC_RESIZED_PROGRESS), PBM_SETPOS, 0, 0);

            g_bResizedCancel = false;
            g_bResizedScanning = true;

            std::wstring scanDir = folderPath;
            wchar_t exBuf[512] = { 0 };
            GetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_EDIT_EXCLUDE), exBuf, 512);
            std::vector<std::wstring> excludeTokens = ParseExcludeTokens(exBuf);

            g_resizedScanThread = std::thread([hDlg, scanDir, excludeTokens, chosenThreads]() {
                CoInitializeEx(NULL, COINIT_MULTITHREADED);

                struct ScanImageItem {
                    std::wstring path;
                    std::wstring name;
                    uint64_t fileSize = 0;
                    int w = 0;
                    int h = 0;
                    uint64_t phash = 0;
                    bool valid = false;
                };

                std::vector<ScanImageItem> imageItems;
                static const std::vector<std::wstring> imgExts = {
                    L".jpg", L".jpeg", L".png", L".bmp", L".webp", L".tiff", L".tif", L".heic"
                };

                try {
                    std::error_code ec;
                    for (auto it = std::filesystem::recursive_directory_iterator(
                        scanDir, std::filesystem::directory_options::skip_permission_denied, ec);
                        !ec && it != std::filesystem::recursive_directory_iterator();
                        it.increment(ec)) {
                        if (g_bResizedCancel) break;

                        if (!excludeTokens.empty()) {
                            std::wstring curPath = it->path().wstring();
                            std::transform(curPath.begin(), curPath.end(), curPath.begin(), ::towlower);
                            for (auto& ch : curPath) { if (ch == L'/') ch = L'\\'; }

                            bool isExcluded = false;
                            for (const auto& tok : excludeTokens) {
                                if (curPath.find(tok) != std::wstring::npos) {
                                    isExcluded = true;
                                    break;
                                }
                            }

                            if (isExcluded) {
                                if (it->is_directory()) {
                                    it.disable_recursion_pending();
                                }
                                continue;
                            }
                        }

                        if (it->is_regular_file()) {
                            std::wstring ext = it->path().extension().wstring();
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                            for (const auto& ve : imgExts) {
                                if (ext == ve) {
                                    ScanImageItem item;
                                    item.path = it->path().wstring();
                                    item.name = it->path().filename().wstring();
                                    item.fileSize = it->file_size();
                                    imageItems.push_back(std::move(item));
                                    break;
                                }
                            }
                        }
                    }
                } catch (...) {}

                const size_t total = imageItems.size();
                if (total == 0 || g_bResizedCancel) {
                    CoUninitialize();
                    PostMessageW(hDlg, WM_APP_RESIZED_FINISHED, (WPARAM)new std::vector<ResizedPairItem>(), 0);
                    return;
                }

                unsigned int numThreads = chosenThreads;
                if (numThreads == 0) numThreads = 4;
                g_resizedActiveThreads = numThreads;
                g_resizedTotalScannedImages = total;
                g_resizedScanStartTime = std::chrono::steady_clock::now();

                std::atomic<size_t> currentIndex{ 0 };
                std::atomic<size_t> processedCount{ 0 };
                std::vector<std::thread> workers;

                for (unsigned int t = 0; t < numThreads; ++t) {
                    workers.emplace_back([&]() {
                        CoInitializeEx(NULL, COINIT_MULTITHREADED);
                        while (!g_bResizedCancel) {
                            size_t idx = currentIndex.fetch_add(1);
                            if (idx >= total) break;

                            auto& item = imageItems[idx];
                            uint64_t hash = 0;
                            int w = 0, h = 0;
                            if (PerceptualHash::ComputePHash(item.path, hash, w, h)) {
                                item.phash = hash;
                                item.w = w;
                                item.h = h;
                                item.valid = true;
                            }

                            size_t done = processedCount.fetch_add(1) + 1;
                            if (done % 10 == 0 || done == total) {
                                PostMessageW(hDlg, WM_APP_RESIZED_PROGRESS, (WPARAM)done, (LPARAM)total);
                            }
                        }
                        CoUninitialize();
                    });
                }

                for (auto& w : workers) {
                    if (w.joinable()) w.join();
                }

                if (g_bResizedCancel) {
                    CoUninitialize();
                    PostMessageW(hDlg, WM_APP_RESIZED_FINISHED, (WPARAM)new std::vector<ResizedPairItem>(), 0);
                    return;
                }

                std::vector<ScanImageItem> validImages;
                validImages.reserve(imageItems.size());
                for (auto& item : imageItems) {
                    if (item.valid && item.w > 0 && item.h > 0) {
                        validImages.push_back(std::move(item));
                    }
                }

                std::unordered_map<std::wstring, ResizedPairItem> bestMatches;
                const size_t vTotal = validImages.size();

                for (size_t i = 0; i < vTotal && !g_bResizedCancel; ++i) {
                    for (size_t j = i + 1; j < vTotal && !g_bResizedCancel; ++j) {
                        int dist = PerceptualHash::GetHammingDistance(validImages[i].phash, validImages[j].phash);
                        if (dist <= 6) {
                            uint64_t pixA = (uint64_t)validImages[i].w * validImages[i].h;
                            uint64_t pixB = (uint64_t)validImages[j].w * validImages[j].h;

                            int resIdx = -1;
                            int origIdx = -1;

                            if (pixA < pixB) {
                                resIdx = (int)i;
                                origIdx = (int)j;
                            } else if (pixB < pixA) {
                                resIdx = (int)j;
                                origIdx = (int)i;
                            } else if (validImages[i].fileSize < validImages[j].fileSize) {
                                resIdx = (int)i;
                                origIdx = (int)j;
                            } else if (validImages[j].fileSize < validImages[i].fileSize) {
                                resIdx = (int)j;
                                origIdx = (int)i;
                            }

                            if (resIdx >= 0) {
                                const auto& rItem = validImages[resIdx];
                                const auto& oItem = validImages[origIdx];

                                uint64_t pixRes = (uint64_t)rItem.w * rItem.h;
                                uint64_t pixOrig = (uint64_t)oItem.w * oItem.h;

                                // 축소이미지와 원본이미지의 크기 차이가 2배 이상 나지 않으면 축소이미지가 아니므로 제외
                                bool is2xDiff = (pixOrig >= 2ULL * pixRes) || (oItem.fileSize >= 2ULL * rItem.fileSize);
                                if (!is2xDiff) {
                                    continue;
                                }

                                int denomA = (rItem.h > 0) ? rItem.h : 1;
                                int denomB = (oItem.h > 0) ? oItem.h : 1;
                                double ratioA = (double)rItem.w / denomA;
                                double ratioB = (double)oItem.w / denomB;

                                if (std::abs(ratioA - ratioB) < 0.15) {
                                    double simPct = ((64.0 - dist) / 64.0) * 100.0;
                                    auto it = bestMatches.find(rItem.path);
                                    if (it == bestMatches.end() ||
                                        ((uint64_t)oItem.w * oItem.h > (uint64_t)it->second.origW * it->second.origH)) {
                                        ResizedPairItem pair;
                                        pair.id = (int)bestMatches.size();
                                        pair.resizedPath = rItem.path;
                                        pair.resizedName = rItem.name;
                                        pair.resizedW = rItem.w;
                                        pair.resizedH = rItem.h;
                                        pair.resizedSize = rItem.fileSize;

                                        pair.origPath = oItem.path;
                                        pair.origName = oItem.name;
                                        pair.origW = oItem.w;
                                        pair.origH = oItem.h;
                                        pair.origSize = oItem.fileSize;

                                        pair.hammingDist = dist;
                                        pair.similarityPct = simPct;
                                        pair.isChecked = true;

                                        bestMatches[rItem.path] = pair;
                                    }
                                }
                            }
                        }
                    }
                }

                std::vector<ResizedPairItem>* pFinalList = new std::vector<ResizedPairItem>();
                pFinalList->reserve(bestMatches.size());
                int finalId = 0;
                for (auto& kv : bestMatches) {
                    kv.second.id = finalId++;
                    pFinalList->push_back(std::move(kv.second));
                }

                CoUninitialize();
                PostMessageW(hDlg, WM_APP_RESIZED_FINISHED, (WPARAM)pFinalList, 0);
            });
        }
        else if (id == IDC_RESIZED_BTN_STOP) {
            g_bResizedCancel = true;
            SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_STATIC_STATUS), L"중지 요청 중...");
            EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_BTN_STOP), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_COMBO_THREADS), TRUE);
        }
        else if (id == IDC_RESIZED_BTN_SELECT_ALL) {
            for (int idx : g_resizedVisibleIndices) {
                g_resizedMatches[idx].isChecked = true;
            }
            HWND hList = GetDlgItem(hDlg, IDC_RESIZED_LIST);
            g_bResizedUpdating = true;
            int count = ListView_GetItemCount(hList);
            for (int i = 0; i < count; ++i) {
                ListView_SetCheckState(hList, i, TRUE);
            }
            g_bResizedUpdating = false;
            UpdateResizedSummary(hDlg);
        }
        else if (id == IDC_RESIZED_BTN_DESELECT) {
            for (int idx : g_resizedVisibleIndices) {
                g_resizedMatches[idx].isChecked = false;
            }
            HWND hList = GetDlgItem(hDlg, IDC_RESIZED_LIST);
            g_bResizedUpdating = true;
            int count = ListView_GetItemCount(hList);
            for (int i = 0; i < count; ++i) {
                ListView_SetCheckState(hList, i, FALSE);
            }
            g_bResizedUpdating = false;
            UpdateResizedSummary(hDlg);
        }
        else if (id == IDC_RESIZED_BTN_OPEN_RESIZED) {
            HWND hList = GetDlgItem(hDlg, IDC_RESIZED_LIST);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel >= 0) {
                LVITEMW lvi = { 0 };
                lvi.mask = LVIF_PARAM;
                lvi.iItem = sel;
                if (ListView_GetItem(hList, &lvi)) {
                    int idx = (int)lvi.lParam;
                    if (idx >= 0 && (size_t)idx < g_resizedMatches.size()) {
                        ShellExecuteW(hDlg, L"open", g_resizedMatches[idx].resizedPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    }
                }
            } else {
                MessageBoxW(hDlg, L"열어볼 항목을 목록에서 먼저 선택해주세요.", L"안내", MB_ICONINFORMATION);
            }
        }
        else if (id == IDC_RESIZED_BTN_OPEN_ORIGINAL) {
            HWND hList = GetDlgItem(hDlg, IDC_RESIZED_LIST);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel >= 0) {
                LVITEMW lvi = { 0 };
                lvi.mask = LVIF_PARAM;
                lvi.iItem = sel;
                if (ListView_GetItem(hList, &lvi)) {
                    int idx = (int)lvi.lParam;
                    if (idx >= 0 && (size_t)idx < g_resizedMatches.size()) {
                        ShellExecuteW(hDlg, L"open", g_resizedMatches[idx].origPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    }
                }
            } else {
                MessageBoxW(hDlg, L"열어볼 항목을 목록에서 먼저 선택해주세요.", L"안내", MB_ICONINFORMATION);
            }
        }
        else if (id == IDC_RESIZED_BTN_DELETE) {
            std::vector<int> checkedIndices;
            uint64_t totalBytes = 0;
            for (int idx : g_resizedVisibleIndices) {
                if (g_resizedMatches[idx].isChecked) {
                    checkedIndices.push_back(idx);
                    totalBytes += g_resizedMatches[idx].resizedSize;
                }
            }

            if (checkedIndices.empty()) {
                MessageBoxW(hDlg, L"삭제할 축소본 파일을 먼저 체크박스에서 선택해주세요.", L"안내", MB_ICONINFORMATION);
                break;
            }

            wchar_t confirmMsg[300];
            swprintf_s(confirmMsg,
                L"선택한 %d개의 축소본 파일을 휴지통으로 안전하게 이동하시겠습니까?\n\n"
                L"• 확보될 디스크 공간: %s\n"
                L"• 안전 보장: 삭제된 파일은 Windows 휴지통에서 복원할 수 있습니다.",
                (int)checkedIndices.size(), FormatFileSize(totalBytes).c_str());

            if (MessageBoxW(hDlg, confirmMsg, L"축소본 안전 삭제 확인", MB_ICONQUESTION | MB_YESNO) == IDYES) {
                std::vector<wchar_t> doubleNullBuf;
                for (int idx : checkedIndices) {
                    const auto& p = g_resizedMatches[idx].resizedPath;
                    for (wchar_t ch : p) doubleNullBuf.push_back(ch);
                    doubleNullBuf.push_back(L'\0');
                }
                doubleNullBuf.push_back(L'\0');

                SHFILEOPSTRUCTW op = { 0 };
                op.hwnd = hDlg;
                op.wFunc = FO_DELETE;
                op.pFrom = doubleNullBuf.data();
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;

                int opRes = SHFileOperationW(&op);
                if (opRes == 0 && !op.fAnyOperationsAborted) {
                    std::unordered_set<int> deletedSet(checkedIndices.begin(), checkedIndices.end());
                    std::vector<ResizedPairItem> remainingMatches;
                    for (size_t i = 0; i < g_resizedMatches.size(); ++i) {
                        if (deletedSet.find((int)i) == deletedSet.end()) {
                            remainingMatches.push_back(std::move(g_resizedMatches[i]));
                        }
                    }
                    g_resizedMatches = std::move(remainingMatches);
                    for (size_t i = 0; i < g_resizedMatches.size(); ++i) {
                        g_resizedMatches[i].id = (int)i;
                    }

                    g_resizedPreviewResized.Clear();
                    g_resizedPreviewOrig.Clear();
                    SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_LBL_RESIZED_INFO), L"축소본: 선택 안 됨");
                    SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_LBL_ORIG_INFO), L"원본: 선택 안 됨");
                    InvalidateRect(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_RESIZED), NULL, FALSE);
                    InvalidateRect(GetDlgItem(hDlg, IDC_RESIZED_PREVIEW_ORIG), NULL, FALSE);

                    RefreshResizedListView(hDlg);

                    wchar_t doneMsg[128];
                    swprintf_s(doneMsg, L"총 %d개의 축소본 파일이 휴지통으로 이동되었습니다.", (int)checkedIndices.size());
                    MessageBoxW(hDlg, doneMsg, L"삭제 완료", MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hDlg, L"일부 또는 전체 파일 삭제 중 오류가 발생했거나 취소되었습니다.", L"알림", MB_ICONWARNING);
                }
            }
        }
        else if (id == IDCANCEL) {
            if (g_bResizedScanning) {
                g_bResizedCancel = true;
                if (g_resizedScanThread.joinable()) {
                    g_resizedScanThread.join();
                }
            }
            EndDialog(hDlg, IDCANCEL);
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_CLOSE:
        if (g_bResizedScanning) {
            g_bResizedCancel = true;
            if (g_resizedScanThread.joinable()) {
                g_resizedScanThread.join();
            }
        }
        EndDialog(hDlg, IDCANCEL);
        return (INT_PTR)TRUE;
    }

    return (INT_PTR)FALSE;
}
