#pragma once

#include "framework.h"
#include <intrin.h>

// Include stb_image and stb_image_resize2 as high-performance external imaging libraries
#ifndef STB_IMAGE_INCLUDED
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#include "stb_image.h"
#endif

#ifndef STBIR_INCLUDE_STB_IMAGE_RESIZE2_H
#include "stb_image_resize2.h"
#endif

namespace PerceptualHash {

// Cosine lookup table for 32x32 DCT to maximize speed
class DctTable {
public:
    double cosTable[32][32];
    DctTable() {
        const double PI = 3.14159265358979323846;
        for (int x = 0; x < 32; ++x) {
            for (int u = 0; u < 32; ++u) {
                cosTable[x][u] = std::cos(((2.0 * x + 1.0) * u * PI) / 64.0);
            }
        }
    }
};

static const DctTable g_dctTable;

// Hamming distance using hardware POPCNT instruction
inline int GetHammingDistance(uint64_t h1, uint64_t h2)
{
    return (int)__popcnt64(h1 ^ h2);
}

// Loads image and downscales to 32x32 8bpp grayscale buffer
// Uses WIC hardware acceleration first, falls back to stb_image
inline bool LoadAndScaleTo32x32(const std::wstring& filePath, std::vector<unsigned char>& out32x32, int& outOrigW, int& outOrigH)
{
    out32x32.resize(32 * 32);

    // 1. Try Windows Imaging Component (WIC) with high quality Fant downsampling
    IWICImagingFactory* pFactory = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (SUCCEEDED(hr)) {
        IWICBitmapDecoder* pDecoder = NULL;
        hr = pFactory->CreateDecoderFromFilename(filePath.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
        if (SUCCEEDED(hr)) {
            IWICBitmapFrameDecode* pFrame = NULL;
            hr = pDecoder->GetFrame(0, &pFrame);
            if (SUCCEEDED(hr)) {
                UINT w = 0, h = 0;
                pFrame->GetSize(&w, &h);
                outOrigW = (int)w;
                outOrigH = (int)h;

                IWICBitmapScaler* pScaler = NULL;
                hr = pFactory->CreateBitmapScaler(&pScaler);
                if (SUCCEEDED(hr)) {
                    hr = pScaler->Initialize(pFrame, 32, 32, WICBitmapInterpolationModeFant);
                    if (SUCCEEDED(hr)) {
                        IWICFormatConverter* pConverter = NULL;
                        hr = pFactory->CreateFormatConverter(&pConverter);
                        if (SUCCEEDED(hr)) {
                            hr = pConverter->Initialize(pScaler, GUID_WICPixelFormat8bppGray,
                                WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
                            if (SUCCEEDED(hr)) {
                                hr = pConverter->CopyPixels(NULL, 32, 32 * 32, out32x32.data());
                                if (SUCCEEDED(hr)) {
                                    pConverter->Release();
                                    pScaler->Release();
                                    pFrame->Release();
                                    pDecoder->Release();
                                    pFactory->Release();
                                    return true;
                                }
                            }
                            pConverter->Release();
                        }
                    }
                    pScaler->Release();
                }
                pFrame->Release();
            }
            pDecoder->Release();
        }
        pFactory->Release();
    }

    // 2. Fallback to external stb_image library
    FILE* fp = NULL;
    if (_wfopen_s(&fp, filePath.c_str(), L"rb") == 0 && fp) {
        fseek(fp, 0, SEEK_END);
        long fileSize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (fileSize > 0 && fileSize < 200 * 1024 * 1024) { // safety limit 200MB
            std::vector<unsigned char> fileBuf(fileSize);
            if (fread(fileBuf.data(), 1, fileSize, fp) == (size_t)fileSize) {
                int w = 0, h = 0, comp = 0;
                unsigned char* pixels = stbi_load_from_memory(fileBuf.data(), (int)fileSize, &w, &h, &comp, 1);
                if (pixels) {
                    outOrigW = w;
                    outOrigH = h;
                    // High-quality SIMD Mitchell/Cubic downsample to 32x32
                    stbir_resize_uint8_linear(pixels, w, h, 0, out32x32.data(), 32, 32, 0, STBIR_1CHANNEL);
                    stbi_image_free(pixels);
                    fclose(fp);
                    return true;
                }
            }
        }
        fclose(fp);
    }

    return false;
}

// 2D Discrete Cosine Transform (DCT) Perceptual Hash (Identical algorithm to Python imagehash.phash)
inline bool ComputePHash(const std::wstring& filePath, uint64_t& outHash, int& outWidth, int& outHeight)
{
    std::vector<unsigned char> pixels32;
    if (!LoadAndScaleTo32x32(filePath, pixels32, outWidth, outHeight)) {
        return false;
    }

    double dct[8][8];
    for (int u = 0; u < 8; ++u) {
        double cu = (u == 0) ? (1.0 / std::sqrt(2.0)) : 1.0;
        for (int v = 0; v < 8; ++v) {
            double cv = (v == 0) ? (1.0 / std::sqrt(2.0)) : 1.0;
            double sum = 0.0;
            for (int x = 0; x < 32; ++x) {
                double cosX = g_dctTable.cosTable[x][u];
                for (int y = 0; y < 32; ++y) {
                    sum += (double)pixels32[y * 32 + x] * cosX * g_dctTable.cosTable[y][v];
                }
            }
            dct[u][v] = sum * (2.0 / 32.0) * cu * cv;
        }
    }

    // Exclude DC component (u=0, v=0) and calculate median of 63 AC coefficients
    std::vector<double> acVals;
    acVals.reserve(63);
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            if (u == 0 && v == 0) continue;
            acVals.push_back(dct[u][v]);
        }
    }

    std::vector<double> sortedVals = acVals;
    std::sort(sortedVals.begin(), sortedVals.end());
    double median = sortedVals[sortedVals.size() / 2];

    uint64_t hash = 0;
    for (double val : acVals) {
        hash = (hash << 1) | (val > median ? 1ULL : 0ULL);
    }

    outHash = hash;
    return true;
}

} // namespace PerceptualHash
