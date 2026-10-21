#pragma once

#include "framework.h"
#include <string>
#include <cstdint>

// Stage 2: Fast 64-bit FNV-1a Sample Hash (Header + Trailer 16KB)
uint64_t CalculateFastSampleHash(const std::wstring& filePath, uint64_t fileSize);

// Stage 3: Zero-Copy Memory-Mapped File (MMF) Binary Comparison
bool AreFilesContentIdentical(const std::wstring& path1, const std::wstring& path2, uint64_t fileSize);

// Fallback MD5 calculator using 1MB large streaming RAM buffer
bool CalculateFileMD5(const std::wstring& filePath, std::string& outHash);
