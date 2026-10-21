#include "dedup_engine.h"
#include <windows.h>
#include <wincrypt.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")

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
