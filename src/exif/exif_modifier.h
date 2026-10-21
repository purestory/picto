#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct ImageDateInfo {
    int id = 0;
    std::wstring filePath;
    std::wstring fileName;
    uint64_t fileSize = 0;
    SYSTEMTIME exifTime = { 0 };
    bool hasExif = false;
    SYSTEMTIME fileTime = { 0 };
    SYSTEMTIME targetTime = { 0 };
    bool isChecked = true;

    std::wstring strCurrentExif;
    std::wstring strTargetTime;
    std::wstring strFileTime;
    std::wstring strFileSize;
    std::wstring folderDir;
};

// Reads image EXIF taken date and filesystem write date
bool ReadImageDateInfo(const std::wstring& filePath, ImageDateInfo& outInfo);

// Modifies image EXIF date (In-place Lossless JPEG Patch + PropertyStore Fallback + FileTime Sync)
bool ModifyImageExifDate(const std::wstring& filePath, const SYSTEMTIME& newTime, bool updateFileTime);

// Helper to format SYSTEMTIME as YYYY-MM-DD HH:MM:SS
std::wstring FormatSystemTime(const SYSTEMTIME& st);

// Helper to add or subtract time offset (days, hours, minutes, seconds)
SYSTEMTIME AddTimeOffset(const SYSTEMTIME& baseTime, int days, int hours, int minutes, int seconds);

// Helper to extract date from filename
bool ParseDateFromFileName(const std::wstring& fileName, SYSTEMTIME& outTime);
