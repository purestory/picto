#pragma once

#include "framework.h"
#include <string>
#include <vector>
#include <cstdint>

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

// Resized Image Cleaner Dialog Procedure
INT_PTR CALLBACK ResizedCleanerDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
