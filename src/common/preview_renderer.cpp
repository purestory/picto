#include "preview_renderer.h"
#include "stb_image.h"
#include "stb_image_resize2.h"
#include <shlwapi.h>

ImagePreviewRenderer::ImagePreviewRenderer()
{
}

ImagePreviewRenderer::~ImagePreviewRenderer()
{
    Clear();
}

void ImagePreviewRenderer::Clear()
{
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
    m_loadAttempted = false;
}

void ImagePreviewRenderer::SetPath(const std::wstring& filePath)
{
    if (m_currentPath != filePath) {
        Clear();
        m_currentPath = filePath;
        m_loadAttempted = false;
    }
}

bool ImagePreviewRenderer::Load(const std::wstring& filePath, int targetW, int targetH)
{
    if (targetW <= 10 || targetH <= 10) return false;
    if (filePath == m_currentPath && m_hBitmap != NULL && m_targetW == targetW && m_targetH == targetH) return true;

    m_loadAttempted = true;
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

    // 1. Try Windows Imaging Component (WIC) with Hardware-accelerated Fant Scaling & Fast In-memory Rotation
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

                // Read EXIF Orientation metadata
                UINT orientation = 1;
                IWICMetadataQueryReader* pMetaReader = NULL;
                if (SUCCEEDED(pFrame->GetMetadataQueryReader(&pMetaReader))) {
                    PROPVARIANT prop;
                    PropVariantInit(&prop);
                    if (SUCCEEDED(pMetaReader->GetMetadataByName(L"/app1/ifd/{ushort=274}", &prop)) ||
                        SUCCEEDED(pMetaReader->GetMetadataByName(L"/ifd/{ushort=274}", &prop)) ||
                        SUCCEEDED(pMetaReader->GetMetadataByName(L"System.Photo.Orientation", &prop))) {
                        if (prop.vt == VT_UI2) orientation = prop.uiVal;
                        else if (prop.vt == VT_UI4) orientation = (UINT)prop.ulVal;
                    }
                    PropVariantClear(&prop);
                    pMetaReader->Release();
                }

                bool bSwapDims = (orientation >= 5 && orientation <= 8);
                UINT dispW = bSwapDims ? origH : origW;
                UINT dispH = bSwapDims ? origW : origH;
                m_origW = (int)dispW;
                m_origH = (int)dispH;

                if (origW > 0 && origH > 0) {
                    float scale = (std::min)((float)targetW / (float)dispW, (float)targetH / (float)dispH);
                    int scaleW = (std::max)(1, (int)(origW * scale));
                    int scaleH = (std::max)(1, (int)(origH * scale));

                    IWICBitmapScaler* pScaler = NULL;
                    hr = pFactory->CreateBitmapScaler(&pScaler);
                    if (SUCCEEDED(hr)) {
                        hr = pScaler->Initialize(pFrame, scaleW, scaleH, WICBitmapInterpolationModeFant);
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

                                        std::vector<uint32_t> scaledBuf(scaleW * scaleH);
                                        hr = pConverter->CopyPixels(NULL, scaleW * 4, (UINT)(scaledBuf.size() * 4), (BYTE*)scaledBuf.data());
                                        if (SUCCEEDED(hr)) {
                                            int finalW = bSwapDims ? scaleH : scaleW;
                                            int finalH = bSwapDims ? scaleW : scaleH;
                                            int startX = (targetW - finalW) / 2;
                                            int startY = (targetH - finalH) / 2;

                                            for (int y = 0; y < scaleH; ++y) {
                                                for (int x = 0; x < scaleW; ++x) {
                                                    uint32_t color = scaledBuf[y * scaleW + x];
                                                    int outX = x;
                                                    int outY = y;

                                                    if (orientation == 6) { // 90 deg CW
                                                        outX = (scaleH - 1 - y);
                                                        outY = x;
                                                    } else if (orientation == 3) { // 180 deg
                                                        outX = (scaleW - 1 - x);
                                                        outY = (scaleH - 1 - y);
                                                    } else if (orientation == 8) { // 270 deg CW
                                                        outX = y;
                                                        outY = (scaleW - 1 - x);
                                                    } else if (orientation == 2) { // Flip H
                                                        outX = (scaleW - 1 - x);
                                                        outY = y;
                                                    } else if (orientation == 4) { // Flip V
                                                        outX = x;
                                                        outY = (scaleH - 1 - y);
                                                    }

                                                    int dstIndex = (startY + outY) * targetW + (startX + outX);
                                                    if (dstIndex >= 0 && dstIndex < targetW * targetH) {
                                                        pPixels[dstIndex] = color;
                                                    }
                                                }
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

                        std::vector<unsigned char> resized(fitW * fitH * 4);
                        stbir_resize_uint8_linear(pixels, w, h, 0, resized.data(), fitW, fitH, 0, STBIR_RGBA);
                        stbi_image_free(pixels);

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
                                for (int x = 0; x < fitW; ++x) {
                                    int srcIdx = (y * fitW + x) * 4;
                                    unsigned char r = resized[srcIdx];
                                    unsigned char g = resized[srcIdx + 1];
                                    unsigned char b = resized[srcIdx + 2];
                                    unsigned char a = resized[srcIdx + 3];

                                    uint32_t color = (a << 24) | (r << 16) | (g << 8) | b;
                                    pPixels[(startY + y) * targetW + (startX + x)] = color;
                                }
                            }
                            m_hBitmap = hDIB;
                        }
                    }
                }
            }
            fclose(fp);
        }
    }

    return (m_hBitmap != NULL);
}

void ImagePreviewRenderer::Draw(HDC hdc, const RECT& rcDest, const wchar_t* placeholderText)
{
    int w = rcDest.right - rcDest.left;
    int h = rcDest.bottom - rcDest.top;
    if (w <= 0 || h <= 0) return;

    // Automatically load image on-demand if path is set and bitmap is missing or size changed
    if (!m_currentPath.empty() && (!m_loadAttempted || m_targetW != w || m_targetH != h)) {
        Load(m_currentPath, w, h);
    }

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBitmap = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldMemBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

    HBRUSH bgBrush = CreateSolidBrush(RGB(24, 24, 27)); // Sleek dark slate #18181b
    RECT localRc = { 0, 0, w, h };
    FillRect(memDC, &localRc, bgBrush);
    DeleteObject(bgBrush);

    if (m_hBitmap) {
        HDC srcDC = CreateCompatibleDC(hdc);
        HBITMAP oldSrcBmp = (HBITMAP)SelectObject(srcDC, m_hBitmap);
        BitBlt(memDC, 0, 0, w, h, srcDC, 0, 0, SRCCOPY);
        SelectObject(srcDC, oldSrcBmp);
        DeleteDC(srcDC);
    } else {
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(161, 161, 170)); // Elegant slate-400

        HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(memDC, hFont);

        DrawTextW(memDC, placeholderText, -1, &localRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memDC, oldFont);
        DeleteObject(hFont);
    }

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(63, 63, 70)); // Refined slate border
    HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
    HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, nullBrush);
    Rectangle(memDC, 0, 0, w, h);
    SelectObject(memDC, oldPen);
    SelectObject(memDC, oldBrush);
    DeleteObject(borderPen);

    BitBlt(hdc, rcDest.left, rcDest.top, w, h, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldMemBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
}
