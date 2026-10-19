#pragma once

#include "targetver.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>

// Windows shell and common controls
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>

// WIC (Windows Imaging Component) and CryptoAPI
#include <wincodec.h>
#include <wincodecsdk.h>
#include <wincrypt.h>

// C++ Standard Library
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <regex>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <queue>
#include <algorithm>

// Library pragmas
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "imm32.lib")
#include <imm.h>
