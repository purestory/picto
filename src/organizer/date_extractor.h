#pragma once

#include "framework.h"
#include "picto.h"
#include <string>

// In-Memory Fast EXIF & Date Extraction Functions
bool ExtractExifDate(const std::wstring& filePath, SYSTEMTIME& outTime);
bool ExtractFilenameDate(const std::wstring& filename, SYSTEMTIME& outTime);
bool ExtractFileMetadataDate(const FileItem& item, SYSTEMTIME& outTime);
bool ExtractDate(const FileItem& item, SYSTEMTIME& outTime);
