#include "exif_editor_dialog.h"
#include "exif_modifier.h"
#include "../Resource.h"
#include "../picto.h"
#include "../common/utils.h"
#include "../common/preview_renderer.h"

#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <unordered_set>
#include <thread>
#include <atomic>

static HWND g_hExifEditorDlg = NULL;
static std::vector<ImageDateInfo> g_exifItems;
static std::unordered_set<std::wstring> g_exifLoadedPaths;
static bool g_bExifUpdating = false;
static ImagePreviewRenderer g_exifPreview;
static std::atomic<bool> g_bExifRunning{ false };
static std::thread g_exifWorkerThread;

static void UpdateExifSummary(HWND hDlg)
{
    int total = (int)g_exifItems.size();
    int checkedCount = 0;
    for (const auto& item : g_exifItems) {
        if (item.isChecked) checkedCount++;
    }

    wchar_t buf[128];
    swprintf_s(buf, L"총 %d개 파일 (선택됨: %d개)", total, checkedCount);
    SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_STATIC_SUMMARY), buf);
    EnableWindow(GetDlgItem(hDlg, IDC_EXIF_BTN_EXECUTE), checkedCount > 0 && !g_bExifRunning);
    EnableWindow(GetDlgItem(hDlg, IDC_EXIF_BTN_REMOVE_SEL), checkedCount > 0 && !g_bExifRunning);
}

static void PopulateListViewItem(HWND hList, int row, const ImageDateInfo& item)
{
    LVITEMW lvi = { 0 };
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = row;
    lvi.iSubItem = 0;
    lvi.pszText = (LPWSTR)item.fileName.c_str();
    lvi.lParam = (LPARAM)row;
    ListView_InsertItem(hList, &lvi);

    ListView_SetItemText(hList, row, 1, (LPWSTR)item.strCurrentExif.c_str());
    ListView_SetItemText(hList, row, 2, (LPWSTR)item.strTargetTime.c_str());
    ListView_SetItemText(hList, row, 3, (LPWSTR)item.strFileTime.c_str());
    ListView_SetItemText(hList, row, 4, (LPWSTR)item.strFileSize.c_str());
    ListView_SetItemText(hList, row, 5, (LPWSTR)item.folderDir.c_str());

    ListView_SetCheckState(hList, row, item.isChecked ? TRUE : FALSE);
}

static void UpdateExifPreview(HWND hDlg, int itemIndex)
{
    HWND hPic = GetDlgItem(hDlg, IDC_EXIF_PREVIEW_PIC);
    HWND hInfo = GetDlgItem(hDlg, IDC_EXIF_PREVIEW_INFO);

    if (itemIndex < 0 || (size_t)itemIndex >= g_exifItems.size()) {
        g_exifPreview.Clear();
        InvalidateRect(hPic, NULL, FALSE);
        SetWindowTextW(hInfo, L"선택된 파일 정보가 여기에 표시됩니다.");
        return;
    }

    const auto& item = g_exifItems[itemIndex];
    g_exifPreview.SetPath(item.filePath);

    RECT rcPic = { 0 };
    GetClientRect(hPic, &rcPic);
    int pw = rcPic.right - rcPic.left;
    int ph = rcPic.bottom - rcPic.top;
    if (pw > 10 && ph > 10) {
        g_exifPreview.Load(item.filePath, pw, ph);
    }
    InvalidateRect(hPic, NULL, FALSE);

    std::wstring infoText = L"파일명: " + item.fileName;
    if (g_exifPreview.m_origW > 0 && g_exifPreview.m_origH > 0) {
        infoText += L"\n해상도: " + std::to_wstring(g_exifPreview.m_origW) + L" x " + std::to_wstring(g_exifPreview.m_origH);
    }
    infoText += L"\n현재 EXIF: " + item.strCurrentExif +
        L"\n변경 예정: " + item.strTargetTime +
        L"\n파일 수정일시: " + item.strFileTime +
        L"\n파일 용량: " + item.strFileSize +
        L"\n폴더: " + item.folderDir;
    SetWindowTextW(hInfo, infoText.c_str());
}

static void AddFilesToExifDialog(HWND hDlg, const std::vector<std::wstring>& filePaths)
{
    if (filePaths.empty()) return;

    HWND hList = GetDlgItem(hDlg, IDC_EXIF_LIST);
    g_bExifUpdating = true;
    SendMessageW(hList, WM_SETREDRAW, FALSE, 0);

    bool bAddedAny = false;
    for (const auto& path : filePaths) {
        if (path.empty()) continue;
        if (g_exifLoadedPaths.find(path) != g_exifLoadedPaths.end()) continue;

        ImageDateInfo info;
        if (ReadImageDateInfo(path, info)) {
            info.id = (int)g_exifItems.size();
            g_exifLoadedPaths.insert(path);
            g_exifItems.push_back(info);
            PopulateListViewItem(hList, (int)g_exifItems.size() - 1, g_exifItems.back());
            bAddedAny = true;
        }
    }

    SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, TRUE);
    g_bExifUpdating = false;

    if (bAddedAny && ListView_GetNextItem(hList, -1, LVNI_SELECTED) < 0 && !g_exifItems.empty()) {
        ListView_SetItemState(hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        UpdateExifPreview(hDlg, 0);
    }

    UpdateExifSummary(hDlg);
}

static bool ParseUserDate(const std::wstring& input, WORD& outYear, WORD& outMonth, WORD& outDay)
{
    std::wstring s = input;
    size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    size_t last = s.find_last_not_of(L" \t\r\n");
    s = s.substr(first, (last - first + 1));

    // 1. 순수 숫자 8자리 (예: 20181024)
    if (s.length() == 8 && std::all_of(s.begin(), s.end(), iswdigit)) {
        int y = _wtoi(s.substr(0, 4).c_str());
        int m = _wtoi(s.substr(4, 2).c_str());
        int d = _wtoi(s.substr(6, 2).c_str());
        if (y >= 1900 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            outYear = (WORD)y;
            outMonth = (WORD)m;
            outDay = (WORD)d;
            return true;
        }
    }

    // 2. 순수 숫자 6자리 (예: 181024)
    if (s.length() == 6 && std::all_of(s.begin(), s.end(), iswdigit)) {
        int y = _wtoi(s.substr(0, 2).c_str());
        y += (y < 70 ? 2000 : 1900);
        int m = _wtoi(s.substr(2, 2).c_str());
        int d = _wtoi(s.substr(4, 2).c_str());
        if (y >= 1900 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            outYear = (WORD)y;
            outMonth = (WORD)m;
            outDay = (WORD)d;
            return true;
        }
    }

    // 3. 구분자 포함 (예: 18-10-24, 2018-10-24, 18.10.24, 2018.10.24, 18/10/24, 2018/10/24 등)
    int y = 0, m = 0, d = 0;
    std::wstring cleaned = s;
    for (wchar_t& ch : cleaned) {
        if (!iswdigit(ch)) ch = L' ';
    }
    if (swscanf_s(cleaned.c_str(), L"%d %d %d", &y, &m, &d) >= 3) {
        if (y < 100) y += (y < 70 ? 2000 : 1900);
        if (y >= 1900 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            outYear = (WORD)y;
            outMonth = (WORD)m;
            outDay = (WORD)d;
            return true;
        }
    }

    return false;
}

static bool ParseUserTime(const std::wstring& input, WORD& outHour, WORD& outMinute, WORD& outSecond)
{
    std::wstring s = input;
    size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        outHour = 9; outMinute = 0; outSecond = 0;
        return true;
    }
    size_t last = s.find_last_not_of(L" \t\r\n");
    s = s.substr(first, (last - first + 1));

    // 1. 순수 숫자
    if (std::all_of(s.begin(), s.end(), iswdigit)) {
        if (s.length() == 6) { // 090000
            int h = _wtoi(s.substr(0, 2).c_str());
            int m = _wtoi(s.substr(2, 2).c_str());
            int sec = _wtoi(s.substr(4, 2).c_str());
            if (h >= 0 && h <= 23 && m >= 0 && m <= 59 && sec >= 0 && sec <= 59) {
                outHour = (WORD)h; outMinute = (WORD)m; outSecond = (WORD)sec;
                return true;
            }
        } else if (s.length() == 4) { // 0900
            int h = _wtoi(s.substr(0, 2).c_str());
            int m = _wtoi(s.substr(2, 2).c_str());
            if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
                outHour = (WORD)h; outMinute = (WORD)m; outSecond = 0;
                return true;
            }
        } else if (s.length() <= 2) { // 9
            int h = _wtoi(s.c_str());
            if (h >= 0 && h <= 23) {
                outHour = (WORD)h; outMinute = 0; outSecond = 0;
                return true;
            }
        }
    }

    // 2. 구분자 포함 (09:00:00, 09:00, 9:0)
    int h = 0, m = 0, sec = 0;
    std::wstring cleaned = s;
    for (wchar_t& ch : cleaned) {
        if (!iswdigit(ch)) ch = L' ';
    }
    int count = swscanf_s(cleaned.c_str(), L"%d %d %d", &h, &m, &sec);
    if (count >= 1) {
        if (h >= 0 && h <= 23 && m >= 0 && m <= 59 && sec >= 0 && sec <= 59) {
            outHour = (WORD)h;
            outMinute = (WORD)m;
            outSecond = (count >= 3) ? (WORD)sec : 0;
            return true;
        }
    }

    return false;
}

static void ApplyTargetDates(HWND hDlg);

static LRESULT CALLBACK DateEditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    HWND hDlg = (HWND)dwRefData;
    if (uMsg == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    }
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        ApplyTargetDates(hDlg);
        return 0;
    }
    if (uMsg == WM_SETFOCUS) {
        LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        PostMessageW(hWnd, EM_SETSEL, 0, -1);
        return res;
    }
    if (uMsg == WM_LBUTTONUP) {
        DWORD selStart = 0, selEnd = 0;
        SendMessageW(hWnd, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
        LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        if (selStart == selEnd) {
            PostMessageW(hWnd, EM_SETSEL, 0, -1);
        }
        return res;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static void ApplyTargetDates(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_EXIF_LIST);
    bool bExact = (IsDlgButtonChecked(hDlg, IDC_EXIF_RADIO_EXACT) == BST_CHECKED);
    bool bOffset = (IsDlgButtonChecked(hDlg, IDC_EXIF_RADIO_OFFSET) == BST_CHECKED);
    bool bFileName = (IsDlgButtonChecked(hDlg, IDC_EXIF_RADIO_FILENAME) == BST_CHECKED);

    SYSTEMTIME exactDate = { 0 };
    if (bExact) {
        wchar_t dBuf[64] = { 0 }, tBuf[64] = { 0 };
        GetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_DATE), dBuf, 64);
        GetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_TIME), tBuf, 64);

        WORD y = 0, m = 0, d = 0, h = 9, min = 0, s = 0;
        if (!ParseUserDate(dBuf, y, m, d)) {
            MessageBoxW(hDlg, L"날짜 형식이 올바르지 않습니다.\n\n입력 예시:\n- 181024 또는 20181024\n- 18-10-24 또는 2018-10-24", L"날짜 입력 오류", MB_ICONWARNING);
            SetFocus(GetDlgItem(hDlg, IDC_EXIF_EDIT_DATE));
            return;
        }
        if (!ParseUserTime(tBuf, h, min, s)) {
            MessageBoxW(hDlg, L"시간 형식이 올바르지 않습니다.\n\n예: 09:00:00 또는 09:00", L"시간 입력 오류", MB_ICONWARNING);
            SetFocus(GetDlgItem(hDlg, IDC_EXIF_EDIT_TIME));
            return;
        }

        exactDate.wYear = y;
        exactDate.wMonth = m;
        exactDate.wDay = d;
        exactDate.wHour = h;
        exactDate.wMinute = min;
        exactDate.wSecond = s;
        exactDate.wMilliseconds = 0;

        wchar_t normDate[32], normTime[32];
        swprintf_s(normDate, L"%04d-%02d-%02d", y, m, d);
        swprintf_s(normTime, L"%02d:%02d:%02d", h, min, s);
        SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_DATE), normDate);
        SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_TIME), normTime);
    }

    int sign = 1;
    int days = 0, hours = 0, mins = 0, secs = 0;
    if (bOffset) {
        HWND hSignCombo = GetDlgItem(hDlg, IDC_EXIF_COMBO_OFFSET_SIGN);
        int sel = (int)SendMessageW(hSignCombo, CB_GETCURSEL, 0, 0);
        if (sel == 1) sign = -1;

        wchar_t buf[32];
        GetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_OFFSET_DAYS), buf, 32);
        days = _wtoi(buf) * sign;
        GetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_OFFSET_HOURS), buf, 32);
        hours = _wtoi(buf) * sign;
        GetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_OFFSET_MINS), buf, 32);
        mins = _wtoi(buf) * sign;
        GetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_OFFSET_SECS), buf, 32);
        secs = _wtoi(buf) * sign;
    }

    g_bExifUpdating = true;
    SendMessageW(hList, WM_SETREDRAW, FALSE, 0);

    for (size_t i = 0; i < g_exifItems.size(); ++i) {
        auto& item = g_exifItems[i];
        if (!item.isChecked) continue;

        if (bExact) {
            item.targetTime = exactDate;
        } else if (bOffset) {
            SYSTEMTIME base = item.hasExif ? item.exifTime : item.fileTime;
            item.targetTime = AddTimeOffset(base, days, hours, mins, secs);
        } else if (bFileName) {
            SYSTEMTIME fnTime;
            if (ParseDateFromFileName(item.fileName, fnTime)) {
                item.targetTime = fnTime;
            }
        }
        item.strTargetTime = FormatSystemTime(item.targetTime);
        ListView_SetItemText(hList, (int)i, 2, (LPWSTR)item.strTargetTime.c_str());
    }

    SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, TRUE);
    g_bExifUpdating = false;

    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel >= 0) {
        UpdateExifPreview(hDlg, sel);
    }

    SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_STATIC_STATUS), L"변경 예정일시가 목록에 반영되었습니다.");
}

void ShowExifEditorDialog(HWND hParent, const std::vector<std::wstring>& initialFiles)
{
    if (g_hExifEditorDlg != NULL && IsWindow(g_hExifEditorDlg)) {
        if (!initialFiles.empty()) {
            std::vector<std::wstring>* pFiles = new std::vector<std::wstring>(initialFiles);
            PostMessageW(g_hExifEditorDlg, WM_APP_EXIF_ADD_FILES, (WPARAM)pFiles, 0);
        }
        SetForegroundWindow(g_hExifEditorDlg);
        if (IsIconic(g_hExifEditorDlg)) ShowWindow(g_hExifEditorDlg, SW_RESTORE);
        return;
    }

    std::vector<std::wstring>* pInit = new std::vector<std::wstring>(initialFiles);
    g_hExifEditorDlg = CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(IDD_EXIF_EDITOR),
        hParent, ExifEditorDlgProc, (LPARAM)pInit);
    if (g_hExifEditorDlg) {
        ShowWindow(g_hExifEditorDlg, SW_SHOW);
        SetForegroundWindow(g_hExifEditorDlg);
    }
}

INT_PTR CALLBACK ExifEditorDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
    {
        g_hExifEditorDlg = hDlg;
        HICON hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_PICTO));
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);

        HWND hList = GetDlgItem(hDlg, IDC_EXIF_LIST);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        lvc.iSubItem = 0; lvc.pszText = (LPWSTR)L"파일명"; lvc.cx = 150;
        ListView_InsertColumn(hList, 0, &lvc);

        lvc.iSubItem = 1; lvc.pszText = (LPWSTR)L"현재 EXIF 촬영일시"; lvc.cx = 140;
        ListView_InsertColumn(hList, 1, &lvc);

        lvc.iSubItem = 2; lvc.pszText = (LPWSTR)L"변경될 일시"; lvc.cx = 140;
        ListView_InsertColumn(hList, 2, &lvc);

        lvc.iSubItem = 3; lvc.pszText = (LPWSTR)L"파일 수정일시"; lvc.cx = 140;
        ListView_InsertColumn(hList, 3, &lvc);

        lvc.iSubItem = 4; lvc.pszText = (LPWSTR)L"파일 크기"; lvc.cx = 80;
        ListView_InsertColumn(hList, 4, &lvc);

        lvc.iSubItem = 5; lvc.pszText = (LPWSTR)L"폴더 위치"; lvc.cx = 200;
        ListView_InsertColumn(hList, 5, &lvc);

        // Option 1 default
        CheckRadioButton(hDlg, IDC_EXIF_RADIO_EXACT, IDC_EXIF_RADIO_FILENAME, IDC_EXIF_RADIO_EXACT);

        // Date/Time defaults: Today, 09:00:00
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t dateBuf[32];
        swprintf_s(dateBuf, L"%04d-%02d-%02d", now.wYear, now.wMonth, now.wDay);
        SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_DATE), dateBuf);
        SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_TIME), L"09:00:00");

        SetWindowSubclass(GetDlgItem(hDlg, IDC_EXIF_EDIT_DATE), DateEditSubclassProc, 1, (DWORD_PTR)hDlg);
        SetWindowSubclass(GetDlgItem(hDlg, IDC_EXIF_EDIT_TIME), DateEditSubclassProc, 2, (DWORD_PTR)hDlg);

        // Offset sign combo
        HWND hSign = GetDlgItem(hDlg, IDC_EXIF_COMBO_OFFSET_SIGN);
        SendMessageW(hSign, CB_ADDSTRING, 0, (LPARAM)L"+ (증가)");
        SendMessageW(hSign, CB_ADDSTRING, 0, (LPARAM)L"- (감소)");
        SendMessageW(hSign, CB_SETCURSEL, 0, 0);

        SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_OFFSET_DAYS), L"0");
        SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_OFFSET_HOURS), L"0");
        SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_OFFSET_MINS), L"0");
        SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_OFFSET_SECS), L"0");

        CheckDlgButton(hDlg, IDC_EXIF_CHK_SYNC_FILETIME, BST_CHECKED);

        g_exifPreview.Clear();
        SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_PREVIEW_INFO), L"선택된 파일 정보가 여기에 표시됩니다.");

        // Initial files
        std::vector<std::wstring>* pInit = (std::vector<std::wstring>*)lParam;
        if (pInit) {
            AddFilesToExifDialog(hDlg, *pInit);
            delete pInit;
        }

        UpdateExifSummary(hDlg);
        return (INT_PTR)TRUE;
    }

    case WM_APP_EXIF_ADD_FILES:
    {
        std::vector<std::wstring>* pFiles = (std::vector<std::wstring>*)wParam;
        if (pFiles) {
            AddFilesToExifDialog(hDlg, *pFiles);
            delete pFiles;
        }
        return (INT_PTR)TRUE;
    }

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* pMMI = (MINMAXINFO*)lParam;
        pMMI->ptMinTrackSize.x = 720;
        pMMI->ptMinTrackSize.y = 450;
        return (INT_PTR)TRUE;
    }

    case WM_SIZE:
    {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        if (w <= 0 || h <= 0) break;

        int margin = 12;
        int pad = 6;
        int previewW = (std::max)(220, (std::min)(480, (w * 32) / 100));
        int leftW = w - previewW - (margin * 2) - pad;
        if (leftW < 350) leftW = 350;
        int previewX = margin + leftW + pad;

        // Top bar buttons
        int btnY = 8;
        int subBtnW = 50;
        int curX = margin + leftW - (subBtnW * 4 + pad * 3);
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_BTN_ADD_FILES), NULL, curX, btnY, subBtnW + 12, 22, SWP_NOZORDER);
        curX += subBtnW + 12 + pad;
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_BTN_REMOVE_SEL), NULL, curX, btnY, subBtnW + 6, 22, SWP_NOZORDER);
        curX += subBtnW + 6 + pad;
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_BTN_SELECT_ALL), NULL, curX, btnY, subBtnW, 22, SWP_NOZORDER);
        curX += subBtnW + pad;
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_BTN_DESELECT), NULL, curX, btnY, subBtnW, 22, SWP_NOZORDER);

        // Options Box & Bottom Actions
        int bottomReserve = 175;
        int listTop = 34;
        int listH = h - listTop - bottomReserve;
        if (listH > 60) {
            SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_LIST), NULL, margin, listTop, leftW, listH, SWP_NOZORDER);
        }

        int grpOptsY = listTop + listH + pad;
        int grpOptsH = 96;
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_GRP_OPTIONS), NULL, margin, grpOptsY, leftW, grpOptsH, SWP_NOZORDER);

        int statusY = grpOptsY + grpOptsH + 4;
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_STATIC_STATUS), NULL, margin, statusY, leftW, 16, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_PROGRESS), NULL, margin, statusY + 18, leftW, 14, SWP_NOZORDER);

        int actBtnY = statusY + 36;
        int execBtnW = 240;
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_BTN_EXECUTE), NULL, margin, actBtnY, execBtnW, 26, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_BTN_FULLSCREEN), NULL, margin + leftW - 136, actBtnY, 65, 26, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDCANCEL), NULL, margin + leftW - 65, actBtnY, 65, 26, SWP_NOZORDER);

        // Right preview panel
        int previewH = h - margin * 2;
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_PREVIEW_GRP), NULL, previewX, margin, previewW, previewH, SWP_NOZORDER);
        int picH = (previewH * 55) / 100;
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_PREVIEW_PIC), NULL, previewX + 10, margin + 20, previewW - 20, picH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_EXIF_PREVIEW_INFO), NULL, previewX + 10, margin + 20 + picH + 8, previewW - 20, previewH - picH - 32, SWP_NOZORDER);

        InvalidateRect(hDlg, NULL, TRUE);
        int sel = ListView_GetNextItem(GetDlgItem(hDlg, IDC_EXIF_LIST), -1, LVNI_SELECTED);
        if (sel >= 0) {
            UpdateExifPreview(hDlg, sel);
        }
        break;
    }

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        if (pDIS->CtlID == IDC_EXIF_PREVIEW_PIC) {
            g_exifPreview.Draw(pDIS->hDC, pDIS->rcItem, L"사진 미리보기");
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_NOTIFY:
    {
        NMHDR* pnm = (NMHDR*)lParam;
        if (pnm->idFrom == IDC_EXIF_LIST) {
            if (pnm->code == LVN_ITEMCHANGED) {
                if (g_bExifUpdating) break;
                NMLISTVIEW* pnmlv = (NMLISTVIEW*)lParam;
                if ((pnmlv->uChanged & LVIF_STATE) &&
                    ((pnmlv->uOldState & LVIS_STATEIMAGEMASK) != (pnmlv->uNewState & LVIS_STATEIMAGEMASK))) {
                    HWND hList = GetDlgItem(hDlg, IDC_EXIF_LIST);
                    int itemIdx = pnmlv->iItem;
                    if (itemIdx >= 0 && (size_t)itemIdx < g_exifItems.size()) {
                        g_exifItems[itemIdx].isChecked = (ListView_GetCheckState(hList, itemIdx) != 0);
                    }
                    UpdateExifSummary(hDlg);
                }
                if ((pnmlv->uNewState & LVIS_SELECTED) || (pnmlv->uOldState & LVIS_SELECTED)) {
                    HWND hList = GetDlgItem(hDlg, IDC_EXIF_LIST);
                    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                    UpdateExifPreview(hDlg, sel);
                }
            }
        }
        break;
    }

    case WM_APP_EXIF_PROGRESS:
    {
        int current = (int)wParam;
        int total = (int)lParam;
        if (total > 0) {
            int pct = (int)(((double)current / total) * 100.0);
            SendMessageW(GetDlgItem(hDlg, IDC_EXIF_PROGRESS), PBM_SETPOS, pct, 0);
            wchar_t sbuf[128];
            swprintf_s(sbuf, L"EXIF 날짜 수정 중: %d / %d (%d%%)", current, total, pct);
            SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_STATIC_STATUS), sbuf);
        }
        break;
    }

    case WM_APP_EXIF_FINISHED:
    {
        g_bExifRunning = false;
        int succCount = (int)wParam;
        int total = (int)lParam;
        SendMessageW(GetDlgItem(hDlg, IDC_EXIF_PROGRESS), PBM_SETPOS, 100, 0);
        SetCursor(LoadCursor(NULL, IDC_ARROW));

        HWND hList = GetDlgItem(hDlg, IDC_EXIF_LIST);
        g_bExifUpdating = true;
        SendMessageW(hList, WM_SETREDRAW, FALSE, 0);

        // Re-read dates of modified files
        for (size_t i = 0; i < g_exifItems.size(); ++i) {
            if (g_exifItems[i].isChecked) {
                ReadImageDateInfo(g_exifItems[i].filePath, g_exifItems[i]);
                ListView_SetItemText(hList, (int)i, 1, (LPWSTR)g_exifItems[i].strCurrentExif.c_str());
                ListView_SetItemText(hList, (int)i, 3, (LPWSTR)g_exifItems[i].strFileTime.c_str());
            }
        }

        SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hList, NULL, TRUE);
        g_bExifUpdating = false;

        wchar_t resBuf[128];
        swprintf_s(resBuf, L"수정 완료: 총 %d개 중 %d개 파일의 EXIF 날짜를 성공적으로 변경했습니다.", total, succCount);
        SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_STATIC_STATUS), resBuf);
        MessageBoxW(hDlg, resBuf, L"EXIF 수정 완료", MB_ICONINFORMATION);

        EnableWindow(GetDlgItem(hDlg, IDC_EXIF_BTN_EXECUTE), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_EXIF_BTN_REMOVE_SEL), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_EXIF_BTN_ADD_FILES), TRUE);
        UpdateExifSummary(hDlg);
        break;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (code == EN_KILLFOCUS) {
            if (id == IDC_EXIF_EDIT_DATE) {
                wchar_t buf[64];
                GetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_DATE), buf, 64);
                WORD y = 0, m = 0, d = 0;
                if (ParseUserDate(buf, y, m, d)) {
                    wchar_t norm[32];
                    swprintf_s(norm, L"%04d-%02d-%02d", y, m, d);
                    SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_DATE), norm);
                }
            } else if (id == IDC_EXIF_EDIT_TIME) {
                wchar_t buf[64];
                GetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_TIME), buf, 64);
                WORD h = 0, m = 0, s = 0;
                if (ParseUserTime(buf, h, m, s)) {
                    wchar_t norm[32];
                    swprintf_s(norm, L"%02d:%02d:%02d", h, m, s);
                    SetWindowTextW(GetDlgItem(hDlg, IDC_EXIF_EDIT_TIME), norm);
                }
            }
        }

        if (id == IDC_EXIF_BTN_FULLSCREEN) {
            ShowWindow(hDlg, IsZoomed(hDlg) ? SW_RESTORE : SW_MAXIMIZE);
        }
        else if (id == IDC_EXIF_BTN_APPLY_PREVIEW) {
            ApplyTargetDates(hDlg);
        }
        else if (id == IDC_EXIF_BTN_SELECT_ALL) {
            HWND hList = GetDlgItem(hDlg, IDC_EXIF_LIST);
            g_bExifUpdating = true;
            for (size_t i = 0; i < g_exifItems.size(); ++i) {
                g_exifItems[i].isChecked = true;
                ListView_SetCheckState(hList, (int)i, TRUE);
            }
            g_bExifUpdating = false;
            UpdateExifSummary(hDlg);
        }
        else if (id == IDC_EXIF_BTN_DESELECT) {
            HWND hList = GetDlgItem(hDlg, IDC_EXIF_LIST);
            g_bExifUpdating = true;
            for (size_t i = 0; i < g_exifItems.size(); ++i) {
                g_exifItems[i].isChecked = false;
                ListView_SetCheckState(hList, (int)i, FALSE);
            }
            g_bExifUpdating = false;
            UpdateExifSummary(hDlg);
        }
        else if (id == IDC_EXIF_BTN_REMOVE_SEL) {
            HWND hList = GetDlgItem(hDlg, IDC_EXIF_LIST);
            g_bExifUpdating = true;
            SendMessageW(hList, WM_SETREDRAW, FALSE, 0);

            std::vector<ImageDateInfo> remaining;
            g_exifLoadedPaths.clear();
            ListView_DeleteAllItems(hList);

            for (const auto& item : g_exifItems) {
                if (!item.isChecked) {
                    remaining.push_back(item);
                    g_exifLoadedPaths.insert(item.filePath);
                    PopulateListViewItem(hList, (int)remaining.size() - 1, remaining.back());
                }
            }
            g_exifItems = std::move(remaining);

            SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(hList, NULL, TRUE);
            g_bExifUpdating = false;

            if (!g_exifItems.empty()) {
                ListView_SetItemState(hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                UpdateExifPreview(hDlg, 0);
            } else {
                UpdateExifPreview(hDlg, -1);
            }
            UpdateExifSummary(hDlg);
        }
        else if (id == IDC_EXIF_BTN_ADD_FILES) {
            const DWORD BUF_SIZE = 65536;
            std::vector<wchar_t> fileBuf(BUF_SIZE, 0);

            OPENFILENAMEW ofn = { 0 };
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hDlg;
            ofn.lpstrFilter = L"이미지 파일 (*.jpg;*.jpeg;*.png;*.heic;*.webp;*.tif;*.tiff)\0*.jpg;*.jpeg;*.png;*.heic;*.webp;*.tif;*.tiff\0모든 파일 (*.*)\0*.*\0";
            ofn.lpstrFile = fileBuf.data();
            ofn.nMaxFile = BUF_SIZE;
            ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;

            if (GetOpenFileNameW(&ofn)) {
                std::vector<std::wstring> selected;
                wchar_t* p = fileBuf.data();
                std::wstring dir = p;
                p += dir.size() + 1;

                if (*p == L'\0') {
                    // Single file
                    selected.push_back(dir);
                } else {
                    // Multi files
                    while (*p != L'\0') {
                        std::wstring fn = p;
                        std::wstring fullPath = dir + L"\\" + fn;
                        selected.push_back(fullPath);
                        p += fn.size() + 1;
                    }
                }
                AddFilesToExifDialog(hDlg, selected);
            }
        }
        else if (id == IDC_EXIF_BTN_EXECUTE) {
            std::vector<ImageDateInfo> targets;
            for (const auto& item : g_exifItems) {
                if (item.isChecked) targets.push_back(item);
            }

            if (targets.empty()) {
                MessageBoxW(hDlg, L"수정할 파일을 목록에서 하나 이상 선택해주세요.", L"알림", MB_ICONWARNING);
                break;
            }

            // Ensure target dates are updated before execution
            ApplyTargetDates(hDlg);

            // Re-fetch targets with updated targetTime
            targets.clear();
            for (const auto& item : g_exifItems) {
                if (item.isChecked) targets.push_back(item);
            }

            wchar_t confirmBuf[256];
            swprintf_s(confirmBuf, L"선택된 %zu개 사진 파일의 EXIF 촬영일시를 일괄 변경하시겠습니까?\n\n(화질 손실 없는 무손실 초고속 수정이 적용됩니다)", targets.size());
            if (MessageBoxW(hDlg, confirmBuf, L"EXIF 일괄 변경 확인", MB_ICONQUESTION | MB_YESNO) != IDYES) {
                break;
            }

            bool bSyncFileTime = (IsDlgButtonChecked(hDlg, IDC_EXIF_CHK_SYNC_FILETIME) == BST_CHECKED);

            if (g_exifWorkerThread.joinable()) {
                g_exifWorkerThread.join();
            }

            g_bExifRunning = true;
            EnableWindow(GetDlgItem(hDlg, IDC_EXIF_BTN_EXECUTE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_EXIF_BTN_REMOVE_SEL), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_EXIF_BTN_ADD_FILES), FALSE);
            SetCursor(LoadCursor(NULL, IDC_WAIT));
            SendMessageW(GetDlgItem(hDlg, IDC_EXIF_PROGRESS), PBM_SETPOS, 0, 0);

            g_exifWorkerThread = std::thread([hDlg, targets, bSyncFileTime]() {
                CoInitializeEx(NULL, COINIT_MULTITHREADED);
                int succ = 0;
                int total = (int)targets.size();

                for (int i = 0; i < total; ++i) {
                    if (ModifyImageExifDate(targets[i].filePath, targets[i].targetTime, bSyncFileTime)) {
                        succ++;
                    }
                    if ((i + 1) % 5 == 0 || (i + 1) == total) {
                        PostMessageW(hDlg, WM_APP_EXIF_PROGRESS, (WPARAM)(i + 1), (LPARAM)total);
                    }
                }

                CoUninitialize();
                PostMessageW(hDlg, WM_APP_EXIF_FINISHED, (WPARAM)succ, (LPARAM)total);
            });
        }
        else if (id == IDCANCEL) {
            DestroyWindow(hDlg);
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hDlg);
        break;

    case WM_DESTROY:
        if (g_exifWorkerThread.joinable()) {
            g_exifWorkerThread.join();
        }
        g_exifPreview.Clear();
        g_exifItems.clear();
        g_exifLoadedPaths.clear();
        g_hExifEditorDlg = NULL;
        break;

    default:
        return FALSE;
    }
    return TRUE;
}
