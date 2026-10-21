#pragma once
#include <windows.h>
#include <vector>
#include <string>

// Custom message for progress/completion
#define WM_APP_EXIF_PROGRESS (WM_APP + 50)
#define WM_APP_EXIF_FINISHED (WM_APP + 51)
#define WM_APP_EXIF_ADD_FILES (WM_APP + 52)

// Opens or focuses the EXIF Editor Dialog and adds the specified files
void ShowExifEditorDialog(HWND hParent, const std::vector<std::wstring>& initialFiles);

// EXIF Editor Dialog Procedure
INT_PTR CALLBACK ExifEditorDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
