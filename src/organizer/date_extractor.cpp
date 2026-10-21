#include "date_extractor.h"
#include <windows.h>
#include <wincodec.h>
#include <regex>
#include <vector>
#include <string>
#include <cstdio>

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

// Zero-I/O Metadata Date: Uses cached FILETIME from RAM (Prioritize lastWriteTime over creationTime)
bool ExtractFileMetadataDate(const FileItem& item, SYSTEMTIME& outTime)
{
    // 파일 복사 시 creationTime은 복사한 시점으로 갱신되므로, 원본 날짜를 보존하는 lastWriteTime을 최우선 적용
    FILETIME ft = item.lastWriteTime;
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) {
        ft = item.creationTime;
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
