#pragma once

#include "framework.h"
#include <string>
#include <vector>
#include <cstdint>

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

// Duplicate Cleaner Dialog Procedure
INT_PTR CALLBACK DupCleanerDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
