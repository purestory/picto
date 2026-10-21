#include "exif_modifier.h"
#include "../common/utils.h"
#include "../organizer/date_extractor.h"

#include <shlwapi.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <filesystem>
#include <cstdio>
#include <vector>

#pragma comment(lib, "propsys.lib")

std::wstring FormatSystemTime(const SYSTEMTIME& st)
{
    if (st.wYear == 0) return L"-";
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

SYSTEMTIME AddTimeOffset(const SYSTEMTIME& baseTime, int days, int hours, int minutes, int seconds)
{
    FILETIME ft;
    if (!SystemTimeToFileTime(&baseTime, &ft)) {
        return baseTime;
    }

    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    int64_t totalSec = (int64_t)days * 86400LL + (int64_t)hours * 3600LL + (int64_t)minutes * 60LL + (int64_t)seconds;
    int64_t delta100ns = totalSec * 10000000LL;

    if (delta100ns < 0 && (uint64_t)(-delta100ns) > uli.QuadPart) {
        uli.QuadPart = 0;
    } else {
        uli.QuadPart += delta100ns;
    }

    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;

    SYSTEMTIME res;
    if (!FileTimeToSystemTime(&ft, &res)) {
        return baseTime;
    }
    return res;
}

bool ParseDateFromFileName(const std::wstring& fileName, SYSTEMTIME& outTime)
{
    return ExtractFilenameDate(fileName, outTime);
}

bool ReadImageDateInfo(const std::wstring& filePath, ImageDateInfo& outInfo)
{
    outInfo.filePath = filePath;
    std::filesystem::path fp(filePath);
    outInfo.fileName = fp.filename().wstring();
    outInfo.folderDir = fp.parent_path().wstring();

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &fad)) {
        outInfo.fileSize = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        FILETIME localFt;
        FileTimeToLocalFileTime(&fad.ftLastWriteTime, &localFt);
        FileTimeToSystemTime(&localFt, &outInfo.fileTime);
    } else {
        outInfo.fileSize = 0;
        ZeroMemory(&outInfo.fileTime, sizeof(outInfo.fileTime));
    }

    SYSTEMTIME exTime = { 0 };
    if (ExtractExifDate(filePath, exTime)) {
        outInfo.hasExif = true;
        outInfo.exifTime = exTime;
        outInfo.targetTime = exTime;
    } else {
        outInfo.hasExif = false;
        ZeroMemory(&outInfo.exifTime, sizeof(outInfo.exifTime));
        outInfo.targetTime = outInfo.fileTime;
    }

    outInfo.strCurrentExif = outInfo.hasExif ? FormatSystemTime(outInfo.exifTime) : L"없음 (EXIF 없음)";
    outInfo.strTargetTime = FormatSystemTime(outInfo.targetTime);
    outInfo.strFileTime = FormatSystemTime(outInfo.fileTime);
    outInfo.strFileSize = FormatFileSize(outInfo.fileSize);
    outInfo.isChecked = true;

    return true;
}

// In-place direct binary patch for JPEG files (0.001ms, Lossless)
static bool PatchJpegExifDateBinary(const std::wstring& filePath, const SYSTEMTIME& newTime)
{
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    char newDateStr[20];
    sprintf_s(newDateStr, "%04d:%02d:%02d %02d:%02d:%02d",
        newTime.wYear, newTime.wMonth, newTime.wDay,
        newTime.wHour, newTime.wMinute, newTime.wSecond);

    // Read first 128KB header
    const DWORD MAX_BUF = 131072;
    std::vector<BYTE> buf(MAX_BUF);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buf.data(), MAX_BUF, &bytesRead, NULL) || bytesRead < 16) {
        CloseHandle(hFile);
        return false;
    }

    if (buf[0] != 0xFF || buf[1] != 0xD8) {
        CloseHandle(hFile);
        return false; // Not a JPEG
    }

    bool bPatched = false;
    size_t pos = 2;
    while (pos + 4 <= (size_t)bytesRead) {
        if (buf[pos] != 0xFF) break;
        unsigned char marker = buf[pos + 1];
        if (marker == 0xDA || marker == 0xD9) break;

        size_t len = ((size_t)buf[pos + 2] << 8) | buf[pos + 3];
        if (len < 2) break;

        if (marker == 0xE1) { // APP1 EXIF segment
            if (pos + 2 + len <= (size_t)bytesRead && len > 8) {
                const char* exifData = (const char*)(buf.data() + pos + 4);
                size_t segLen = len - 2;
                if (memcmp(exifData, "Exif\0\0", 6) == 0) {
                    for (size_t i = 6; i + 19 <= segLen; ++i) {
                        // Match YYYY:MM:DD HH:MM:SS format
                        if (exifData[i + 4] == ':' && exifData[i + 7] == ':' && exifData[i + 10] == ' ' &&
                            exifData[i + 13] == ':' && exifData[i + 16] == ':') {
                            // Verify digits
                            bool allDigits = true;
                            for (size_t k = 0; k < 19; ++k) {
                                if (k == 4 || k == 7 || k == 10 || k == 13 || k == 16) continue;
                                if (exifData[i + k] < '0' || exifData[i + k] > '9') {
                                    allDigits = false;
                                    break;
                                }
                            }
                            if (allDigits) {
                                DWORD fileOffset = (DWORD)(pos + 4 + i);
                                SetFilePointer(hFile, fileOffset, NULL, FILE_BEGIN);
                                DWORD bytesWritten = 0;
                                WriteFile(hFile, newDateStr, 19, &bytesWritten, NULL);
                                bPatched = true;
                            }
                        }
                    }
                }
            }
            break;
        }
        pos += 2 + len;
    }

    CloseHandle(hFile);
    return bPatched;
}

// Modify EXIF Date using Windows Shell Property Store
static bool SetPropertyStoreDate(const std::wstring& filePath, const SYSTEMTIME& newTime)
{
    IPropertyStore* pStore = NULL;
    HRESULT hr = SHGetPropertyStoreFromParsingName(filePath.c_str(), NULL,
        GPS_READWRITE, IID_PPV_ARGS(&pStore));
    if (FAILED(hr)) return false;

    FILETIME ftLocal, ftUtc;
    SystemTimeToFileTime(&newTime, &ftLocal);
    LocalFileTimeToFileTime(&ftLocal, &ftUtc);

    PROPVARIANT prop;
    hr = InitPropVariantFromFileTime(&ftUtc, &prop);
    if (SUCCEEDED(hr)) {
        pStore->SetValue(PKEY_Photo_DateTaken, prop);
        pStore->SetValue(PKEY_ItemDate, prop);
        pStore->Commit();
        PropVariantClear(&prop);
    }
    pStore->Release();
    return SUCCEEDED(hr);
}

bool ModifyImageExifDate(const std::wstring& filePath, const SYSTEMTIME& newTime, bool updateFileTime)
{
    // Step 1: Direct in-place binary patch for JPEG files
    bool bPatched = PatchJpegExifDateBinary(filePath, newTime);

    // Step 2: Fallback / Secondary update via Windows Property Store
    SetPropertyStoreDate(filePath, newTime);

    // Step 3: Synchronize file system timestamps
    if (updateFileTime) {
        FILETIME ftLocal, ftUtc;
        SystemTimeToFileTime(&newTime, &ftLocal);
        LocalFileTimeToFileTime(&ftLocal, &ftUtc);

        HANDLE hFile = CreateFileW(filePath.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            SetFileTime(hFile, &ftUtc, &ftUtc, &ftUtc);
            CloseHandle(hFile);
        }
    }

    return true;
}
