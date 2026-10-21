#pragma once

#include "framework.h"
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <wincodec.h>

class ImagePreviewRenderer {
public:
    HBITMAP m_hBitmap = NULL;
    int m_origW = 0;
    int m_origH = 0;
    int m_targetW = 0;
    int m_targetH = 0;
    uint64_t m_fileSize = 0;
    std::wstring m_currentPath;
    bool m_loadAttempted = false;

    ImagePreviewRenderer();
    ~ImagePreviewRenderer();

    void Clear();
    void SetPath(const std::wstring& filePath);
    bool Load(const std::wstring& filePath, int targetW, int targetH);
    void Draw(HDC hdc, const RECT& rcDest, const wchar_t* placeholderText = L"사진 미리보기");
};
