#pragma once

#include "framework.h"
#include "picto.h"
#include <string>

extern OrganizerOptions g_currentOptions;

// File movement & destination folder resolution
bool MoveOrCopyFileToDateFolder(const std::wstring& filePath, const std::wstring& filename,
                               const SYSTEMTIME& dateTaken, const OrganizerOptions& options,
                               std::wstring& outDestination);

// Background Worker & Scanner Threads
void WorkerThreadProc(int threadId);
void ScanAndProcessThread();

// Processing Start / Cancel / Progress
void StartOrganizing(HWND hWnd);
void CancelOrganizing();
void UpdateStatsDisplay(HWND hWnd);
void OnProcessingFinished(HWND hWnd);

// Dialog & UI Management
INT_PTR CALLBACK OrganizerDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
void ShowOrganizerDialog(HWND hParent);
void CreateMainControls(HWND hWnd);
void UpdateLayout(HWND hWnd, int clientWidth, int clientHeight);

