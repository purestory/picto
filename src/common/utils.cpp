#include "utils.h"
#include "picto.h"
#include "Resource.h"

// Font handles
HFONT g_hFontNormal = NULL;
HFONT g_hFontBold = NULL;
HFONT g_hFontTitle = NULL;
HFONT g_hFontLog = NULL;

// Logging globals
std::mutex g_logMutex;
std::vector<std::wstring> g_logQueue;

// DPI Awareness & Scaling Helper Functions
void InitDpiAwareness()
{
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL(WINAPI* SetProcessDpiAwarenessContextProc)(HANDLE);
        SetProcessDpiAwarenessContextProc setContext =
            (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setContext) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((HANDLE)-4)
            if (setContext((HANDLE)-4)) {
                return;
            }
        }
    }

    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        typedef enum {
            PROCESS_DPI_UNAWARE = 0,
            PROCESS_SYSTEM_DPI_AWARE = 1,
            PROCESS_PER_MONITOR_DPI_AWARE = 2
        } PROCESS_DPI_AWARENESS;
        typedef HRESULT(WINAPI* SetProcessDpiAwarenessProc)(PROCESS_DPI_AWARENESS);
        SetProcessDpiAwarenessProc setDpiAwareness =
            (SetProcessDpiAwarenessProc)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (setDpiAwareness) {
            if (SUCCEEDED(setDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
                FreeLibrary(hShcore);
                return;
            }
        }
        FreeLibrary(hShcore);
    }

    SetProcessDPIAware();
}

UINT GetWindowDpi(HWND hWnd)
{
    typedef UINT(WINAPI* GetDpiForWindowProc)(HWND);
    static GetDpiForWindowProc pGetDpiForWindow = (GetDpiForWindowProc)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (pGetDpiForWindow && hWnd && IsWindow(hWnd)) {
        UINT dpi = pGetDpiForWindow(hWnd);
        if (dpi > 0) return dpi;
    }

    HDC hdc = GetDC(hWnd);
    if (hdc) {
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(hWnd, hdc);
        if (dpi > 0) return (UINT)dpi;
    }
    return 96;
}

int ScaleDpi(int value, UINT dpi)
{
    return MulDiv(value, (int)dpi, 96);
}

static UINT s_cachedDpi = 0;

void CreateDpiFonts(UINT dpi)
{
    if (s_cachedDpi == dpi && g_hFontNormal && g_hFontBold && g_hFontTitle && g_hFontLog) {
        return;
    }
    s_cachedDpi = dpi;

    if (g_hFontNormal) DeleteObject(g_hFontNormal);
    if (g_hFontBold) DeleteObject(g_hFontBold);
    if (g_hFontTitle) DeleteObject(g_hFontTitle);
    if (g_hFontLog) DeleteObject(g_hFontLog);

    int normalHeight = -MulDiv(13, (int)dpi, 96);
    int boldHeight = -MulDiv(14, (int)dpi, 96);
    int titleHeight = -MulDiv(18, (int)dpi, 96);
    int logHeight = -MulDiv(12, (int)dpi, 96);

    g_hFontNormal = CreateFontW(normalHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!g_hFontNormal) {
        g_hFontNormal = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }

    // Use "Segoe UI Emoji" first so emoji symbols in buttons never break into '||' upon focus/click
    g_hFontBold = CreateFontW(boldHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Emoji");
    if (!g_hFontBold) {
        g_hFontBold = CreateFontW(boldHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }

    g_hFontTitle = CreateFontW(titleHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    g_hFontLog = CreateFontW(logHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

void ApplyFontsToControls(HWND hWnd)
{
    HWND hChild = GetWindow(hWnd, GW_CHILD);
    while (hChild) {
        int id = GetDlgCtrlID(hChild);
        if (id == IDC_EDIT_LOG) {
            SendMessageW(hChild, WM_SETFONT, (WPARAM)g_hFontLog, TRUE);
        } else if (id == IDC_MAIN_TITLE) {
            SendMessageW(hChild, WM_SETFONT, (WPARAM)g_hFontTitle, TRUE);
        } else if (id == IDC_BTN_START || id == IDC_BTN_DUP_CLEAN || id == IDC_BTN_RESIZED_CLEAN ||
                   id == IDC_BTN_GPS_MAP || id == IDC_BTN_EXIF_EDIT || id == IDC_STATIC_STATUS) {
            SendMessageW(hChild, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        } else {
            SendMessageW(hChild, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        }
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }
}

std::wstring FormatFileSize(uint64_t bytes)
{
    wchar_t buf[64];
    if (bytes < 1024) {
        swprintf_s(buf, L"%llu B", bytes);
    } else if (bytes < 1024 * 1024) {
        swprintf_s(buf, L"%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        swprintf_s(buf, L"%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

void EnqueueLog(const std::wstring& msg)
{
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    struct tm tmNow;
    localtime_s(&tmNow, &timeT);

    wchar_t timeBuf[32];
    swprintf_s(timeBuf, L"[%02d:%02d:%02d] ", tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);

    std::wstring formatted = std::wstring(timeBuf) + msg + L"\r\n";

    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_logQueue.push_back(std::move(formatted));
    }
}

void DrainLogQueue(HWND hEditLog)
{
    std::vector<std::wstring> localQueue;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logQueue.empty()) return;
        localQueue.swap(g_logQueue);
    }

    std::wstring combined;
    for (const auto& line : localQueue) {
        combined += line;
    }

    int length = GetWindowTextLengthW(hEditLog);
    SendMessageW(hEditLog, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    SendMessageW(hEditLog, EM_REPLACESEL, FALSE, (LPARAM)combined.c_str());
    SendMessageW(hEditLog, EM_SCROLLCARET, 0, 0);
}

bool BrowseFolder(HWND hParent, std::wstring& outPath)
{
    IFileDialog* pfd = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (SUCCEEDED(hr)) {
        DWORD dwOptions;
        if (SUCCEEDED(pfd->GetOptions(&dwOptions))) {
            pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        }
        pfd->SetTitle(L"폴더 선택");

        if (SUCCEEDED(pfd->Show(hParent))) {
            IShellItem* psi = NULL;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath = NULL;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    outPath = pszPath;
                    CoTaskMemFree(pszPath);
                    psi->Release();
                    pfd->Release();
                    return true;
                }
                psi->Release();
            }
        }
        pfd->Release();
        return false;
    }

    BROWSEINFOW bi = { 0 };
    bi.hwndOwner = hParent;
    bi.lpszTitle = L"폴더를 선택하세요:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            outPath = path;
            CoTaskMemFree(pidl);
            return true;
        }
        CoTaskMemFree(pidl);
    }
    return false;
}

bool BrowseFile(HWND hParent, std::wstring& outPath, const wchar_t* filter)
{
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hParent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        outPath = szFile;
        return true;
    }
    return false;
}

bool BrowseMultipleFiles(HWND hParent, std::vector<std::wstring>& outFiles, const wchar_t* filter)
{
    outFiles.clear();
    std::vector<wchar_t> buffer(262144, 0);

    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hParent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = (DWORD)buffer.size();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        const wchar_t* p = buffer.data();
        std::wstring firstStr = p;
        p += firstStr.length() + 1;

        if (*p == L'\0') {
            outFiles.push_back(firstStr);
        } else {
            std::filesystem::path dirPath(firstStr);
            while (*p != L'\0') {
                std::wstring fileName = p;
                std::filesystem::path fullPath = dirPath / fileName;
                outFiles.push_back(fullPath.wstring());
                p += fileName.length() + 1;
            }
        }
        return !outFiles.empty();
    }
    return false;
}

LRESULT CALLBACK FilterEditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    switch (uMsg) {
    case WM_GETDLGCODE:
    {
        LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        return res | DLGC_WANTALLKEYS;
    }

    case WM_KEYDOWN:
    {
        bool isEnter = (wParam == VK_RETURN);
        if (!isEnter && wParam == VK_PROCESSKEY) {
            UINT vk = ImmGetVirtualKey(hWnd);
            if (vk == VK_RETURN) {
                isEnter = true;
            }
        }
        if (isEnter) {
            HIMC hImc = ImmGetContext(hWnd);
            if (hImc) {
                ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
                ImmReleaseContext(hWnd, hImc);
            }
            HWND hDlg = (HWND)dwRefData;
            int ctrlId = GetDlgCtrlID(hWnd);
            PostMessageW(hDlg, WM_APP_TRIGGER_SEARCH, (WPARAM)ctrlId, 0);
            return 0;
        }
        break;
    }

    case WM_KEYUP:
    {
        bool isEnter = (wParam == VK_RETURN);
        if (!isEnter && wParam == VK_PROCESSKEY) {
            UINT vk = ImmGetVirtualKey(hWnd);
            if (vk == VK_RETURN) {
                isEnter = true;
            }
        }
        if (isEnter) {
            HIMC hImc = ImmGetContext(hWnd);
            if (hImc) {
                ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
                ImmReleaseContext(hWnd, hImc);
            }
            HWND hDlg = (HWND)dwRefData;
            int ctrlId = GetDlgCtrlID(hWnd);
            PostMessageW(hDlg, WM_APP_TRIGGER_SEARCH, (WPARAM)ctrlId, 0);
            return 0;
        }
        break;
    }

    case WM_CHAR:
        if (wParam == VK_RETURN || wParam == 10 || wParam == 13) {
            HIMC hImc = ImmGetContext(hWnd);
            if (hImc) {
                ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
                ImmReleaseContext(hWnd, hImc);
            }
            HWND hDlg = (HWND)dwRefData;
            int ctrlId = GetDlgCtrlID(hWnd);
            PostMessageW(hDlg, WM_APP_TRIGGER_SEARCH, (WPARAM)ctrlId, 0);
            return 0;
        }
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, FilterEditSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

std::vector<std::wstring> ParseExcludeTokens(const std::wstring& input)
{
    std::vector<std::wstring> tokens;
    size_t start = 0;
    while (start < input.size()) {
        size_t comma = input.find(L',', start);
        std::wstring token;
        if (comma == std::wstring::npos) {
            token = input.substr(start);
            start = input.size();
        } else {
            token = input.substr(start, comma - start);
            start = comma + 1;
        }

        size_t first = token.find_first_not_of(L" \t\r\n\"'");
        if (first != std::wstring::npos) {
            size_t last = token.find_last_not_of(L" \t\r\n\"'");
            std::wstring trimmed = token.substr(first, (last - first + 1));
            for (auto& ch : trimmed) {
                if (ch == L'/') ch = L'\\';
            }
            if (trimmed.size() > 1 && trimmed.back() == L'\\') {
                trimmed.pop_back();
            }
            std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), ::towlower);
            if (!trimmed.empty()) {
                tokens.push_back(std::move(trimmed));
            }
        }
    }
    return tokens;
}
