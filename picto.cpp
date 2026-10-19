// picto.cpp : High-performance Multi-threaded Photo/Video Organizer
// Ported from Python picto to Win32 C++ (Visual Studio 2022)
// Enhanced with Memory-First 3-Stage Deduplication & Memory-Mapped File I/O

#include "framework.h"
#include "picto.h"
#include "perceptual_hash.h"

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Global application state
HINSTANCE g_hInst = NULL;
HWND g_hMainWnd = NULL;
HFONT g_hFontNormal = NULL;
HFONT g_hFontBold = NULL;
HFONT g_hFontLog = NULL;
UINT g_currentDpi = 96;

// Control Handles
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
HWND g_hChkOrgMonth = NULL;
HWND g_hChkCopyFiles = NULL;
HWND g_hChkRemoveDups = NULL;

HWND g_hBtnStart = NULL;
HWND g_hBtnCancel = NULL;
HWND g_hBtnDupClean = NULL;
HWND g_hBtnResizedClean = NULL;
HWND g_hBtnGpsMap = NULL;

HWND g_hGrpProgress = NULL;
HWND g_hLblStatus = NULL;
HWND g_hProgressBar = NULL;
HWND g_hLblProcessed = NULL;
HWND g_hLblMoved = NULL;
HWND g_hLblDups = NULL;
HWND g_hLblSpeed = NULL;

HWND g_hGrpLog = NULL;
HWND g_hEditLog = NULL;

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
std::mutex g_candidateMutex;
std::unordered_map<uint64_t, std::vector<CandidateRecord>> g_candidatesBySize;

std::mutex g_logMutex;
std::vector<std::wstring> g_logQueue;

// GPS Map Dialog Globals
std::vector<GpsPoint> g_batchGpsData;
std::atomic<bool> g_bGpsScanning{ false };
std::thread g_gpsScanThread;

// -------------------------------------------------------------
// DPI Awareness & Scaling Helper Functions
// -------------------------------------------------------------
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

void CreateDpiFonts(UINT dpi)
{
    if (g_hFontNormal) DeleteObject(g_hFontNormal);
    if (g_hFontBold) DeleteObject(g_hFontBold);
    if (g_hFontLog) DeleteObject(g_hFontLog);

    int normalHeight = -MulDiv(13, (int)dpi, 96);
    int boldHeight = -MulDiv(14, (int)dpi, 96);
    int logHeight = -MulDiv(12, (int)dpi, 96);

    g_hFontNormal = CreateFontW(normalHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!g_hFontNormal) {
        g_hFontNormal = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }

    g_hFontBold = CreateFontW(boldHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
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
        } else if (id == IDC_BTN_START || id == IDC_BTN_DUP_CLEAN || id == IDC_BTN_RESIZED_CLEAN || id == IDC_STATIC_STATUS) {
            SendMessageW(hChild, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        } else {
            SendMessageW(hChild, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        }
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }
}

inline std::wstring FormatFileSize(uint64_t bytes)
{
    wchar_t buf[64];
    if (bytes < 1024) {
        swprintf_s(buf, L"%llu B", bytes);
    } else if (bytes < 1024 * 1024) {
        swprintf_s(buf, L"%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024 * 1024 * 1024) {
        swprintf_s(buf, L"%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

// -------------------------------------------------------------
// Image Preview Rendering Engine (WIC + stb_image with Letterbox)
// -------------------------------------------------------------
class ImagePreviewRenderer {
public:
    HBITMAP m_hBitmap = NULL;
    int m_origW = 0;
    int m_origH = 0;
    int m_targetW = 0;
    int m_targetH = 0;
    uint64_t m_fileSize = 0;
    std::wstring m_currentPath;

    ~ImagePreviewRenderer() {
        Clear();
    }

    void Clear() {
        if (m_hBitmap) {
            DeleteObject(m_hBitmap);
            m_hBitmap = NULL;
        }
        m_origW = 0;
        m_origH = 0;
        m_targetW = 0;
        m_targetH = 0;
        m_fileSize = 0;
        m_currentPath.clear();
    }

    void SetPath(const std::wstring& filePath) {
        if (m_currentPath != filePath) {
            Clear();
            m_currentPath = filePath;
        }
    }

    bool Load(const std::wstring& filePath, int targetW, int targetH) {
        if (targetW <= 10 || targetH <= 10) return false;
        if (filePath == m_currentPath && m_hBitmap != NULL && m_targetW == targetW && m_targetH == targetH) return true;

        if (m_hBitmap) {
            DeleteObject(m_hBitmap);
            m_hBitmap = NULL;
        }
        m_currentPath = filePath;
        m_targetW = targetW;
        m_targetH = targetH;

        if (filePath.empty() || !PathFileExistsW(filePath.c_str())) {
            return false;
        }

        std::error_code ec;
        m_fileSize = std::filesystem::file_size(filePath, ec);

        // 1. Try Windows Imaging Component (WIC) with Hardware-accelerated Fant Scaling
        IWICImagingFactory* pFactory = NULL;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
        if (SUCCEEDED(hr)) {
            IWICBitmapDecoder* pDecoder = NULL;
            hr = pFactory->CreateDecoderFromFilename(filePath.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
            if (SUCCEEDED(hr)) {
                IWICBitmapFrameDecode* pFrame = NULL;
                hr = pDecoder->GetFrame(0, &pFrame);
                if (SUCCEEDED(hr)) {
                    UINT origW = 0, origH = 0;
                    pFrame->GetSize(&origW, &origH);
                    m_origW = (int)origW;
                    m_origH = (int)origH;

                    if (origW > 0 && origH > 0) {
                        float scale = (std::min)((float)targetW / (float)origW, (float)targetH / (float)origH);
                        int fitW = (std::max)(1, (int)(origW * scale));
                        int fitH = (std::max)(1, (int)(origH * scale));

                        IWICBitmapScaler* pScaler = NULL;
                        hr = pFactory->CreateBitmapScaler(&pScaler);
                        if (SUCCEEDED(hr)) {
                            hr = pScaler->Initialize(pFrame, fitW, fitH, WICBitmapInterpolationModeFant);
                            if (SUCCEEDED(hr)) {
                                IWICFormatConverter* pConverter = NULL;
                                hr = pFactory->CreateFormatConverter(&pConverter);
                                if (SUCCEEDED(hr)) {
                                    hr = pConverter->Initialize(pScaler, GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
                                    if (SUCCEEDED(hr)) {
                                        BITMAPINFO bmi = { 0 };
                                        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                                        bmi.bmiHeader.biWidth = targetW;
                                        bmi.bmiHeader.biHeight = -targetH; // Top-down
                                        bmi.bmiHeader.biPlanes = 1;
                                        bmi.bmiHeader.biBitCount = 32;
                                        bmi.bmiHeader.biCompression = BI_RGB;

                                        void* pBits = NULL;
                                        HDC hScreenDC = GetDC(NULL);
                                        HBITMAP hDIB = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
                                        ReleaseDC(NULL, hScreenDC);

                                        if (hDIB && pBits) {
                                            uint32_t* pPixels = (uint32_t*)pBits;
                                            for (int i = 0; i < targetW * targetH; ++i) {
                                                pPixels[i] = 0xFF18181B; // Sleek dark slate #18181b
                                            }

                                            int startX = (targetW - fitW) / 2;
                                            int startY = (targetH - fitH) / 2;

                                            std::vector<BYTE> scaledBuf(fitW * fitH * 4);
                                            hr = pConverter->CopyPixels(NULL, fitW * 4, (UINT)scaledBuf.size(), scaledBuf.data());
                                            if (SUCCEEDED(hr)) {
                                                for (int y = 0; y < fitH; ++y) {
                                                    uint32_t* dstLine = pPixels + (startY + y) * targetW + startX;
                                                    uint32_t* srcLine = (uint32_t*)(scaledBuf.data() + y * fitW * 4);
                                                    memcpy(dstLine, srcLine, fitW * 4);
                                                }
                                                m_hBitmap = hDIB;
                                            } else {
                                                DeleteObject(hDIB);
                                            }
                                        }
                                    }
                                    pConverter->Release();
                                }
                            }
                            pScaler->Release();
                        }
                    }
                    pFrame->Release();
                }
                pDecoder->Release();
            }
            pFactory->Release();
        }

        // 2. Fallback to stb_image + stb_image_resize2
        if (!m_hBitmap) {
            FILE* fp = NULL;
            if (_wfopen_s(&fp, filePath.c_str(), L"rb") == 0 && fp) {
                fseek(fp, 0, SEEK_END);
                long fileSize = ftell(fp);
                fseek(fp, 0, SEEK_SET);

                if (fileSize > 0 && fileSize < 200 * 1024 * 1024) {
                    std::vector<unsigned char> fileBuf(fileSize);
                    if (fread(fileBuf.data(), 1, fileSize, fp) == (size_t)fileSize) {
                        int w = 0, h = 0, comp = 0;
                        unsigned char* pixels = stbi_load_from_memory(fileBuf.data(), (int)fileSize, &w, &h, &comp, 4);
                        if (pixels) {
                            m_origW = w;
                            m_origH = h;
                            float scale = (std::min)((float)targetW / (float)w, (float)targetH / (float)h);
                            int fitW = (std::max)(1, (int)(w * scale));
                            int fitH = (std::max)(1, (int)(h * scale));

                            std::vector<unsigned char> scaledBuf(fitW * fitH * 4);
                            stbir_resize_uint8_linear(pixels, w, h, 0, scaledBuf.data(), fitW, fitH, 0, STBIR_RGBA);

                            BITMAPINFO bmi = { 0 };
                            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                            bmi.bmiHeader.biWidth = targetW;
                            bmi.bmiHeader.biHeight = -targetH;
                            bmi.bmiHeader.biPlanes = 1;
                            bmi.bmiHeader.biBitCount = 32;
                            bmi.bmiHeader.biCompression = BI_RGB;

                            void* pBits = NULL;
                            HDC hScreenDC = GetDC(NULL);
                            HBITMAP hDIB = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
                            ReleaseDC(NULL, hScreenDC);

                            if (hDIB && pBits) {
                                uint32_t* pPixels = (uint32_t*)pBits;
                                for (int i = 0; i < targetW * targetH; ++i) {
                                    pPixels[i] = 0xFF18181B;
                                }

                                int startX = (targetW - fitW) / 2;
                                int startY = (targetH - fitH) / 2;

                                for (int y = 0; y < fitH; ++y) {
                                    uint32_t* dstLine = pPixels + (startY + y) * targetW + startX;
                                    unsigned char* srcLine = scaledBuf.data() + y * fitW * 4;
                                    for (int x = 0; x < fitW; ++x) {
                                        unsigned char r = srcLine[x * 4 + 0];
                                        unsigned char g = srcLine[x * 4 + 1];
                                        unsigned char b = srcLine[x * 4 + 2];
                                        unsigned char a = srcLine[x * 4 + 3];
                                        dstLine[x] = (a << 24) | (r << 16) | (g << 8) | b;
                                    }
                                }
                                m_hBitmap = hDIB;
                            }
                            stbi_image_free(pixels);
                        }
                    }
                }
                fclose(fp);
            }
        }

        return (m_hBitmap != NULL);
    }

    void Draw(HDC hdc, const RECT& rect, const std::wstring& placeholder) {
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0) return;

        if (!m_currentPath.empty() && (m_targetW != w || m_targetH != h || m_hBitmap == NULL)) {
            Load(m_currentPath, w, h);
        }

        if (m_hBitmap) {
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, m_hBitmap);
            BitBlt(hdc, rect.left, rect.top, w, h, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteDC(memDC);

            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(55, 65, 81));
            HGDIOBJ oldPen = SelectObject(hdc, hPen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(hPen);
        } else {
            HBRUSH hBrush = CreateSolidBrush(RGB(24, 24, 27));
            FillRect(hdc, &rect, hBrush);
            DeleteObject(hBrush);

            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(55, 65, 81));
            HGDIOBJ oldPen = SelectObject(hdc, hPen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(hPen);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(156, 163, 175));
            RECT textRc = rect;
            DrawTextW(hdc, placeholder.c_str(), -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
};

// -------------------------------------------------------------
// Logging & Safe UI Helpers
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// Folder & File Selection Dialogs
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// Memory-First Ultra-Fast Deduplication Functions
// -------------------------------------------------------------

// Stage 2: Fast 64-bit FNV-1a Sample Hash (Header + Trailer 16KB)
uint64_t CalculateFastSampleHash(const std::wstring& filePath, uint64_t fileSize)
{
    if (fileSize == 0) return 0;

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    uint64_t hash = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;
    const DWORD SAMPLE_SIZE = 16384; // 16 KB

    if (fileSize <= SAMPLE_SIZE * 2) {
        BYTE buf[32768];
        DWORD bytesRead = 0;
        if (ReadFile(hFile, buf, (DWORD)fileSize, &bytesRead, NULL) && bytesRead > 0) {
            for (DWORD i = 0; i < bytesRead; ++i) {
                hash ^= buf[i];
                hash *= prime;
            }
        }
    } else {
        // Sample 1: First 16KB
        BYTE headBuf[SAMPLE_SIZE];
        DWORD headRead = 0;
        if (ReadFile(hFile, headBuf, SAMPLE_SIZE, &headRead, NULL)) {
            for (DWORD i = 0; i < headRead; ++i) {
                hash ^= headBuf[i];
                hash *= prime;
            }
        }

        // Sample 2: Last 16KB
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)(fileSize - SAMPLE_SIZE);
        if (SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) {
            BYTE tailBuf[SAMPLE_SIZE];
            DWORD tailRead = 0;
            if (ReadFile(hFile, tailBuf, SAMPLE_SIZE, &tailRead, NULL)) {
                for (DWORD i = 0; i < tailRead; ++i) {
                    hash ^= tailBuf[i];
                    hash *= prime;
                }
            }
        }
    }

    CloseHandle(hFile);
    return hash;
}

// Stage 3: Zero-Copy Memory-Mapped File (MMF) Binary Comparison
bool AreFilesContentIdentical(const std::wstring& path1, const std::wstring& path2, uint64_t fileSize)
{
    if (fileSize == 0) return true;

    HANDLE hFile1 = CreateFileW(path1.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile1 == INVALID_HANDLE_VALUE) return false;

    HANDLE hFile2 = CreateFileW(path2.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile2 == INVALID_HANDLE_VALUE) {
        CloseHandle(hFile1);
        return false;
    }

    bool bIdentical = false;

    // Use Memory-Mapped Files for up to 128 MB (Zero-copy, SIMD vectorized memcmp)
    if (fileSize <= 128 * 1024 * 1024) {
        HANDLE hMap1 = CreateFileMappingW(hFile1, NULL, PAGE_READONLY, 0, 0, NULL);
        HANDLE hMap2 = CreateFileMappingW(hFile2, NULL, PAGE_READONLY, 0, 0, NULL);

        if (hMap1 && hMap2) {
            const void* p1 = MapViewOfFile(hMap1, FILE_MAP_READ, 0, 0, (size_t)fileSize);
            const void* p2 = MapViewOfFile(hMap2, FILE_MAP_READ, 0, 0, (size_t)fileSize);

            if (p1 && p2) {
                bIdentical = (memcmp(p1, p2, (size_t)fileSize) == 0);
            }
            if (p1) UnmapViewOfFile(p1);
            if (p2) UnmapViewOfFile(p2);
        }
        if (hMap1) CloseHandle(hMap1);
        if (hMap2) CloseHandle(hMap2);
    } else {
        // Stream in 2MB large memory chunks for massive video files
        const size_t CHUNK_SIZE = 2 * 1024 * 1024;
        std::vector<BYTE> buf1(CHUNK_SIZE);
        std::vector<BYTE> buf2(CHUNK_SIZE);

        uint64_t remaining = fileSize;
        bIdentical = true;

        while (remaining > 0) {
            DWORD toRead = (DWORD)std::min<uint64_t>(remaining, CHUNK_SIZE);
            DWORD read1 = 0, read2 = 0;

            if (!ReadFile(hFile1, buf1.data(), toRead, &read1, NULL) ||
                !ReadFile(hFile2, buf2.data(), toRead, &read2, NULL) ||
                read1 != toRead || read2 != toRead) {
                bIdentical = false;
                break;
            }

            if (memcmp(buf1.data(), buf2.data(), toRead) != 0) {
                bIdentical = false;
                break;
            }
            remaining -= toRead;
        }
    }

    CloseHandle(hFile1);
    CloseHandle(hFile2);
    return bIdentical;
}

// Fallback MD5 calculator using 1MB large streaming RAM buffer
bool CalculateFileMD5(const std::wstring& filePath, std::string& outHash)
{
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    bool bSuccess = false;

    if (CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
            // Allocate 1MB RAM buffer to maximize sequential disk bandwidth
            const size_t BUF_SIZE = 1024 * 1024;
            std::vector<BYTE> buffer(BUF_SIZE);
            DWORD bytesRead = 0;
            bool readOk = true;

            while (ReadFile(hFile, buffer.data(), (DWORD)BUF_SIZE, &bytesRead, NULL) && bytesRead > 0) {
                if (!CryptHashData(hHash, buffer.data(), bytesRead, 0)) {
                    readOk = false;
                    break;
                }
            }

            if (readOk) {
                BYTE hashBytes[16];
                DWORD hashLen = sizeof(hashBytes);
                if (CryptGetHashParam(hHash, HP_HASHVAL, hashBytes, &hashLen, 0)) {
                    char hexStr[33];
                    for (int i = 0; i < 16; ++i) {
                        sprintf_s(hexStr + (i * 2), 3, "%02x", hashBytes[i]);
                    }
                    hexStr[32] = '\0';
                    outHash = hexStr;
                    bSuccess = true;
                }
            }
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }

    CloseHandle(hFile);
    return bSuccess;
}

// -------------------------------------------------------------
// In-Memory Fast EXIF & Date Extraction Functions
// -------------------------------------------------------------
static bool ParseExifDateString(const char* str, SYSTEMTIME& outTime)
{
    if (!str) return false;
    int y = 0, m = 0, d = 0, hr = 0, min = 0, sec = 0;
    if (sscanf_s(str, "%d:%d:%d %d:%d:%d", &y, &m, &d, &hr, &min, &sec) == 6) {
        if (y >= 1970 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            ZeroMemory(&outTime, sizeof(outTime));
            outTime.wYear = (WORD)y;
            outTime.wMonth = (WORD)m;
            outTime.wDay = (WORD)d;
            outTime.wHour = (WORD)hr;
            outTime.wMinute = (WORD)min;
            outTime.wSecond = (WORD)sec;
            return true;
        }
    }
    return false;
}

// Ultra-fast memory buffer JPEG APP1 EXIF parser (0 seek, zero-syscall in-RAM)
static bool FastScanJpegExifDateFromMemory(const unsigned char* pData, size_t dataLen, SYSTEMTIME& outTime)
{
    if (!pData || dataLen < 16) return false;
    if (pData[0] != 0xFF || pData[1] != 0xD8) return false;

    size_t pos = 2;
    while (pos + 4 <= dataLen) {
        if (pData[pos] != 0xFF) break;
        unsigned char marker = pData[pos + 1];
        if (marker == 0xDA || marker == 0xD9) break;

        size_t len = ((size_t)pData[pos + 2] << 8) | pData[pos + 3];
        if (len < 2) break;

        if (marker == 0xE1) { // APP1 segment
            if (pos + 2 + len <= dataLen && len > 8) {
                const char* exifData = (const char*)(pData + pos + 4);
                size_t segLen = len - 2;
                if (memcmp(exifData, "Exif\0\0", 6) == 0) {
                    for (size_t i = 6; i + 19 <= segLen; ++i) {
                        if (exifData[i + 4] == ':' && exifData[i + 7] == ':' && exifData[i + 10] == ' ' &&
                            exifData[i + 13] == ':' && exifData[i + 16] == ':') {
                            char dateBuf[20];
                            memcpy(dateBuf, exifData + i, 19);
                            dateBuf[19] = '\0';
                            if (ParseExifDateString(dateBuf, outTime)) {
                                return true;
                            }
                        }
                    }
                }
            }
            break;
        }
        pos += 2 + len;
    }
    return false;
}

// Windows Imaging Component (WIC) EXIF Reader with Single-Shot RAM Cache
bool ExtractExifDate(const std::wstring& filePath, SYSTEMTIME& outTime)
{
    // Try single-shot 64KB RAM buffer parse first
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        BYTE headBuf[65536];
        DWORD readBytes = 0;
        if (ReadFile(hFile, headBuf, sizeof(headBuf), &readBytes, NULL) && readBytes > 0) {
            if (FastScanJpegExifDateFromMemory(headBuf, readBytes, outTime)) {
                CloseHandle(hFile);
                return true;
            }
        }
        CloseHandle(hFile);
    }

    // Fallback to WIC COM parser
    IWICImagingFactory* pFactory = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return false;

    IWICBitmapDecoder* pDecoder = NULL;
    hr = pFactory->CreateDecoderFromFilename(filePath.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) {
        pFactory->Release();
        return false;
    }

    IWICBitmapFrameDecode* pFrame = NULL;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) {
        pDecoder->Release();
        pFactory->Release();
        return false;
    }

    IWICMetadataQueryReader* pReader = NULL;
    hr = pFrame->GetMetadataQueryReader(&pReader);
    bool bSuccess = false;

    if (SUCCEEDED(hr)) {
        PROPVARIANT propValue;
        PropVariantInit(&propValue);

        const wchar_t* queries[] = {
            L"/app1/ifd/exif/{ushort=36867}",
            L"/app1/ifd/{ushort=306}",
            L"System.Photo.DateTaken"
        };

        for (const wchar_t* q : queries) {
            if (SUCCEEDED(pReader->GetMetadataByName(q, &propValue))) {
                if (propValue.vt == VT_LPSTR && propValue.pszVal) {
                    if (ParseExifDateString(propValue.pszVal, outTime)) {
                        bSuccess = true;
                    }
                } else if (propValue.vt == VT_LPWSTR && propValue.pwszVal) {
                    char strVal[64];
                    WideCharToMultiByte(CP_UTF8, 0, propValue.pwszVal, -1, strVal, sizeof(strVal), NULL, NULL);
                    if (ParseExifDateString(strVal, outTime)) {
                        bSuccess = true;
                    }
                } else if (propValue.vt == VT_FILETIME) {
                    FILETIME localFt;
                    FileTimeToLocalFileTime(&propValue.filetime, &localFt);
                    FileTimeToSystemTime(&localFt, &outTime);
                    bSuccess = true;
                }
                PropVariantClear(&propValue);
                if (bSuccess) break;
            }
        }
        pReader->Release();
    }

    pFrame->Release();
    pDecoder->Release();
    pFactory->Release();
    return bSuccess;
}

bool ExtractFilenameDate(const std::wstring& filename, SYSTEMTIME& outTime)
{
    static const std::vector<std::wregex> patterns = {
        std::wregex(L"(?:IMG|Screenshot|VID|PXL)?[-_]?(\\d{4})[-_](\\d{2})[-_](\\d{2})", std::regex_constants::icase),
        std::wregex(L"(?:IMG|Screenshot|VID|PXL)?[-_]?(\\d{4})(\\d{2})(\\d{2})", std::regex_constants::icase),
        std::wregex(L"(\\d{2})[-_](\\d{2})[-_](\\d{4})", std::regex_constants::icase)
    };

    for (size_t idx = 0; idx < patterns.size(); ++idx) {
        std::wsmatch match;
        if (std::regex_search(filename, match, patterns[idx])) {
            int y = 0, m = 0, d = 0;
            if (idx == 0 || idx == 1) {
                y = std::stoi(match[1].str());
                m = std::stoi(match[2].str());
                d = std::stoi(match[3].str());
            } else if (idx == 2) {
                m = std::stoi(match[1].str());
                d = std::stoi(match[2].str());
                y = std::stoi(match[3].str());
            }

            if (y >= 1970 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                ZeroMemory(&outTime, sizeof(outTime));
                outTime.wYear = (WORD)y;
                outTime.wMonth = (WORD)m;
                outTime.wDay = (WORD)d;
                outTime.wHour = 12;
                outTime.wMinute = 0;
                outTime.wSecond = 0;
                return true;
            }
        }
    }
    return false;
}

// Zero-I/O Metadata Date: Uses cached FILETIME from RAM
bool ExtractFileMetadataDate(const FileItem& item, SYSTEMTIME& outTime)
{
    FILETIME ft = item.creationTime;
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) {
        ft = item.lastWriteTime;
    }
    if (ft.dwLowDateTime != 0 || ft.dwHighDateTime != 0) {
        FILETIME localFt;
        FileTimeToLocalFileTime(&ft, &localFt);
        return FileTimeToSystemTime(&localFt, &outTime) != 0;
    }
    return false;
}

bool ExtractDate(const FileItem& item, SYSTEMTIME& outTime)
{
    if (ExtractExifDate(item.filePath, outTime)) {
        return true;
    }
    if (ExtractFilenameDate(item.filename, outTime)) {
        return true;
    }
    return ExtractFileMetadataDate(item, outTime);
}

// -------------------------------------------------------------
// GPS Extraction (WIC)
// -------------------------------------------------------------
static double ConvertGpsCoord(const PROPVARIANT& prop)
{
    if ((prop.vt & VT_VECTOR) && (prop.vt & VT_UI4)) {
        if (prop.caul.cElems >= 6) {
            double deg = (double)prop.caul.pElems[0] / (double)prop.caul.pElems[1];
            double min = (double)prop.caul.pElems[2] / (double)prop.caul.pElems[3];
            double sec = (double)prop.caul.pElems[4] / (double)prop.caul.pElems[5];
            return deg + (min / 60.0) + (sec / 3600.0);
        }
    } else if ((prop.vt & VT_VECTOR) && (prop.vt & VT_UI8)) {
        if (prop.cauh.cElems >= 3) {
            double deg = (double)(prop.cauh.pElems[0].QuadPart & 0xFFFFFFFF) / (double)(prop.cauh.pElems[0].QuadPart >> 32);
            double min = (double)(prop.cauh.pElems[1].QuadPart & 0xFFFFFFFF) / (double)(prop.cauh.pElems[1].QuadPart >> 32);
            double sec = (double)(prop.cauh.pElems[2].QuadPart & 0xFFFFFFFF) / (double)(prop.cauh.pElems[2].QuadPart >> 32);
            return deg + (min / 60.0) + (sec / 3600.0);
        }
    }
    return 0.0;
}

bool ExtractGps(const std::wstring& filePath, double& outLat, double& outLon)
{
    IWICImagingFactory* pFactory = NULL;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory)))) {
        return false;
    }

    IWICBitmapDecoder* pDecoder = NULL;
    if (FAILED(pFactory->CreateDecoderFromFilename(filePath.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder))) {
        pFactory->Release();
        return false;
    }

    IWICBitmapFrameDecode* pFrame = NULL;
    if (FAILED(pDecoder->GetFrame(0, &pFrame))) {
        pDecoder->Release();
        pFactory->Release();
        return false;
    }

    IWICMetadataQueryReader* pReader = NULL;
    bool bFound = false;

    if (SUCCEEDED(pFrame->GetMetadataQueryReader(&pReader))) {
        PROPVARIANT propLat, propLatRef, propLon, propLonRef;
        PropVariantInit(&propLat);
        PropVariantInit(&propLatRef);
        PropVariantInit(&propLon);
        PropVariantInit(&propLonRef);

        const wchar_t* latQueries[] = { L"/app1/ifd/gps/{ushort=2}", L"/app1/ifd/gps/subifd:{ushort=2}" };
        const wchar_t* latRefQueries[] = { L"/app1/ifd/gps/{ushort=1}", L"/app1/ifd/gps/subifd:{ushort=1}" };
        const wchar_t* lonQueries[] = { L"/app1/ifd/gps/{ushort=4}", L"/app1/ifd/gps/subifd:{ushort=4}" };
        const wchar_t* lonRefQueries[] = { L"/app1/ifd/gps/{ushort=3}", L"/app1/ifd/gps/subifd:{ushort=3}" };

        bool gotLat = false, gotLon = false;
        char latRef = 'N', lonRef = 'E';

        for (int i = 0; i < 2; ++i) {
            if (!gotLat && SUCCEEDED(pReader->GetMetadataByName(latQueries[i], &propLat))) {
                outLat = ConvertGpsCoord(propLat);
                if (outLat != 0.0) gotLat = true;
            }
            if (SUCCEEDED(pReader->GetMetadataByName(latRefQueries[i], &propLatRef))) {
                if (propLatRef.vt == VT_LPSTR && propLatRef.pszVal) latRef = propLatRef.pszVal[0];
            }
            if (!gotLon && SUCCEEDED(pReader->GetMetadataByName(lonQueries[i], &propLon))) {
                outLon = ConvertGpsCoord(propLon);
                if (outLon != 0.0) gotLon = true;
            }
            if (SUCCEEDED(pReader->GetMetadataByName(lonRefQueries[i], &propLonRef))) {
                if (propLonRef.vt == VT_LPSTR && propLonRef.pszVal) lonRef = propLonRef.pszVal[0];
            }
        }

        if (gotLat && gotLon) {
            if (latRef == 'S' || latRef == 's') outLat = -outLat;
            if (lonRef == 'W' || lonRef == 'w') outLon = -outLon;
            bFound = true;
        }

        PropVariantClear(&propLat);
        PropVariantClear(&propLatRef);
        PropVariantClear(&propLon);
        PropVariantClear(&propLonRef);
        pReader->Release();
    }

    pFrame->Release();
    pDecoder->Release();
    pFactory->Release();
    return bFound;
}

// -------------------------------------------------------------
// HTML Leaflet GPS Map Generation
// -------------------------------------------------------------
bool GenerateAndOpenLeafletMap(const std::vector<GpsPoint>& points)
{
    if (points.empty()) return false;

    double sumLat = 0.0, sumLon = 0.0;
    for (const auto& pt : points) {
        sumLat += pt.latitude;
        sumLon += pt.longitude;
    }
    double centerLat = sumLat / points.size();
    double centerLon = sumLon / points.size();

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring htmlPath = std::wstring(tempPath) + L"picto_photo_map.html";

    std::wofstream out(htmlPath, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;

    out << LR"(<!DOCTYPE html>
<html lang="ko">
<head>
  <meta charset="utf-8"/>
  <title>사진 GPS 지도 보기 - Picto</title>
  <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
  <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
  <style>html, body, #map { height: 100%; margin: 0; padding: 0; font-family: sans-serif; }</style>
</head>
<body>
<div id="map"></div>
<script>
const center = [)" << centerLat << L", " << centerLon << LR"(];
const map = L.map('map').setView(center, 13);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
  maxZoom: 19,
  attribution: '&copy; <a href="https://openstreetmap.org">OpenStreetMap</a> contributors'
}).addTo(map);

const markers = [
)";

    for (size_t i = 0; i < points.size(); ++i) {
        std::filesystem::path p(points[i].filePath);
        std::wstring filename = p.filename().wstring();

        std::wstring safeName;
        for (wchar_t ch : filename) {
            if (ch == L'\\') safeName += L"\\\\";
            else if (ch == L'"') safeName += L"\\\"";
            else safeName += ch;
        }

        out << L"  { lat: " << std::fixed << std::setprecision(6) << points[i].latitude
            << L", lon: " << points[i].longitude
            << L", name: \"" << safeName << L"\" }"
            << (i + 1 < points.size() ? L"," : L"") << L"\n";
    }

    out << LR"(];

markers.forEach(m => {
  const popupHtml = `<b>${m.name}</b><br/>위도: ${m.lat.toFixed(6)}<br/>경도: ${m.lon.toFixed(6)}`;
  L.marker([m.lat, m.lon]).addTo(map).bindPopup(popupHtml).bindTooltip(m.name);
});
</script>
</body>
</html>)";

    out.close();

    ShellExecuteW(NULL, L"open", htmlPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
    return true;
}

// -------------------------------------------------------------
// File Movement & Destination Path Resolution
// -------------------------------------------------------------
bool MoveOrCopyFileToDateFolder(const std::wstring& filePath, const std::wstring& filename,
                               const SYSTEMTIME& dateTaken, const OrganizerOptions& options,
                               std::wstring& outDestination)
{
    wchar_t folderPath[MAX_PATH];
    if (options.organizeByMonth) {
        swprintf_s(folderPath, L"%s\\%04d\\%02d", options.outputFolder.c_str(), dateTaken.wYear, dateTaken.wMonth);
    } else {
        swprintf_s(folderPath, L"%s\\%04d", options.outputFolder.c_str(), dateTaken.wYear);
    }

    SHCreateDirectoryExW(NULL, folderPath, NULL);

    std::filesystem::path srcP(filename);
    std::wstring ext = srcP.extension().wstring();

    wchar_t baseFilename[MAX_PATH];
    swprintf_s(baseFilename, L"%04d%02d%02d_%02d%02d%02d",
        dateTaken.wYear, dateTaken.wMonth, dateTaken.wDay,
        dateTaken.wHour, dateTaken.wMinute, dateTaken.wSecond);

    std::wstring destPath = std::wstring(folderPath) + L"\\" + baseFilename + ext;
    int counter = 1;
    while (PathFileExistsW(destPath.c_str())) {
        wchar_t disambig[MAX_PATH];
        swprintf_s(disambig, L"%s\\%s_%03d%s", folderPath, baseFilename, counter++, ext.c_str());
        destPath = disambig;
    }

    outDestination = destPath;

    if (options.copyFiles) {
        return CopyFileW(filePath.c_str(), destPath.c_str(), FALSE) != 0;
    } else {
        return MoveFileExW(filePath.c_str(), destPath.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) != 0;
    }
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
                    g_stats.processedFiles++;
                } else {
                    g_stats.errors++;
                    g_stats.processedFiles++;
                    EnqueueLog(L"파일 이동/복사 실패: " + item.filename);
                }
            } else {
                g_stats.errors++;
                g_stats.processedFiles++;
                EnqueueLog(L"날짜 추출 실패: " + item.filename);
            }
        }
        catch (...) {
            g_stats.errors++;
            g_stats.processedFiles++;
            EnqueueLog(L"파일 처리 예외: " + item.filename);
        }
    }

    CoUninitialize();
}

// -------------------------------------------------------------
// Directory Scan, RAM Bucketing, and Worker Launch
// -------------------------------------------------------------
void ScanAndProcessThread()
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    EnqueueLog(L"입력 폴더 검색 시작: " + g_currentOptions.inputFolder);

    static const std::vector<std::wstring> validExts = {
        L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tiff", L".tif", L".webp", L".heic", L".heif",
        L".mp4", L".avi", L".mov", L".mkv", L".wmv", L".flv", L".webm", L".m4v", L".3gp"
    };

    g_fileList.clear();

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

    if (g_bCancelRequested) {
        PostMessageW(g_hMainWnd, WM_APP_WORK_FINISHED, 0, 0);
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
        PostMessageW(g_hMainWnd, WM_APP_WORK_FINISHED, 0, 0);
        CoUninitialize();
        return;
    }

    // RAM Phase 1: High-Speed File Size Bucketing
    // If a file size is unique across the entire directory, it CANNOT be a duplicate!
    // This eliminates hash computations for 90-98% of all files without reading a single byte!
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

    // Launch worker threads
    int numWorkers = g_currentOptions.threadCount;
    wchar_t startMsg[128];
    swprintf_s(startMsg, L"%d개 작업자 스레드로 고속 처리 시작!", numWorkers);
    EnqueueLog(startMsg);

    g_workerThreads.clear();
    for (int i = 0; i < numWorkers; ++i) {
        g_workerThreads.emplace_back(WorkerThreadProc, i + 1);
    }

    // Wait for all workers to complete
    for (auto& th : g_workerThreads) {
        if (th.joinable()) th.join();
    }
    g_workerThreads.clear();

    PostMessageW(g_hMainWnd, WM_APP_WORK_FINISHED, 0, 0);
    CoUninitialize();
}

// -------------------------------------------------------------
// UI Layout and Control Creation
// -------------------------------------------------------------
void CreateMainControls(HWND hWnd)
{
    // 1. Folder Settings Group
    g_hGrpFolder = CreateWindowExW(0, L"BUTTON", L"폴더 설정",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, (HMENU)IDC_GRP_FOLDER, g_hInst, NULL);
    g_hLblInput = CreateWindowExW(0, L"STATIC", L"입력 폴더:",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_LBL_INPUT, g_hInst, NULL);
    g_hEditInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_INPUT, g_hInst, NULL);
    g_hBtnBrowseInput = CreateWindowExW(0, L"BUTTON", L"선택",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_BROWSE_INPUT, g_hInst, NULL);

    g_hLblOutput = CreateWindowExW(0, L"STATIC", L"출력 폴더:",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_LBL_OUTPUT, g_hInst, NULL);
    g_hEditOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_OUTPUT, g_hInst, NULL);
    g_hBtnBrowseOutput = CreateWindowExW(0, L"BUTTON", L"선택",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_BROWSE_OUTPUT, g_hInst, NULL);

    // 2. Performance Settings Group
    g_hGrpPerf = CreateWindowExW(0, L"BUTTON", L"성능 설정",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, (HMENU)IDC_GRP_PERF, g_hInst, NULL);
    g_hLblThreads = CreateWindowExW(0, L"STATIC", L"스레드 수:",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_LBL_THREADS, g_hInst, NULL);
    g_hEditThreads = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER, 0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_THREADS, g_hInst, NULL);
    g_hSpinThreads = CreateWindowExW(0, UPDOWN_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_AUTOBUDDY | UDS_ARROWKEYS,
        0, 0, 0, 0, hWnd, (HMENU)IDC_SPIN_THREADS, g_hInst, NULL);

    unsigned int cpuCount = std::thread::hardware_concurrency();
    if (cpuCount == 0) cpuCount = 4;
    SendMessageW(g_hSpinThreads, UDM_SETRANGE32, 1, cpuCount * 2);
    SendMessageW(g_hSpinThreads, UDM_SETPOS32, 0, cpuCount);

    wchar_t cpuInfoBuf[64];
    swprintf_s(cpuInfoBuf, L"(시스템 감지: %u 코어/스레드)", cpuCount);
    g_hLblCpuInfo = CreateWindowExW(0, L"STATIC", cpuInfoBuf,
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_CPU_INFO, g_hInst, NULL);

    // 3. Processing Options Group
    g_hGrpOpts = CreateWindowExW(0, L"BUTTON", L"처리 옵션",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, (HMENU)IDC_GRP_OPTS, g_hInst, NULL);
    g_hChkOrgMonth = CreateWindowExW(0, L"BUTTON", L"월별 폴더 생성",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_ORG_MONTH, g_hInst, NULL);
    g_hChkCopyFiles = CreateWindowExW(0, L"BUTTON", L"파일 복사 (이동 대신)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_COPY_FILES, g_hInst, NULL);
    g_hChkRemoveDups = CreateWindowExW(0, L"BUTTON", L"동일 중복 파일 삭제 (고속 RAM 크기 분류 & MMF 3단계 검증)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_REMOVE_DUPS, g_hInst, NULL);

    Button_SetCheck(g_hChkRemoveDups, BST_CHECKED);

    // 4. Action Buttons
    g_hBtnStart = CreateWindowExW(0, L"BUTTON", L"🚀 고속 파일 정리 시작",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_START, g_hInst, NULL);
    g_hBtnCancel = CreateWindowExW(0, L"BUTTON", L"❌ 취소",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_CANCEL, g_hInst, NULL);
    g_hBtnDupClean = CreateWindowExW(0, L"BUTTON", L"📁 중복 정리 (전용)",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_DUP_CLEAN, g_hInst, NULL);
    g_hBtnResizedClean = CreateWindowExW(0, L"BUTTON", L"🖼️ 축소 사진 정리 (전용)",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_RESIZED_CLEAN, g_hInst, NULL);
    g_hBtnGpsMap = CreateWindowExW(0, L"BUTTON", L"🗺️ GPS 지도",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_GPS_MAP, g_hInst, NULL);

    // 5. Progress and Statistics Group
    g_hGrpProgress = CreateWindowExW(0, L"BUTTON", L"진행 상황",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, (HMENU)IDC_GRP_PROGRESS, g_hInst, NULL);
    g_hLblStatus = CreateWindowExW(0, L"STATIC", L"대기 중...",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_STATUS, g_hInst, NULL);
    g_hProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, hWnd, (HMENU)IDC_PROGRESS_BAR, g_hInst, NULL);

    g_hLblProcessed = CreateWindowExW(0, L"STATIC", L"처리된 파일: 0",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_STAT_PROCESSED, g_hInst, NULL);
    g_hLblMoved = CreateWindowExW(0, L"STATIC", L"이동된 파일: 0",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_STAT_MOVED, g_hInst, NULL);
    g_hLblDups = CreateWindowExW(0, L"STATIC", L"중복 제거: 0",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_STAT_DUPS, g_hInst, NULL);
    g_hLblSpeed = CreateWindowExW(0, L"STATIC", L"처리 속도: 0.0 파일/초",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_STAT_SPEED, g_hInst, NULL);

    // 6. Real-time Log Group
    g_hGrpLog = CreateWindowExW(0, L"BUTTON", L"실시간 처리 로그",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, (HMENU)IDC_GRP_LOG, g_hInst, NULL);
    g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_LOG, g_hInst, NULL);

    CreateDpiFonts(g_currentDpi);
    ApplyFontsToControls(hWnd);
}

void UpdateLayout(HWND hWnd, int clientWidth, int clientHeight)
{
    if (clientWidth <= 0 || clientHeight <= 0) return;

    UINT dpi = g_currentDpi;
    int margin = ScaleDpi(15, dpi);
    int pad = ScaleDpi(8, dpi);
    int rowHeight = ScaleDpi(26, dpi);
    int btnWidth = ScaleDpi(80, dpi);
    int lblWidth = ScaleDpi(85, dpi);

    int contentW = clientWidth - (margin * 2);
    int curY = margin;

    // 1. Folder Settings Group
    int grpFolderH = ScaleDpi(105, dpi);
    SetWindowPos(g_hGrpFolder, NULL, margin, curY, contentW, grpFolderH, SWP_NOZORDER);

    int innerY = curY + ScaleDpi(24, dpi);
    int editW = contentW - lblWidth - btnWidth - (pad * 4);

    // Row 1: Input folder
    SetWindowPos(g_hLblInput, NULL, margin + pad * 2, innerY + ScaleDpi(3, dpi), lblWidth, rowHeight, SWP_NOZORDER);
    SetWindowPos(g_hEditInput, NULL, margin + pad * 2 + lblWidth, innerY, editW, rowHeight, SWP_NOZORDER);
    SetWindowPos(g_hBtnBrowseInput, NULL, margin + contentW - pad * 2 - btnWidth, innerY, btnWidth, rowHeight, SWP_NOZORDER);

    // Row 2: Output folder
    innerY += rowHeight + pad;
    SetWindowPos(g_hLblOutput, NULL, margin + pad * 2, innerY + ScaleDpi(3, dpi), lblWidth, rowHeight, SWP_NOZORDER);
    SetWindowPos(g_hEditOutput, NULL, margin + pad * 2 + lblWidth, innerY, editW, rowHeight, SWP_NOZORDER);
    SetWindowPos(g_hBtnBrowseOutput, NULL, margin + contentW - pad * 2 - btnWidth, innerY, btnWidth, rowHeight, SWP_NOZORDER);

    curY += grpFolderH + pad;

    // 2. Performance Settings Group
    int grpPerfH = ScaleDpi(68, dpi);
    SetWindowPos(g_hGrpPerf, NULL, margin, curY, contentW, grpPerfH, SWP_NOZORDER);

    innerY = curY + ScaleDpi(25, dpi);
    int spinW = ScaleDpi(75, dpi);
    SetWindowPos(g_hLblThreads, NULL, margin + pad * 2, innerY + ScaleDpi(3, dpi), lblWidth, rowHeight, SWP_NOZORDER);
    SetWindowPos(g_hEditThreads, NULL, margin + pad * 2 + lblWidth, innerY, spinW, rowHeight, SWP_NOZORDER);
    SetWindowPos(g_hLblCpuInfo, NULL, margin + pad * 3 + lblWidth + spinW, innerY + ScaleDpi(3, dpi), ScaleDpi(260, dpi), rowHeight, SWP_NOZORDER);

    curY += grpPerfH + pad;

    // 3. Processing Options Group
    int grpOptsH = ScaleDpi(95, dpi);
    SetWindowPos(g_hGrpOpts, NULL, margin, curY, contentW, grpOptsH, SWP_NOZORDER);

    innerY = curY + ScaleDpi(26, dpi);
    int optColW = (contentW - pad * 4) / 2;
    SetWindowPos(g_hChkOrgMonth, NULL, margin + pad * 2, innerY, optColW, rowHeight, SWP_NOZORDER);
    SetWindowPos(g_hChkCopyFiles, NULL, margin + pad * 2 + optColW, innerY, optColW, rowHeight, SWP_NOZORDER);

    innerY += rowHeight + ScaleDpi(4, dpi);
    SetWindowPos(g_hChkRemoveDups, NULL, margin + pad * 2, innerY, contentW - pad * 4, rowHeight, SWP_NOZORDER);

    curY += grpOptsH + pad;

    // 4. Action Buttons (5 buttons)
    int actBtnH = ScaleDpi(38, dpi);
    int availW = contentW - (pad * 4);
    int startBtnW = (availW * 26) / 100;
    int cancelBtnW = (availW * 12) / 100;
    int dupBtnW = (availW * 22) / 100;
    int cleanBtnW = (availW * 24) / 100;
    int mapBtnW = availW - startBtnW - cancelBtnW - dupBtnW - cleanBtnW;

    int curX = margin;
    SetWindowPos(g_hBtnStart, NULL, curX, curY, startBtnW, actBtnH, SWP_NOZORDER);
    curX += startBtnW + pad;
    SetWindowPos(g_hBtnCancel, NULL, curX, curY, cancelBtnW, actBtnH, SWP_NOZORDER);
    curX += cancelBtnW + pad;
    SetWindowPos(g_hBtnDupClean, NULL, curX, curY, dupBtnW, actBtnH, SWP_NOZORDER);
    curX += dupBtnW + pad;
    SetWindowPos(g_hBtnResizedClean, NULL, curX, curY, cleanBtnW, actBtnH, SWP_NOZORDER);
    curX += cleanBtnW + pad;
    SetWindowPos(g_hBtnGpsMap, NULL, curX, curY, mapBtnW, actBtnH, SWP_NOZORDER);

    curY += actBtnH + pad;

    // 5. Progress and Statistics Group
    int grpProgH = ScaleDpi(110, dpi);
    SetWindowPos(g_hGrpProgress, NULL, margin, curY, contentW, grpProgH, SWP_NOZORDER);

    innerY = curY + ScaleDpi(24, dpi);
    SetWindowPos(g_hLblStatus, NULL, margin + pad * 2, innerY, contentW - pad * 4, ScaleDpi(20, dpi), SWP_NOZORDER);

    innerY += ScaleDpi(22, dpi);
    int progBarH = ScaleDpi(22, dpi);
    SetWindowPos(g_hProgressBar, NULL, margin + pad * 2, innerY, contentW - pad * 4, progBarH, SWP_NOZORDER);

    innerY += progBarH + ScaleDpi(8, dpi);
    int statColW = (contentW - pad * 4) / 4;
    SetWindowPos(g_hLblProcessed, NULL, margin + pad * 2, innerY, statColW, ScaleDpi(20, dpi), SWP_NOZORDER);
    SetWindowPos(g_hLblMoved, NULL, margin + pad * 2 + statColW, innerY, statColW, ScaleDpi(20, dpi), SWP_NOZORDER);
    SetWindowPos(g_hLblDups, NULL, margin + pad * 2 + statColW * 2, innerY, statColW, ScaleDpi(20, dpi), SWP_NOZORDER);
    SetWindowPos(g_hLblSpeed, NULL, margin + pad * 2 + statColW * 3, innerY, statColW, ScaleDpi(20, dpi), SWP_NOZORDER);

    curY += grpProgH + pad;

    // 6. Real-time Log Group (Fills the remaining height)
    int logGrpH = clientHeight - curY - margin;
    if (logGrpH > ScaleDpi(80, dpi)) {
        SetWindowPos(g_hGrpLog, NULL, margin, curY, contentW, logGrpH, SWP_NOZORDER);
        SetWindowPos(g_hEditLog, NULL, margin + pad * 2, curY + ScaleDpi(24, dpi),
            contentW - pad * 4, logGrpH - ScaleDpi(32, dpi), SWP_NOZORDER);
    }
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

    BOOL bSuccess = FALSE;
    int threads = (int)SendMessageW(g_hSpinThreads, UDM_GETPOS32, 0, (LPARAM)&bSuccess);
    if (!bSuccess || threads < 1) threads = 4;

    g_currentOptions.inputFolder = strIn;
    g_currentOptions.outputFolder = strOut;
    g_currentOptions.threadCount = threads;
    g_currentOptions.organizeByMonth = (Button_GetCheck(g_hChkOrgMonth) == BST_CHECKED);
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

// -------------------------------------------------------------
// GPS Map Dialog Procedure
// -------------------------------------------------------------
static GpsPoint g_singleGpsPoint;

INT_PTR CALLBACK GpsMapDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
    {
        HWND hTab = GetDlgItem(hDlg, IDC_TAB_GPS);
        TCITEMW tie = { 0 };
        tie.mask = TCIF_TEXT;
        tie.pszText = (LPWSTR)L"단일 파일";
        TabCtrl_InsertItem(hTab, 0, &tie);
        tie.pszText = (LPWSTR)L"여러 파일 (폴더)";
        TabCtrl_InsertItem(hTab, 1, &tie);

        HWND hList = GetDlgItem(hDlg, IDC_GPS_BATCH_LIST);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        lvc.iSubItem = 0;
        lvc.pszText = (LPWSTR)L"파일명";
        lvc.cx = 150;
        ListView_InsertColumn(hList, 0, &lvc);

        lvc.iSubItem = 1;
        lvc.pszText = (LPWSTR)L"위도";
        lvc.cx = 90;
        ListView_InsertColumn(hList, 1, &lvc);

        lvc.iSubItem = 2;
        lvc.pszText = (LPWSTR)L"경도";
        lvc.cx = 90;
        ListView_InsertColumn(hList, 2, &lvc);

        lvc.iSubItem = 3;
        lvc.pszText = (LPWSTR)L"전체 경로";
        lvc.cx = 250;
        ListView_InsertColumn(hList, 3, &lvc);

        ShowWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_GRP), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_EDIT), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_BROWSE), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_INFO), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_VIEW), SW_SHOW);

        ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_GRP), SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_EDIT), SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_BROWSE), SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_LIST), SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_SCAN), SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), SW_HIDE);

        return (INT_PTR)TRUE;
    }

    case WM_NOTIFY:
    {
        NMHDR* pnm = (NMHDR*)lParam;
        if (pnm->idFrom == IDC_TAB_GPS && pnm->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(pnm->hwndFrom);
            bool isSingle = (sel == 0);

            int showSingle = isSingle ? SW_SHOW : SW_HIDE;
            int showBatch = isSingle ? SW_HIDE : SW_SHOW;

            ShowWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_GRP), showSingle);
            ShowWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_EDIT), showSingle);
            ShowWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_BROWSE), showSingle);
            ShowWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_INFO), showSingle);
            ShowWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_VIEW), showSingle);

            ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_GRP), showBatch);
            ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_EDIT), showBatch);
            ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_BROWSE), showBatch);
            ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_LIST), showBatch);
            ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_SCAN), showBatch);
            ShowWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), showBatch);
        }
        break;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_GPS_SINGLE_BROWSE) {
            std::wstring filePath;
            if (BrowseFile(hDlg, filePath, L"이미지 파일 (*.jpg;*.jpeg;*.png;*.tiff;*.tif;*.heic)\0*.jpg;*.jpeg;*.png;*.tiff;*.tif;*.heic\0모든 파일 (*.*)\0*.*\0")) {
                SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_SINGLE_EDIT), filePath.c_str());
                double lat = 0.0, lon = 0.0;
                if (ExtractGps(filePath, lat, lon)) {
                    g_singleGpsPoint.filePath = filePath;
                    g_singleGpsPoint.latitude = lat;
                    g_singleGpsPoint.longitude = lon;

                    wchar_t infoBuf[128];
                    swprintf_s(infoBuf, L"GPS 정보: 위도 %.6f, 경도 %.6f", lat, lon);
                    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_SINGLE_INFO), infoBuf);
                    EnableWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_VIEW), TRUE);
                } else {
                    SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_SINGLE_INFO), L"GPS 정보: 없음");
                    EnableWindow(GetDlgItem(hDlg, IDC_GPS_SINGLE_VIEW), FALSE);
                }
            }
        }
        else if (id == IDC_GPS_SINGLE_VIEW) {
            std::vector<GpsPoint> pts = { g_singleGpsPoint };
            GenerateAndOpenLeafletMap(pts);
        }
        else if (id == IDC_GPS_BATCH_BROWSE) {
            std::wstring folderPath;
            if (BrowseFolder(hDlg, folderPath)) {
                SetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_EDIT), folderPath.c_str());
                ListView_DeleteAllItems(GetDlgItem(hDlg, IDC_GPS_BATCH_LIST));
                g_batchGpsData.clear();
                EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), FALSE);
            }
        }
        else if (id == IDC_GPS_BATCH_SCAN) {
            wchar_t folderPath[MAX_PATH];
            GetWindowTextW(GetDlgItem(hDlg, IDC_GPS_BATCH_EDIT), folderPath, MAX_PATH);
            if (folderPath[0] == L'\0' || !PathIsDirectoryW(folderPath)) {
                MessageBoxW(hDlg, L"폴더를 먼저 선택해주세요.", L"경고", MB_ICONWARNING);
                break;
            }

            HWND hList = GetDlgItem(hDlg, IDC_GPS_BATCH_LIST);
            ListView_DeleteAllItems(hList);
            g_batchGpsData.clear();
            EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_SCAN), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), FALSE);
            SetCursor(LoadCursor(NULL, IDC_WAIT));

            if (g_gpsScanThread.joinable()) {
                g_gpsScanThread.join();
            }

            std::wstring scanDir = folderPath;
            g_gpsScanThread = std::thread([hDlg, hList, scanDir]() {
                CoInitializeEx(NULL, COINIT_MULTITHREADED);
                std::vector<GpsPoint> localPoints;

                static const std::vector<std::wstring> imgExts = {
                    L".jpg", L".jpeg", L".png", L".tiff", L".tif", L".heic", L".heif"
                };

                try {
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(
                        scanDir, std::filesystem::directory_options::skip_permission_denied)) {
                        if (entry.is_regular_file()) {
                            std::wstring ext = entry.path().extension().wstring();
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

                            for (const auto& ve : imgExts) {
                                if (ext == ve) {
                                    double lat = 0.0, lon = 0.0;
                                    std::wstring pathStr = entry.path().wstring();
                                    if (ExtractGps(pathStr, lat, lon)) {
                                        localPoints.push_back({ pathStr, lat, lon });
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
                catch (...) {}

                CoUninitialize();

                PostMessageW(hDlg, WM_APP_SCAN_FINISHED, (WPARAM)new std::vector<GpsPoint>(std::move(localPoints)), 0);
            });
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
                GenerateAndOpenLeafletMap(viewPoints);
            }
        }
        else if (id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_APP_SCAN_FINISHED:
    {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_SCAN), TRUE);

        std::vector<GpsPoint>* pPoints = (std::vector<GpsPoint>*)wParam;
        if (pPoints) {
            g_batchGpsData = std::move(*pPoints);
            delete pPoints;
        }

        HWND hList = GetDlgItem(hDlg, IDC_GPS_BATCH_LIST);
        SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(hList);

        for (size_t i = 0; i < g_batchGpsData.size(); ++i) {
            const auto& pt = g_batchGpsData[i];
            std::filesystem::path p(pt.filePath);
            std::wstring fn = p.filename().wstring();

            LVITEMW lvi = { 0 };
            lvi.mask = LVIF_TEXT;
            lvi.iItem = (int)i;
            lvi.iSubItem = 0;
            lvi.pszText = (LPWSTR)fn.c_str();
            ListView_InsertItem(hList, &lvi);

            wchar_t coordBuf[64];
            swprintf_s(coordBuf, L"%.6f", pt.latitude);
            ListView_SetItemText(hList, (int)i, 1, coordBuf);

            swprintf_s(coordBuf, L"%.6f", pt.longitude);
            ListView_SetItemText(hList, (int)i, 2, coordBuf);

            ListView_SetItemText(hList, (int)i, 3, (LPWSTR)pt.filePath.c_str());
        }

        SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hList, NULL, TRUE);

        if (!g_batchGpsData.empty()) {
            EnableWindow(GetDlgItem(hDlg, IDC_GPS_BATCH_VIEWALL), TRUE);
        }
        break;
    }

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return (INT_PTR)TRUE;
    }

    return (INT_PTR)FALSE;
}

// -------------------------------------------------------------
// Resized Image Cleaner Dialog Procedure
// Uses stb_image + WIC + 2D-DCT pHash for high-performance detection
// -------------------------------------------------------------
struct ResizedPairItem {
    int id = 0;
    std::wstring resizedPath;
    std::wstring resizedName;
    int resizedW = 0;
    int resizedH = 0;
    uint64_t resizedSize = 0;

    std::wstring origPath;
    std::wstring origName;
    int origW = 0;
    int origH = 0;
    uint64_t origSize = 0;

    int hammingDist = 0;
    double similarityPct = 0.0;
    bool isChecked = true;

    // Fast in-memory cached display strings & search key (Zero-lag sorting/filtering)
    std::wstring strResDim;
    std::wstring strResSize;
    std::wstring strOrigDim;
    std::wstring strOrigSize;
    std::wstring strSimilarity;
    std::wstring resizedDir;
    std::wstring origDir;
    std::wstring lowerFilterKey;
};

static std::vector<ResizedPairItem> g_resizedMatches;
static std::atomic<bool> g_bResizedScanning{ false };
static std::atomic<bool> g_bResizedCancel{ false };
static std::thread g_resizedScanThread;
static int g_resizedSortCol = 0;
static bool g_resizedSortAsc = true;
static bool g_bResizedUpdating = false;
static ImagePreviewRenderer g_resizedPreviewResized;
static ImagePreviewRenderer g_resizedPreviewOrig;

// Subclass procedure to intercept Enter key in search edit boxes
static LRESULT CALLBACK FilterEditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
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
            return 0; // Suppress beep
        }
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, FilterEditSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static std::vector<std::wstring> ParseExcludeTokens(const std::wstring& input)
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

static std::vector<int> g_resizedVisibleIndices;

void UpdateResizedSummary(HWND hDlg)
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

void RefreshResizedListView(HWND hDlg)
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
        GetWindowTextW(g_hEditInput, inPath, MAX_PATH);
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

        // Status row & Progress
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_STATIC_STATUS), NULL, margin, 40, leftW - btnW - pad, 18, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_RESIZED_BTN_STOP), NULL, margin + leftW - btnW, 37, btnW, rowH, SWP_NOZORDER);
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
            wchar_t sbuf[128];
            swprintf_s(sbuf, L"이미지 pHash 및 해상도 분석 중: %d / %d (%d%%)", current, total, pct);
            SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_STATIC_STATUS), sbuf);
        }
        break;
    }

    case WM_APP_RESIZED_FINISHED:
    {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_BTN_SCAN), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_RESIZED_BTN_STOP), FALSE);
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

        wchar_t statusBuf[128];
        swprintf_s(statusBuf, L"탐색 완료: %d쌍의 축소본이 감지되었습니다.", (int)g_resizedMatches.size());
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
            SetCursor(LoadCursor(NULL, IDC_WAIT));
            SetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_STATIC_STATUS), L"폴더 내 이미지 파일 탐색 중...");
            SendMessageW(GetDlgItem(hDlg, IDC_RESIZED_PROGRESS), PBM_SETPOS, 0, 0);

            g_bResizedCancel = false;
            g_bResizedScanning = true;

            std::wstring scanDir = folderPath;
            wchar_t exBuf[512] = { 0 };
            GetWindowTextW(GetDlgItem(hDlg, IDC_RESIZED_EDIT_EXCLUDE), exBuf, 512);
            std::vector<std::wstring> excludeTokens = ParseExcludeTokens(exBuf);

            g_resizedScanThread = std::thread([hDlg, scanDir, excludeTokens]() {
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

                unsigned int numThreads = std::thread::hardware_concurrency();
                if (numThreads == 0) numThreads = 4;
                if (numThreads > 8) numThreads = 8;

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

                                // [중요 개발 규칙] 축소이미지와 원본이미지의 크기 차이가 2배 이상 나지 않으면 축소이미지가 아니므로 제외
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

// -------------------------------------------------------------
// Duplicate File Cleaner Dialog Procedure
// Uses 3-Stage MMF Zero-Copy Comparison Engine
// -------------------------------------------------------------
struct DupPairItem {
    int id = 0;
    std::wstring dupPath;
    std::wstring dupName;
    uint64_t fileSize = 0;

    std::wstring origPath;
    std::wstring origName;

    bool isChecked = true;

    // Fast in-memory cached display string & search key (Zero-lag sorting/filtering)
    std::wstring strFileSize;
    std::wstring dupDir;
    std::wstring origDir;
    std::wstring lowerFilterKey;
};

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

void UpdateDupSummary(HWND hDlg)
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

void RefreshDupListView(HWND hDlg)
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
        GetWindowTextW(g_hEditInput, inPath, MAX_PATH);
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

        // Status row & Progress
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_STATIC_STATUS), NULL, margin, 40, leftW - btnW - pad, 18, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hDlg, IDC_DUP_BTN_STOP), NULL, margin + leftW - btnW, 37, btnW, rowH, SWP_NOZORDER);
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
            wchar_t sbuf[128];
            swprintf_s(sbuf, L"후보 파일 샘플 해시 및 무결성 분석 중: %d / %d (%d%%)", current, total, pct);
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_STATIC_STATUS), sbuf);
        }
        break;
    }

    case WM_APP_DUP_FINISHED:
    {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        EnableWindow(GetDlgItem(hDlg, IDC_DUP_BTN_SCAN), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_DUP_BTN_STOP), FALSE);
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

        wchar_t statusBuf[128];
        swprintf_s(statusBuf, L"탐색 완료: %d개의 중복 파일이 감지되었습니다.", (int)g_dupMatches.size());
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
            SetCursor(LoadCursor(NULL, IDC_WAIT));
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_STATIC_STATUS), L"폴더 내 파일 목록 검색 중...");
            SendMessageW(GetDlgItem(hDlg, IDC_DUP_PROGRESS), PBM_SETPOS, 0, 0);

            g_bDupCancel = false;
            g_bDupScanning = true;

            std::wstring scanDir = folderPath;
            wchar_t exBuf[512] = { 0 };
            GetWindowTextW(GetDlgItem(hDlg, IDC_DUP_EDIT_EXCLUDE), exBuf, 512);
            std::vector<std::wstring> excludeTokens = ParseExcludeTokens(exBuf);

            g_dupScanThread = std::thread([hDlg, scanDir, excludeTokens]() {
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

                // Stage 2 & 3: Fast Sample Hash + MMF Zero-Copy Verification
                for (auto& bucket : candidateBuckets) {
                    if (g_bDupCancel) break;

                    std::unordered_map<uint64_t, std::vector<FileScanEntry>> hashGroups;
                    for (auto& item : bucket) {
                        if (g_bDupCancel) break;
                        uint64_t sHash = CalculateFastSampleHash(item.path, item.fileSize);
                        hashGroups[sHash].push_back(std::move(item));

                        size_t done = processedFiles.fetch_add(1) + 1;
                        if (done % 5 == 0 || done == candidateCount) {
                            PostMessageW(hDlg, WM_APP_DUP_PROGRESS, (WPARAM)done, (LPARAM)candidateCount);
                        }
                    }

                    for (auto& hg : hashGroups) {
                        if (hg.second.size() > 1) {
                            const auto& orig = hg.second[0];
                            for (size_t k = 1; k < hg.second.size(); ++k) {
                                if (g_bDupCancel) break;
                                const auto& dupCandidate = hg.second[k];
                                if (AreFilesContentIdentical(orig.path, dupCandidate.path, orig.fileSize)) {
                                    DupPairItem match;
                                    match.id = (int)pFinalList->size();
                                    match.dupPath = dupCandidate.path;
                                    match.dupName = dupCandidate.name;
                                    match.fileSize = dupCandidate.fileSize;
                                    match.origPath = orig.path;
                                    match.origName = orig.name;
                                    match.isChecked = true;
                                    pFinalList->push_back(std::move(match));
                                }
                            }
                        }
                    }
                }

                CoUninitialize();
                PostMessageW(hDlg, WM_APP_DUP_FINISHED, (WPARAM)pFinalList, 0);
            });
        }
        else if (id == IDC_DUP_BTN_STOP) {
            g_bDupCancel = true;
            SetWindowTextW(GetDlgItem(hDlg, IDC_DUP_STATIC_STATUS), L"중지 요청 중...");
            EnableWindow(GetDlgItem(hDlg, IDC_DUP_BTN_STOP), FALSE);
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
        pMMI->ptMinTrackSize.x = ScaleDpi(750, dpi);
        pMMI->ptMinTrackSize.y = ScaleDpi(580, dpi);
        break;
    }

    case WM_TIMER:
        if (wParam == TIMER_STATS_UPDATE) {
            UpdateStatsDisplay(hWnd);
        }
        break;

    case WM_APP_WORK_FINISHED:
        OnProcessingFinished(hWnd);
        break;

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_BROWSE_INPUT) {
            std::wstring path;
            if (BrowseFolder(hWnd, path)) {
                SetWindowTextW(g_hEditInput, path.c_str());
            }
        }
        else if (id == IDC_BTN_BROWSE_OUTPUT) {
            std::wstring path;
            if (BrowseFolder(hWnd, path)) {
                SetWindowTextW(g_hEditOutput, path.c_str());
            }
        }
        else if (id == IDC_BTN_START) {
            StartOrganizing(hWnd);
        }
        else if (id == IDC_BTN_CANCEL) {
            CancelOrganizing();
        }
        else if (id == IDC_BTN_DUP_CLEAN) {
            DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_DUP_CLEANER), hWnd, DupCleanerDlgProc, 0);
        }
        else if (id == IDC_BTN_RESIZED_CLEAN) {
            DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_RESIZED_CLEANER), hWnd, ResizedCleanerDlgProc, 0);
        }
        else if (id == IDC_BTN_GPS_MAP) {
            DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_GPSMAP), hWnd, GpsMapDlgProc, 0);
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
        if (g_hFontNormal) DeleteObject(g_hFontNormal);
        if (g_hFontBold) DeleteObject(g_hFontBold);
        if (g_hFontLog) DeleteObject(g_hFontLog);
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

    int initW = 920;
    int initH = 740;

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
                SetWindowTextW(g_hEditInput, argv[++i]);
            } else if (_wcsicmp(argv[i], L"--output") == 0 && i + 1 < argc) {
                SetWindowTextW(g_hEditOutput, argv[++i]);
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

    if (g_gpsScanThread.joinable()) {
        g_gpsScanThread.join();
    }

    CoUninitialize();
    CloseHandle(hMutex);

    return (int)msg.wParam;
}
