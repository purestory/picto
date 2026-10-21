#pragma once

#include "framework.h"
#include "Resource.h"
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <commctrl.h>
#include <shlobj.h>
#include <imm.h>

#pragma comment(lib, "imm32.lib")

// Global application fonts
extern HFONT g_hFontNormal;
extern HFONT g_hFontBold;
extern HFONT g_hFontTitle;
extern HFONT g_hFontLog;
extern UINT g_currentDpi;

// Logging globals
extern std::mutex g_logMutex;
extern std::vector<std::wstring> g_logQueue;

// DPI Awareness & Scaling Helper Functions
void InitDpiAwareness();
UINT GetWindowDpi(HWND hWnd);
int ScaleDpi(int value, UINT dpi);
void CreateDpiFonts(UINT dpi);
void ApplyFontsToControls(HWND hWnd);

// Size & String Formatting
std::wstring FormatFileSize(uint64_t bytes);

// Logging Helpers
void EnqueueLog(const std::wstring& msg);
void DrainLogQueue(HWND hEditLog);

// Folder & File Selection Dialogs
bool BrowseFolder(HWND hParent, std::wstring& outPath);
bool BrowseFile(HWND hParent, std::wstring& outPath, const wchar_t* filter);
bool BrowseMultipleFiles(HWND hParent, std::vector<std::wstring>& outFiles, const wchar_t* filter);

// Subclass procedure to intercept Enter key and commit IME in search edit boxes
LRESULT CALLBACK FilterEditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

// Exclude folder parsing helper
std::vector<std::wstring> ParseExcludeTokens(const std::wstring& input);
