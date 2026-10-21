#pragma once

#include "framework.h"

// GPS Map Dialog Procedure
INT_PTR CALLBACK GpsMapDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

// Thread cleanup on main window exit
void CleanupGpsScanThread();
