#include "dup_cleaner_dialog.h"
#include "dedup_engine.h"
#include "common/utils.h"
#include "common/preview_renderer.h"
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

#pragma comment(lib, "imm32.lib")

extern HINSTANCE g_hInst;
extern HWND g_hEditInput;

// -------------------------------------------------------------
// Duplicate File Cleaner State
// -------------------------------------------------------------
static std::vector<DupPairItem> g_dupMatches;
static std::atomic<bool> g_bDupScanning{ false };
static std::atomic<bool> g_bDupCancel{ false };
static std::thread g_dupScanThread;
static int g_dupSortCol = 0;
static bool g_dupSortAsc = true;
static bool g_bDupUpdating = false;
static ImagePreviewRenderer g_dupPreviewDup;
static ImagePreviewRenderer g_dupPreviewOrig;

static std::vector<int> g_dupVisibleIndices;

static std::chrono::steady_clock::time_point g_dupScanStartTime;
static unsigned int g_dupActiveThreads = 0;
static size_t g_dupCandidateTotal = 0;

static void UpdateDupSummary(HWND hDlg)
{
    int totalVisible = (int)g_dupVisibleIndices.size();
    int totalAll = (int)g_dupMatches.size();
    int checkedCount = 0;
    uint64_t savedBytes = 0;

    for (int idx : g_dupVisibleIndices) {
        if (g_dupMatches[idx].isChecked) {
            checkedCount++;
            savedBytes += g_dupMatches[idx].fileSize;
        }
    }

    wchar_t buf[256];
    if (totalVisible == totalAll) {
        swprintf_s(buf, L"발견된 중복 파일: %d개 | 선택됨: %d개 (절약 가능 공간: %s)",
            totalAll, checkedCount, FormatFileSize(savedBytes).c_str());
    } else {
        swprintf_s(buf, L"표시된 중복 파일: %d개 (전체 %d개, 제외 %d개) | 선택됨: %d개 (절약 공간: %s)",
            totalVisible, totalAll, totalAll - totalVisible, checkedCount, FormatFileSize(savedBytes).c_str());
    }
    SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_STATIC_SUMMARY), buf);

    EnableWindow(GetDlgItem(hDlg, IDC_DUP_BTN_DELETE), checkedCount > 0);
}

static void RefreshDupListView(HWND hDlg)
{
    // Force-commit any active IME composition before reading edit texts
    HWND hExEdit = GetDlgItem(hDlg, IDC_DUP_EDIT_EXCLUDE);
    if (hExEdit) {
        HIMC hImc = ImmGetContext(hExEdit);
        if (hImc) {
            ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
            ImmReleaseContext(hExEdit, hImc);
        }
    }
    HWND hFilterEdit = GetDlgItem(hDlg, IDC_DUP_EDIT_FILTER);
    if (hFilterEdit) {
        HIMC hImc = ImmGetContext(hFilterEdit);
        if (hImc) {
            ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
            ImmReleaseContext(hFilterEdit, hImc);
        }
    }

    wchar_t filterBuf[256] = { 0 };
    GetWindowTextW(GetDlgItem(hDlg, IDC_DUP_EDIT_FILTER), filterBuf, 256);
    std::wstring filter = filterBuf;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::towlower);

    wchar_t exBuf[512] = { 0 };
    GetWindowTextW(GetDlgItem(hDlg, IDC_DUP_EDIT_EXCLUDE), exBuf, 512);
    std::vector<std::wstring> excludeTokens = ParseExcludeTokens(exBuf);

    std::vector<int> visibleIndices;
    visibleIndices.reserve(g_dupMatches.size());
    for (size_t i = 0; i < g_dupMatches.size(); ++i) {
        const auto& item = g_dupMatches[i];
        if (!filter.empty() && item.lowerFilterKey.find(filter) == std::wstring::npos) {
            continue;
        }

        if (!excludeTokens.empty()) {
            std::wstring lowerDupPath = item.dupPath;
            std::transform(lowerDupPath.begin(), lowerDupPath.end(), lowerDupPath.begin(), ::towlower);
            for (auto& ch : lowerDupPath) { if (ch == L'/') ch = L'\\'; }

            std::wstring lowerOrigPath = item.origPath;
            std::transform(lowerOrigPath.begin(), lowerOrigPath.end(), lowerOrigPath.begin(), ::towlower);
            for (auto& ch : lowerOrigPath) { if (ch == L'/') ch = L'\\'; }

            bool isExcluded = false;
            for (const auto& tok : excludeTokens) {
                if (lowerDupPath.find(tok) != std::wstring::npos || lowerOrigPath.find(tok) != std::wstring::npos) {
                    isExcluded = true;
                    break;
                }
            }
            if (isExcluded) continue;
        }

        visibleIndices.push_back((int)i);
    }
    g_dupVisibleIndices = visibleIndices;

    std::sort(visibleIndices.begin(), visibleIndices.end(), [](int a, int b) {
        const auto& itemA = g_dupMatches[a];
        const auto& itemB = g_dupMatches[b];
        int cmp = 0;
        switch (g_dupSortCol) {
        case 0: cmp = _wcsicmp(itemA.dupName.c_str(), itemB.dupName.c_str()); break;
        case 1:
            if (itemA.fileSize < itemB.fileSize) cmp = -1;
            else if (itemA.fileSize > itemB.fileSize) cmp = 1;
            break;
        case 2: cmp = _wcsicmp(itemA.origName.c_str(), itemB.origName.c_str()); break;
        case 3: cmp = _wcsicmp(itemA.dupDir.c_str(), itemB.dupDir.c_str()); break;
        case 4: cmp = _wcsicmp(itemA.origDir.c_str(), itemB.origDir.c_str()); break;
        default: break;
        }
        return g_dupSortAsc ? (cmp < 0) : (cmp > 0);
    });

    HWND hList = GetDlgItem(hDlg, IDC_DUP_LIST);
    g_bDupUpdating = true;
    SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hList);

    for (size_t row = 0; row < visibleIndices.size(); ++row) {
        int idx = visibleIndices[row];
        const auto& item = g_dupMatches[idx];

        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = (int)row;
        lvi.iSubItem = 0;
        lvi.pszText = (LPWSTR)item.dupName.c_str();
        lvi.lParam = (LPARAM)idx;
        ListView_InsertItem(hList, &lvi);

        ListView_SetItemText(hList, (int)row, 1, (LPWSTR)item.strFileSize.c_str());
        ListView_SetItemText(hList, (int)row, 2, (LPWSTR)item.origName.c_str());
        ListView_SetItemText(hList, (int)row, 3, (LPWSTR)item.dupDir.c_str());
        ListView_SetItemText(hList, (int)row, 4, (LPWSTR)item.origDir.c_str());

        ListView_SetCheckState(hList, (int)row, item.isChecked ? TRUE : FALSE);
    }

    SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, TRUE);
    g_bDupUpdating = false;

    UpdateDupSummary(hDlg);
}

INT_PTR CALLBACK DupCleanerDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
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
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_EDIT_FOLDER), inPath);
        }

        HWND hList = GetDlgItem(hDlg, IDC_DUP_LIST);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        lvc.iSubItem = 0; lvc.pszText = (LPWSTR)L"중복 파일명"; lvc.cx = 160;
        ListView_InsertColumn(hList, 0, &lvc);

        lvc.iSubItem = 1; lvc.pszText = (LPWSTR)L"파일 크기"; lvc.cx = 95;
        ListView_InsertColumn(hList, 1, &lvc);

        lvc.iSubItem = 2; lvc.pszText = (LPWSTR)L"기준 원본 파일명"; lvc.cx = 160;
        ListView_InsertColumn(hList, 2, &lvc);

        lvc.iSubItem = 3; lvc.pszText = (LPWSTR)L"중복 파일 위치"; lvc.cx = 200;
        ListView_InsertColumn(hList, 3, &lvc);

        lvc.iSubItem = 4; lvc.pszText = (LPWSTR)L"원본 위치"; lvc.cx = 200;
        ListView_InsertColumn(hList, 4, &lvc);

        SetWindowSubclass(GetDlgItem(hDlg, IDC_DUP_EDIT_FILTER), FilterEditSubclassProc, 0, (DWORD_PTR)hDlg);
        SetWindowSubclass(GetDlgItem(hDlg, IDC_DUP_EDIT_EXCLUDE), FilterEditSubclassProc, 1, (DWORD_PTR)hDlg);
        SetWindowSubclass(GetDlgItem(hDlg, IDC_DUP_EDIT_FOLDER), FilterEditSubclassProc, 2, (DWORD_PTR)hDlg);

        SendMessageW(GetDlgItem(hDlg, IDC_DUP_EDIT_FILTER), EM_SETCUEBANNER, TRUE, (LPARAM)L"파일명/경로 검색어 입력 후 Enter");
        SendMessageW(GetDlgItem(hDlg, IDC_DUP_EDIT_EXCLUDE), EM_SETCUEBANNER, TRUE, (LPARAM)L"제외할 폴더명 입력 (예: temp, backup) - 콤마(,)로 여러 개 구분");

        g_dupPreviewDup.Clear();
        g_dupPreviewOrig.Clear();
        SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_LBL_DUP_INFO), L"중복본: 선택 안 됨");
        SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_LBL_ORIG_INFO), L"기준 원본: 선택 안 됨");

        // Initialize Thread Selection Combobox
        HWND hCombo = GetDlgItem(hDlg, IDC_DUP_COMBO_THREADS);
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

        UpdateDupSummary(hDlg);
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

        int previewW = (std::max)(220, (std::min)(480, (w * 34) / 100));
        int leftW = w - previewW - (margin * 2) - pad;
        if (leftW < 300) leftW = 300;
        int previewX = margin + leftW + pad;

        // Left Pane: Folder row
        int editW = leftW - 75 - (btnW * 2) - pad * 2;
        if (editW < 80) editW = 80;
        SetWindowPos(GetDlgItem(hDlg, IDC_STATIC), NULL, margin, 14, 70, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_EDIT_FOLDER), NULL, margin + 72, 12, editW, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_BROWSE), NULL, margin + 72 + editW + pad, 11, btnW, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_SCAN), NULL, margin + 72 + editW + pad + btnW + pad, 11, btnW, rowH, SWP_NOZORDER);

        // Status row & Progress (Status Text, Thread Label, Thread Combo, Stop Button)
        int comboW = 82;
        int lblThW = 42;
        int stopX = margin + leftW - btnW;
        int comboX = stopX - pad - comboW;
        int lblThX = comboX - pad - lblThW;
        int statusW = lblThX - margin - pad;
        if (statusW < 100) statusW = 100;

        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_STATIC_STATUS), NULL, margin, 40, statusW, 18, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_LBL_THREADS), NULL, lblThX, 40, lblThW, 18, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_COMBO_THREADS), NULL, comboX, 37, comboW, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_STOP), NULL, stopX, 37, btnW, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_PROGRESS), NULL, margin, 62, leftW, 14, SWP_NOZORDER);

        // Filter search row
        int searchBtnW = 46;
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_LBL_FILTER), NULL, margin, 82, 70, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_EDIT_FILTER), NULL, margin + 72, 80, leftW - 72 - searchBtnW - pad, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_SEARCH), NULL, margin + leftW - searchBtnW, 80, searchBtnW, rowH, SWP_NOZORDER);

        // Exclude Folder row
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_LBL_EXCLUDE), NULL, margin, 108, 70, rowH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_EDIT_EXCLUDE), NULL, margin + 72, 106, leftW - 72, rowH, SWP_NOZORDER);

        // Listview
        int bottomReserve = 66;
        int listTop = 134;
        int listH = h - listTop - bottomReserve;
        if (listH > 50) {
            SetWindowPos(GetDlgItem(hDlg, IDC_DUP_LIST), NULL, margin, listTop, leftW, listH, SWP_NOZORDER);
        }

        // Summary label
        int summaryY = h - 56;
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_STATIC_SUMMARY), NULL, margin, summaryY, leftW, 18, SWP_NOZORDER);

        // Action Buttons row
        int btnY = h - 34;
        int actBtnH = 26;
        int x = margin;
        int subBtnW = 60;
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_SELECT_ALL), NULL, x, btnY, subBtnW, actBtnH, SWP_NOZORDER);
        x += subBtnW + pad;
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_DESELECT), NULL, x, btnY, subBtnW, actBtnH, SWP_NOZORDER);
        x += subBtnW + pad;
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_OPEN_DUP), NULL, x, btnY, 72, actBtnH, SWP_NOZORDER);
        x += 72 + pad;
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_OPEN_ORIG), NULL, x, btnY, 80, actBtnH, SWP_NOZORDER);
        x += 80 + pad;

        int closeW = 50;
        int fsBtnW = 85;
        int delW = (std::max)(120, margin + leftW - x - pad - fsBtnW - pad - closeW);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_DELETE), NULL, margin + leftW - closeW - pad - fsBtnW - pad - delW, btnY, delW, actBtnH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_FULLSCREEN), NULL, margin + leftW - closeW - pad - fsBtnW, btnY, fsBtnW, actBtnH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDCANCEL), NULL, margin + leftW - closeW, btnY, closeW, actBtnH, SWP_NOZORDER);

        if (IsZoomed(hDlg)) {
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_BTN_FULLSCREEN), L"🗗 창 복원");
        } else {
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_BTN_FULLSCREEN), L"🗖 전체화면");
        }

        // Right Pane: Previews
        int grpPreviewH = h - (margin * 2);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_PREVIEW_GRP), NULL, previewX, margin, previewW, grpPreviewH, SWP_NOZORDER);

        int pBoxX = previewX + 10;
        int pBoxW = previewW - 20;
        int pAvailH = grpPreviewH - 24 - 10 - 40;
        int pBoxH = (std::max)(50, pAvailH / 2);

        int curPY = margin + 22;
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_PREVIEW_DUP), NULL, pBoxX, curPY, pBoxW, pBoxH, SWP_NOZORDER);
        curPY += pBoxH + 4;
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_LBL_DUP_INFO), NULL, pBoxX, curPY, pBoxW, 16, SWP_NOZORDER);
        curPY += 20;

        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_PREVIEW_ORIG), NULL, pBoxX, curPY, pBoxW, pBoxH, SWP_NOZORDER);
        curPY += pBoxH + 4;
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_LBL_ORIG_INFO), NULL, pBoxX, curPY, pBoxW, 16, SWP_NOZORDER);

        InvalidateRect(GetDlgItem(hDlg, IDC_DUP_PREVIEW_DUP), NULL, FALSE);
        InvalidateRect(GetDlgItem(hDlg, IDC_DUP_PREVIEW_ORIG), NULL, FALSE);
        break;
    }

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        if (pDIS->CtlID == IDC_DUP_PREVIEW_DUP) {
            g_dupPreviewDup.Draw(pDIS->hDC, pDIS->rcItem, L"중복 파일 미리보기");
            return (INT_PTR)TRUE;
        } else if (pDIS->CtlID == IDC_DUP_PREVIEW_ORIG) {
            g_dupPreviewOrig.Draw(pDIS->hDC, pDIS->rcItem, L"기준 원본 파일 미리보기");
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_NOTIFY:
    {
        NMHDR* pnm = (NMHDR*)lParam;
        if (pnm->idFrom == IDC_DUP_LIST) {
            if (pnm->code == LVN_ITEMCHANGED) {
                if (g_bDupUpdating) break;

                NMLISTVIEW* pnmlv = (NMLISTVIEW*)lParam;
                if ((pnmlv->uChanged & LVIF_STATE) &&
                    ((pnmlv->uOldState & LVIS_STATEIMAGEMASK) != (pnmlv->uNewState & LVIS_STATEIMAGEMASK))) {
                    HWND hList = GetDlgItem(hDlg, IDC_DUP_LIST);
                    int itemIdx = pnmlv->iItem;
                    if (itemIdx >= 0) {
                        LVITEMW lvi = { 0 };
                        lvi.mask = LVIF_PARAM;
                        lvi.iItem = itemIdx;
                        if (ListView_GetItem(hList, &lvi)) {
                            int idx = (int)lvi.lParam;
                            if (idx >= 0 && (size_t)idx < g_dupMatches.size()) {
                                g_dupMatches[idx].isChecked = (ListView_GetCheckState(hList, itemIdx) != 0);
                            }
                        }
                    }
                    UpdateDupSummary(hDlg);
                }
                if ((pnmlv->uNewState & LVIS_SELECTED) || (pnmlv->uOldState & LVIS_SELECTED)) {
                    HWND hList = GetDlgItem(hDlg, IDC_DUP_LIST);
                    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                    if (sel >= 0) {
                        LVITEMW lvi = { 0 };
                        lvi.mask = LVIF_PARAM;
                        lvi.iItem = sel;
                        if (ListView_GetItem(hList, &lvi)) {
                            int idx = (int)lvi.lParam;
                            if (idx >= 0 && (size_t)idx < g_dupMatches.size()) {
                                const auto& item = g_dupMatches[idx];
                                g_dupPreviewDup.SetPath(item.dupPath);
                                g_dupPreviewOrig.SetPath(item.origPath);

                                wchar_t dInfo[256], oInfo[256];
                                swprintf_s(dInfo, L"중복본: %s", item.strFileSize.c_str());
                                swprintf_s(oInfo, L"기준 원본: %s", item.strFileSize.c_str());
                                SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_LBL_DUP_INFO), dInfo);
                                SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_LBL_ORIG_INFO), oInfo);

                                InvalidateRect(GetDlgItem(hDlg, IDC_DUP_PREVIEW_DUP), NULL, FALSE);
                                InvalidateRect(GetDlgItem(hDlg, IDC_DUP_PREVIEW_ORIG), NULL, FALSE);
                            }
                        }
                    } else {
                        g_dupPreviewDup.Clear();
                        g_dupPreviewOrig.Clear();
                        SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_LBL_DUP_INFO), L"중복본: 선택 안 됨");
                        SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_LBL_ORIG_INFO), L"기준 원본: 선택 안 됨");
                        InvalidateRect(GetDlgItem(hDlg, IDC_DUP_PREVIEW_DUP), NULL, FALSE);
                        InvalidateRect(GetDlgItem(hDlg, IDC_DUP_PREVIEW_ORIG), NULL, FALSE);
                    }
                }
            } else if (pnm->code == LVN_COLUMNCLICK) {
                NMLISTVIEW* pnmv = (NMLISTVIEW*)lParam;
                if (g_dupSortCol == pnmv->iSubItem) {
                    g_dupSortAsc = !g_dupSortAsc;
                } else {
                    g_dupSortCol = pnmv->iSubItem;
                    g_dupSortAsc = true;
                }
                RefreshDupListView(hDlg);
            } else if (pnm->code == NM_DBLCLK) {
                NMITEMACTIVATE* pnmia = (NMITEMACTIVATE*)lParam;
                if (pnmia->iItem >= 0) {
                    HWND hList = GetDlgItem(hDlg, IDC_DUP_LIST);
                    LVITEMW lvi = { 0 };
                    lvi.mask = LVIF_PARAM;
                    lvi.iItem = pnmia->iItem;
                    if (ListView_GetItem(hList, &lvi)) {
                        int idx = (int)lvi.lParam;
                        if (idx >= 0 && (size_t)idx < g_dupMatches.size()) {
                            ShellExecuteW(hDlg, L"open", g_dupMatches[idx].dupPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                        }
                    }
                }
            }
        }
        break;
    }

    case WM_APP_DUP_PROGRESS:
    {
        int current = (int)wParam;
        int total = (int)lParam;
        if (total > 0) {
            int pct = (int)(((double)current / total) * 100.0);
            SendMessageW(GetDlgItem(hDlg, IDC_DUP_PROGRESS), PBM_SETPOS, pct, 0);

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - g_dupScanStartTime).count();
            double speed = (elapsed > 0.05) ? (current / elapsed) : 0.0;

            wchar_t sbuf[256];
            swprintf_s(sbuf, L"해시 및 무결성 분석 중: %d / %d (%d%%) | %.1f 파일/초 (스레드 %u개)",
                       current, total, pct, speed, g_dupActiveThreads);
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_STATIC_STATUS), sbuf);
        }
        break;
    }

    case WM_APP_DUP_FINISHED:
    {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        EnableWindow(GetDlgItem(hDlg, IDC_DUP_BTN_SCAN), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_DUP_BTN_STOP), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_DUP_COMBO_THREADS), TRUE);
        SendMessageW(GetDlgItem(hDlg, IDC_DUP_PROGRESS), PBM_SETPOS, 100, 0);

        std::vector<DupPairItem>* pResults = (std::vector<DupPairItem>*)wParam;
        if (pResults) {
            g_dupMatches = std::move(*pResults);
            delete pResults;
        }

        for (size_t i = 0; i < g_dupMatches.size(); ++i) {
            auto& item = g_dupMatches[i];
            item.id = (int)i;
            item.isChecked = true;
            item.dupDir = std::filesystem::path(item.dupPath).parent_path().wstring();
            item.origDir = std::filesystem::path(item.origPath).parent_path().wstring();
            item.strFileSize = FormatFileSize(item.fileSize);

            std::wstring lk = item.dupName + L" " + item.origName + L" " + item.dupPath + L" " + item.origPath;
            std::transform(lk.begin(), lk.end(), lk.begin(), ::towlower);
            item.lowerFilterKey = std::move(lk);
        }

        RefreshDupListView(hDlg);

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - g_dupScanStartTime).count();
        double speed = (elapsed > 0.05 && g_dupCandidateTotal > 0) ? (g_dupCandidateTotal / elapsed) : 0.0;

        wchar_t statusBuf[256];
        if (elapsed > 0.05 && g_dupCandidateTotal > 0) {
            swprintf_s(statusBuf, L"분석 완료: %d개 중복 파일 발견 (후보 %zu개, %.1f초, 평균 %.1f 파일/초, 스레드 %u개)",
                       (int)g_dupMatches.size(), g_dupCandidateTotal, elapsed, speed, g_dupActiveThreads);
        } else {
            swprintf_s(statusBuf, L"탐색 완료: %d개의 중복 파일이 감지되었습니다.", (int)g_dupMatches.size());
        }
        SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_STATIC_STATUS), statusBuf);
        break;
    }

    case WM_APP_TRIGGER_SEARCH:
    {
        if (g_dupMatches.empty() && !g_bDupScanning) {
            SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_DUP_BTN_SCAN, BN_CLICKED), (LPARAM)GetDlgItem(hDlg, IDC_DUP_BTN_SCAN));
        } else {
            RefreshDupListView(hDlg);
        }
        return (INT_PTR)TRUE;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_DUP_BTN_FULLSCREEN) {
            if (IsZoomed(hDlg)) {
                ShowWindow(hDlg, SW_RESTORE);
            } else {
                ShowWindow(hDlg, SW_MAXIMIZE);
            }
        }
        else if (id == IDC_DUP_BTN_SEARCH) {
            if (g_dupMatches.empty() && !g_bDupScanning) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_DUP_BTN_SCAN, BN_CLICKED), (LPARAM)GetDlgItem(hDlg, IDC_DUP_BTN_SCAN));
            } else {
                RefreshDupListView(hDlg);
            }
        }
        else if (id == IDOK) {
            HWND hFocus = GetFocus();
            if (hFocus == GetDlgItem(hDlg, IDC_DUP_LIST)) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_DUP_BTN_OPEN_DUP, BN_CLICKED), 0);
                return (INT_PTR)TRUE;
            }
            if (g_dupMatches.empty() && !g_bDupScanning) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_DUP_BTN_SCAN, BN_CLICKED), (LPARAM)GetDlgItem(hDlg, IDC_DUP_BTN_SCAN));
            } else {
                RefreshDupListView(hDlg);
            }
            return (INT_PTR)TRUE;
        }
        else if (code == EN_KILLFOCUS) {
            if (id == IDC_DUP_EDIT_FILTER || id == IDC_DUP_EDIT_EXCLUDE) {
                if (!g_dupMatches.empty()) {
                    RefreshDupListView(hDlg);
                }
            }
        }
        else if (id == IDC_DUP_BTN_BROWSE) {
            std::wstring folderPath;
            if (BrowseFolder(hDlg, folderPath)) {
                SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_EDIT_FOLDER), folderPath.c_str());
            }
        }
        else if (id == IDC_DUP_BTN_SCAN) {
            wchar_t folderPath[MAX_PATH];
            GetWindowTextW(GetDlgItem(hDlg, IDC_DUP_EDIT_FOLDER), folderPath, MAX_PATH);
            if (folderPath[0] == L'\0' || !PathIsDirectoryW(folderPath)) {
                MessageBoxW(hDlg, L"탐색할 대상 폴더를 먼저 선택해주세요.", L"경고", MB_ICONWARNING);
                break;
            }

            if (g_dupScanThread.joinable()) {
                g_bDupCancel = true;
                g_dupScanThread.join();
            }

            HWND hList = GetDlgItem(hDlg, IDC_DUP_LIST);
            ListView_DeleteAllItems(hList);
            g_dupMatches.clear();
            g_dupPreviewDup.Clear();
            g_dupPreviewOrig.Clear();
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_LBL_DUP_INFO), L"중복본: 선택 안 됨");
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_LBL_ORIG_INFO), L"기준 원본: 선택 안 됨");
            InvalidateRect(GetDlgItem(hDlg, IDC_DUP_PREVIEW_DUP), NULL, FALSE);
            InvalidateRect(GetDlgItem(hDlg, IDC_DUP_PREVIEW_ORIG), NULL, FALSE);
            UpdateDupSummary(hDlg);

            EnableWindow(GetDlgItem(hDlg, IDC_DUP_BTN_SCAN), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_DUP_BTN_STOP), TRUE);
            EnableWindow(GetDlgItem(hDlg, IDC_DUP_BTN_DELETE), FALSE);

            HWND hCombo = GetDlgItem(hDlg, IDC_DUP_COMBO_THREADS);
            int curSel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
            unsigned int chosenThreads = (curSel >= 0) ? (unsigned int)SendMessageW(hCombo, CB_GETITEMDATA, curSel, 0) : 0;
            if (chosenThreads == 0 || chosenThreads == (unsigned int)CB_ERR) {
                chosenThreads = std::thread::hardware_concurrency();
                if (chosenThreads == 0) chosenThreads = 4;
            }
            g_dupActiveThreads = chosenThreads;
            EnableWindow(hCombo, FALSE);

            SetCursor(LoadCursor(NULL, IDC_WAIT));
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_STATIC_STATUS), L"폴더 내 파일 목록 검색 중...");
            SendMessageW(GetDlgItem(hDlg, IDC_DUP_PROGRESS), PBM_SETPOS, 0, 0);

            g_bDupCancel = false;
            g_bDupScanning = true;

            std::wstring scanDir = folderPath;
            wchar_t exBuf[512] = { 0 };
            GetWindowTextW(GetDlgItem(hDlg, IDC_DUP_EDIT_EXCLUDE), exBuf, 512);
            std::vector<std::wstring> excludeTokens = ParseExcludeTokens(exBuf);

            g_dupScanThread = std::thread([hDlg, scanDir, excludeTokens, chosenThreads]() {
                CoInitializeEx(NULL, COINIT_MULTITHREADED);

                struct FileScanEntry {
                    std::wstring path;
                    std::wstring name;
                    uint64_t fileSize = 0;
                };

                std::vector<FileScanEntry> allFiles;
                try {
                    std::error_code ec;
                    for (auto it = std::filesystem::recursive_directory_iterator(
                        scanDir, std::filesystem::directory_options::skip_permission_denied, ec);
                        !ec && it != std::filesystem::recursive_directory_iterator();
                        it.increment(ec)) {
                        if (g_bDupCancel) break;

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
                            FileScanEntry fe;
                            fe.path = it->path().wstring();
                            fe.name = it->path().filename().wstring();
                            fe.fileSize = it->file_size();
                            allFiles.push_back(std::move(fe));
                        }
                    }
                } catch (...) {}

                if (allFiles.empty() || g_bDupCancel) {
                    CoUninitialize();
                    PostMessageW(hDlg, WM_APP_DUP_FINISHED, (WPARAM)new std::vector<DupPairItem>(), 0);
                    return;
                }

                // Stage 1: Group by file size in RAM (Discard unique sizes with 0 disk reads)
                std::unordered_map<uint64_t, std::vector<FileScanEntry>> sizeMap;
                for (auto& fe : allFiles) {
                    if (fe.fileSize > 0) {
                        sizeMap[fe.fileSize].push_back(std::move(fe));
                    }
                }

                std::vector<std::vector<FileScanEntry>> candidateBuckets;
                size_t candidateCount = 0;
                for (auto& pair : sizeMap) {
                    if (pair.second.size() > 1) {
                        candidateCount += pair.second.size();
                        candidateBuckets.push_back(std::move(pair.second));
                    }
                }

                if (candidateBuckets.empty() || g_bDupCancel) {
                    CoUninitialize();
                    PostMessageW(hDlg, WM_APP_DUP_FINISHED, (WPARAM)new std::vector<DupPairItem>(), 0);
                    return;
                }

                std::vector<DupPairItem>* pFinalList = new std::vector<DupPairItem>();
                std::atomic<size_t> processedFiles{ 0 };

                // Stage 2 & 3: Fast Sample Hash + MMF Zero-Copy Verification (Multi-threaded)
                unsigned int numThreads = chosenThreads;
                if (numThreads == 0) numThreads = 4;
                g_dupActiveThreads = numThreads;
                g_dupCandidateTotal = candidateCount;
                g_dupScanStartTime = std::chrono::steady_clock::now();

                std::mutex resultMutex;
                std::atomic<size_t> bucketIndex{ 0 };
                const size_t totalBuckets = candidateBuckets.size();
                std::vector<std::thread> workers;

                for (unsigned int t = 0; t < numThreads; ++t) {
                    workers.emplace_back([&]() {
                        CoInitializeEx(NULL, COINIT_MULTITHREADED);
                        while (!g_bDupCancel) {
                            size_t bIdx = bucketIndex.fetch_add(1);
                            if (bIdx >= totalBuckets) break;

                            auto& bucket = candidateBuckets[bIdx];
                            std::unordered_map<uint64_t, std::vector<FileScanEntry>> hashGroups;

                            for (auto& item : bucket) {
                                if (g_bDupCancel) break;
                                uint64_t sHash = CalculateFastSampleHash(item.path, item.fileSize);
                                hashGroups[sHash].push_back(std::move(item));

                                size_t done = processedFiles.fetch_add(1) + 1;
                                if (done % 10 == 0 || done == candidateCount) {
                                    PostMessageW(hDlg, WM_APP_DUP_PROGRESS, (WPARAM)done, (LPARAM)candidateCount);
                                }
                            }

                            std::vector<DupPairItem> localMatches;
                            for (auto& hg : hashGroups) {
                                if (hg.second.size() > 1) {
                                    const auto& orig = hg.second[0];
                                    for (size_t k = 1; k < hg.second.size(); ++k) {
                                        if (g_bDupCancel) break;
                                        const auto& dupCandidate = hg.second[k];
                                        if (AreFilesContentIdentical(orig.path, dupCandidate.path, orig.fileSize)) {
                                            DupPairItem match;
                                            match.dupPath = dupCandidate.path;
                                            match.dupName = dupCandidate.name;
                                            match.fileSize = dupCandidate.fileSize;
                                            match.origPath = orig.path;
                                            match.origName = orig.name;
                                            match.isChecked = true;
                                            localMatches.push_back(std::move(match));
                                        }
                                    }
                                }
                            }

                            if (!localMatches.empty()) {
                                std::lock_guard<std::mutex> lock(resultMutex);
                                for (auto& lm : localMatches) {
                                    lm.id = (int)pFinalList->size();
                                    pFinalList->push_back(std::move(lm));
                                }
                            }
                        }
                        CoUninitialize();
                    });
                }

                for (auto& w : workers) {
                    if (w.joinable()) w.join();
                }

                CoUninitialize();
                PostMessageW(hDlg, WM_APP_DUP_FINISHED, (WPARAM)pFinalList, 0);
            });
        }
        else if (id == IDC_DUP_BTN_STOP) {
            g_bDupCancel = true;
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_STATIC_STATUS), L"중지 요청 중...");
            EnableWindow(GetDlgItem(hDlg, IDC_DUP_BTN_STOP), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_DUP_COMBO_THREADS), TRUE);
        }
        else if (id == IDC_DUP_BTN_SELECT_ALL) {
            for (int idx : g_dupVisibleIndices) {
                g_dupMatches[idx].isChecked = true;
            }
            HWND hList = GetDlgItem(hDlg, IDC_DUP_LIST);
            g_bDupUpdating = true;
            int count = ListView_GetItemCount(hList);
            for (int i = 0; i < count; ++i) {
                ListView_SetCheckState(hList, i, TRUE);
            }
            g_bDupUpdating = false;
            UpdateDupSummary(hDlg);
        }
        else if (id == IDC_DUP_BTN_DESELECT) {
            for (int idx : g_dupVisibleIndices) {
                g_dupMatches[idx].isChecked = false;
            }
            HWND hList = GetDlgItem(hDlg, IDC_DUP_LIST);
            g_bDupUpdating = true;
            int count = ListView_GetItemCount(hList);
            for (int i = 0; i < count; ++i) {
                ListView_SetCheckState(hList, i, FALSE);
            }
            g_bDupUpdating = false;
            UpdateDupSummary(hDlg);
        }
        else if (id == IDC_DUP_BTN_OPEN_DUP) {
            HWND hList = GetDlgItem(hDlg, IDC_DUP_LIST);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel >= 0) {
                LVITEMW lvi = { 0 };
                lvi.mask = LVIF_PARAM;
                lvi.iItem = sel;
                if (ListView_GetItem(hList, &lvi)) {
                    int idx = (int)lvi.lParam;
                    if (idx >= 0 && (size_t)idx < g_dupMatches.size()) {
                        ShellExecuteW(hDlg, L"open", g_dupMatches[idx].dupPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    }
                }
            } else {
                MessageBoxW(hDlg, L"열어볼 항목을 목록에서 먼저 선택해주세요.", L"안내", MB_ICONINFORMATION);
            }
        }
        else if (id == IDC_DUP_BTN_OPEN_ORIG) {
            HWND hList = GetDlgItem(hDlg, IDC_DUP_LIST);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel >= 0) {
                LVITEMW lvi = { 0 };
                lvi.mask = LVIF_PARAM;
                lvi.iItem = sel;
                if (ListView_GetItem(hList, &lvi)) {
                    int idx = (int)lvi.lParam;
                    if (idx >= 0 && (size_t)idx < g_dupMatches.size()) {
                        ShellExecuteW(hDlg, L"open", g_dupMatches[idx].origPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    }
                }
            } else {
                MessageBoxW(hDlg, L"열어볼 항목을 목록에서 먼저 선택해주세요.", L"안내", MB_ICONINFORMATION);
            }
        }
        else if (id == IDC_DUP_BTN_DELETE) {
            std::vector<int> checkedIndices;
            uint64_t totalBytes = 0;
            for (int idx : g_dupVisibleIndices) {
                if (g_dupMatches[idx].isChecked) {
                    checkedIndices.push_back(idx);
                    totalBytes += g_dupMatches[idx].fileSize;
                }
            }

            if (checkedIndices.empty()) {
                MessageBoxW(hDlg, L"삭제할 중복 파일을 먼저 체크박스에서 선택해주세요.", L"안내", MB_ICONINFORMATION);
                break;
            }

            wchar_t confirmMsg[300];
            swprintf_s(confirmMsg,
                L"선택한 %d개의 중복 파일을 휴지통으로 안전하게 이동하시겠습니까?\n\n"
                L"• 확보될 디스크 공간: %s\n"
                L"• 안전 보장: 삭제된 파일은 Windows 휴지통에서 복원할 수 있습니다.",
                (int)checkedIndices.size(), FormatFileSize(totalBytes).c_str());

            if (MessageBoxW(hDlg, confirmMsg, L"중복 파일 안전 삭제 확인", MB_ICONQUESTION | MB_YESNO) == IDYES) {
                std::vector<wchar_t> doubleNullBuf;
                for (int idx : checkedIndices) {
                    const auto& p = g_dupMatches[idx].dupPath;
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
                    std::vector<DupPairItem> remainingMatches;
                    for (size_t i = 0; i < g_dupMatches.size(); ++i) {
                        if (deletedSet.find((int)i) == deletedSet.end()) {
                            remainingMatches.push_back(std::move(g_dupMatches[i]));
                        }
                    }
                    g_dupMatches = std::move(remainingMatches);
                    for (size_t i = 0; i < g_dupMatches.size(); ++i) {
                        g_dupMatches[i].id = (int)i;
                    }

                    g_dupPreviewDup.Clear();
                    g_dupPreviewOrig.Clear();
                    SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_LBL_DUP_INFO), L"중복본: 선택 안 됨");
                    SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_LBL_ORIG_INFO), L"기준 원본: 선택 안 됨");
                    InvalidateRect(GetDlgItem(hDlg, IDC_DUP_PREVIEW_DUP), NULL, FALSE);
                    InvalidateRect(GetDlgItem(hDlg, IDC_DUP_PREVIEW_ORIG), NULL, FALSE);

                    RefreshDupListView(hDlg);

                    wchar_t doneMsg[128];
                    swprintf_s(doneMsg, L"총 %d개의 중복 파일이 휴지통으로 이동되었습니다.", (int)checkedIndices.size());
                    MessageBoxW(hDlg, doneMsg, L"삭제 완료", MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hDlg, L"일부 또는 전체 파일 삭제 중 오류가 발생했거나 취소되었습니다.", L"알림", MB_ICONWARNING);
                }
            }
        }
        else if (id == IDCANCEL) {
            if (g_bDupScanning) {
                g_bDupCancel = true;
                if (g_dupScanThread.joinable()) {
                    g_dupScanThread.join();
                }
            }
            EndDialog(hDlg, IDCANCEL);
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_CLOSE:
        if (g_bDupScanning) {
            g_bDupCancel = true;
            if (g_dupScanThread.joinable()) {
                g_dupScanThread.join();
            }
        }
        EndDialog(hDlg, IDCANCEL);
        return (INT_PTR)TRUE;
    }

    return (INT_PTR)FALSE;
}
