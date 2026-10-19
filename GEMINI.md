# Agent Instructions (AI 행동 지침)

**항상 다음 지침을 준수하여 사용자를 지원하세요.**
1. **지식 업데이트:** 향후 개발을 진행하면서 특정 프로젝트에 국한되지 않는 **일반적인 Win32 C++ 개발, 성능 최적화, 혹은 UI/UX 개선 패턴**을 새롭게 발견하거나 구현하게 되면, AI는 스스로 판단하여 **이 문서(GEMINI.md)의 '개발 규칙' 항목에 해당 내용을 자동으로 요약 및 추가**해야 합니다.
2. 단, 너무 지엽적이거나 특정 상황에만 해당하는 특수한 예외 사항은 억지로 추가하지 않습니다.

---

# 빌드 규칙 (Build Rules)

환경 변수에 `cmake`와 `msbuild`가 등록되어 있지 않은 Windows 환경에서 C++ 프로젝트를 빌드할 때는 다음 규칙을 따릅니다.

1. PowerShell에서 `cmake --build`나 `msbuild` 명령어를 직접 실행하지 않습니다.
2. 빌드 시 반드시 **Visual Studio 2022 Community의 MSBuild 절대 경로**를 사용해야 합니다.
3. `&` 연산자를 사용하여 다음과 같이 솔루션을 빌드합니다.
   `& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" picto.sln /p:Configuration=Release /p:Platform=x64`

---

# 개발 규칙 (Development Rules)

Win32 C++ 프로젝트 개발 시 소프트웨어의 완성도를 높이기 위해 다음 원칙을 항상 준수하세요.

1. **동적 레이아웃 및 리사이징 (WM_SIZE 처리)**
   - `.rc` 파일에 정의된 컨트롤 크기 및 위치는 픽셀이 아닌 **DLU(Dialog Units)** 기반이므로, 시스템 DPI나 시스템 폰트에 따라 렌더링되는 크기가 달라집니다.
   - 따라서 `WM_SIZE` 핸들러에서 하드코딩된 픽셀 값을 사용한 위치/크기 계산을 피해야 합니다.
   - 항상 `GetWindowRect`로 컨트롤의 실제 렌더링 크기(width/height)를 구하거나, `MapDialogRect`를 통해 DLU를 픽셀로 변환한 후, 부모 클라이언트 영역(`GetClientRect`)에 비례하여 동적으로 배치(우측/하단 정렬 등)하세요.

2. **깜빡임 없는 UI 업데이트 (In-place Update & 더블 버퍼링)**
   - 리스트뷰(`ListView`)나 진행률, 통계 라벨 등의 아이템 상태가 변경되었을 때 전체 컨트롤을 지우고 새로고침하지 마세요.
   - 화면 깜빡임과 스크롤 초기화를 방지하기 위해 `ListView_SetItemText` 등을 사용하여 **변경된 특정 항목만 부분 업데이트(In-place Update)** 해야 합니다.
   - 잦은 텍스트 변경이 일어나는 통계 라벨은 이전 값과 달라졌을 때만 `SetWindowTextW`를 호출합니다.

3. **고DPI 및 다중 모니터 환경에서의 폰트/창 비트맵 스트레칭 방지 (Per-Monitor V2 DPI & MulDiv 스케일링)**
   - 구형 API인 `SetProcessDPIAware()`는 프로세스를 단일 시스템 DPI에 고정시킵니다. 이로 인해 다중 모니터(배율 불일치), 절전 모드 복귀, 화면 해상도/배율 변경 시 Windows DWM이 창을 강제로 비트맵 확대/축소하여 폰트가 뭉개지고 블러 처리되는 현상이 발생합니다.
   - 반드시 런타임에 `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`를 최우선 적용(실패 시 `SetProcessDpiAwareness` -> `SetProcessDPIAware` Fallback)하여 DWM 가상화 확대를 원천 차단해야 합니다.
   - 또한 모든 폰트 높이(`CreateFontW`) 및 윈도우 배치 좌표/패딩은 `-MulDiv(기본픽셀, dpi, 96)`로 현재 모니터의 실제 DPI(`GetDpiForWindow`)에 비례하여 동적으로 계산하고, `WM_DPICHANGED`를 처리해야 합니다.

4. **다중 스레드 I/O 처리 및 UI 응답성 보장**
   - 대용량 파일 탐색, MD5 해시 계산, EXIF 파싱 등 부하가 큰 작업은 절대 UI 스레드에서 직접 실행하지 마세요.
   - 작업자 스레드 풀(Worker Thread Pool)에서 비동기 병렬로 처리하고, 진행률 및 통계 데이터는 `std::atomic` 또는 뮤텍스로 보호하여 원자적으로 갱신합니다.
   - UI 스레드는 타이머(100~200ms) 주기로 통계 상태를 폴링하여 UI 컨트롤에 반영함으로써 불필요한 메시지 큐 과부하와 UI 프리징을 방지합니다.

5. **WIC(Windows Imaging Component) 및 Direct EXIF 파싱 Fallback**
   - 사진 EXIF 및 GPS 메타데이터 추출 시 Windows 기본 내장 컴포넌트인 WIC(`IWICImagingFactory`, `IWICMetadataQueryReader`)를 적극 활용하여 외부 무거운 라이브러리 의존성을 제거합니다.
   - 대량의 JPEG 고속 스캔 시에는 APP1 EXIF 세그먼트 직접 바이너리 파싱을 최우선 시도하고, 실패 시 WIC로 Fallback하는 2단계 파이프라인을 구축하여 처리 속도를 극대화합니다.

6. **메모리(RAM) 기반 다단계 중복 파일 필터링 및 MMF(Memory-Mapped File) I/O 최적화**
   - 대량의 파일이나 대용량 미디어(고해상도 사진, 4K/8K 영상 등)에 대해 무작정 전체 파일 해시(MD5/SHA)를 계산하면 심각한 디스크 I/O 병목이 발생하여 처리 속도가 급격히 저하됩니다.
   - **1단계 (RAM 크기 버킷 필터링)**: 디렉터리 스캔 시 획득한 파일 크기(`uint64_t`)를 RAM 해시맵에 등록하여, 크기가 고유한 파일(전체의 90~98%)은 디스크 읽기 및 해시 연산을 완전히 생략(0ms)합니다.
   - **2단계 (고속 샘플 해시 필터링)**: 동일한 크기를 가진 후보군 파일만 앞/뒤 16KB를 읽어 64비트 정수 연산(FNV-1a 등) 기반의 고속 샘플 해시를 비교합니다.
   - **3단계 (MMF 메모리 매핑 무결성 검증)**: 크기와 샘플 해시까지 일치하는 극소수의 파일에 한해 `CreateFileMapping` / `MapViewOfFile` 기반의 제로 카피(Zero-Copy) 메모리 매핑과 CPU 벡터화 `memcmp` 비교를 수행함으로써 100% 무결성을 보장하면서도 I/O 처리 속도를 수십~수백 배 향상시킵니다.

7. **pHash(지각 해시) 기반 축소/리사이즈 미디어 탐색 및 stb_image/WIC 하이브리드 파이프라인**
   - 대용량 사진 일반 정리 과정에서 모든 이미지에 대해 2D-DCT 지각 해시(pHash)를 계산하면 전체 정리 속도가 현저히 저하됩니다. 따라서 해상도가 축소된 썸네일/리사이즈 이미지 탐색 및 삭제는 반드시 **별도 전용 비동기 모달/창으로 분리**하여 파이프라인 간 간섭을 원천 차단해야 합니다.
   - 미디어 디코딩 및 다운샘플링 시 WIC(`IWICBitmapScaler` Fant 알고리즘) 하드웨어 가속을 우선 사용하고, 지원되지 않거나 손상된 파일은 검증된 고성능 외부 라이브러리인 `stb_image` 및 SIMD 가속 `stb_image_resize2`(`stbir_resize_uint8_linear`)를 Fallback 파이프라인으로 구성합니다.
   - 해밍 거리(Hamming Distance) 계산 시 CPU 내장 하드웨어 POPCNT(`__popcnt64`)를 적용하여 수만 쌍의 지각 해시 비교를 수 밀리초(ms) 안에 완료합니다.

8. **안전한 파일 삭제 UI/UX 및 Windows 휴지통(Recycle Bin) 연동 (`SHFileOperationW`)**
   - 중복 파일이나 축소본을 정리할 때 `DeleteFileW`를 사용한 즉시 영구 삭제는 사용자의 오조작 시 치명적인 데이터 유실을 초래합니다.
   - 일괄 삭제 작업 시 반드시 `SHFileOperationW`와 `FOF_ALLOWUNDO` 플래그를 결합하여 삭제된 파일이 Windows 휴지통(Recycle Bin)으로 이동되도록 보장하고, 사용자에게 절약되는 디스크 용량 및 삭제 대상 파일 목록을 직관적으로 확인시켜야 합니다.

9. **ListView 검색 필터 및 컬럼 정렬 시 체크 상태 무결성 보존 (lParam 인덱싱 패턴)**
   - 실시간 텍스트 검색(`EN_CHANGE`)이나 컬럼 헤더 클릭 정렬(`LVN_COLUMNCLICK`)로 인해 ListView의 항목이 갱신될 때, UI 컨트롤 상태에만 의존하면 필터링 중 보이지 않게 된 항목의 체크박스 상태가 유실됩니다.
   - 원본 데이터 구조체에 `id` 및 `bool isChecked`를 유지하고, ListView 아이템의 `lParam`에 인덱스/포인터를 바인딩하여 필터링 및 정렬 전후로 체크 상태를 원본 데이터와 자동 동기화해야 합니다.

10. **Owner-Draw Static 컨트롤을 활용한 반응형 무깜빡임 듀얼 이미지 미리보기**
   - 다이얼로그 내 고화질 이미지 미리보기를 구현할 때 `SS_OWNERDRAW` 스타일의 Static 컨트롤과 `WM_DRAWITEM` 메시지 핸들러를 결합하여 윈도우 서브클래싱 오버헤드 없이 완벽한 더블 버퍼링 렌더링을 구현합니다.
   - 컨트롤의 가변 크기(`rcItem`)에 맞추어 원본 종횡비(Aspect Ratio)를 유지하는 Letterbox 여백 계산과 WIC/stb_image 기반 DIBSection 렌더링을 적용하여, 창 리사이징 시에도 왜곡 없는 고품질 프리뷰를 즉시 제공합니다.

11. **대량 ListView 갱신 시 이벤트 폭주 방지(Notification Suppression) 및 메모리 캐싱 기반 초고속 정렬/필터링**
   - `ListView_SetCheckState`나 아이템 삽입 등 Win32 컨트롤 상태 변경은 매 항목마다 부모 창으로 `LVN_ITEMCHANGED` 알림 메시지를 전송합니다. 리스트를 정렬하거나 검색 필터링하여 일괄 삽입할 때 이 알림을 억제하지 않으면 $O(N^2)$의 메시지 큐 폭주가 발생하여 심각한 UI 프리징(버벅임)을 유발합니다.
   - 대량 갱신 시에는 반드시 불리언 가드(예: `g_bUpdating = true`)를 걸어 불필요한 연쇄 이벤트(`SyncCheckStates`, 통계 재계산)를 원천 차단하고, `WM_SETREDRAW`를 활용해야 합니다.
   - 정렬 및 통계 계산 시 Win32 API(`ListView_GetItem` 등)를 호출하지 않고, RAM 구조체에 포맷팅된 문자열 및 소문자 검색 키(`lowerFilterKey`)를 미리 캐싱(Pre-caching)하여 순수 CPU 메모리 연산으로 처리함으로써 대규모 리스트에서도 지연 없는 즉각적인 정렬 및 검색 반응 속도를 보장합니다.

12. **Win32 모달 다이얼로그의 전체화면(Maximize/Restore) 토글 및 반응형 2열 레이아웃**
   - 기본 대화상자(`.rc`)에 `DS_MODALFRAME`이 지정되어 있으면 `WS_MAXIMIZEBOX`를 주더라도 제목 표시줄의 최대화 버튼이 비활성화되거나 숨겨집니다. `DS_MODALFRAME`을 제거하고 `WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX` 스타일을 결합해야 창 테두리 드래그 크기 조절 및 Windows 최대화/복원 버튼이 정상 동작합니다.
   - UI 내에 전용 '전체화면' 토글 버튼을 제공할 때는 `IsZoomed(hDlg)`를 체크하여 `ShowWindow(hDlg, SW_MAXIMIZE)`와 `ShowWindow(hDlg, SW_RESTORE)`를 스위칭하고, `WM_SIZE`에서 버튼 텍스트(`🗖 전체화면` ↔ `🗗 창 복원`)를 동기화합니다.
   - 전체화면 전환 시 `WM_SIZE`에서 좌측 리스트/컨트롤 영역과 우측 반응형 프리뷰 패널의 비율을 동적으로 재계산하여 넓은 고해상도 모니터에서도 화면 공간을 100% 효율적으로 활용할 수 있도록 합니다.

13. **대화상자 Edit 컨트롤의 엔터(Enter) 키 검색 처리 및 IME 타이핑 버벅임 방지 (`ImmNotifyIME` & `ImmGetVirtualKey`)**
   - Win32 대화상자에서 실시간 검색(`EN_CHANGE`)을 적용하면 한글 IME 자모 조합이나 빠른 타이핑 시 매 글자마다 리스트뷰를 재구축하여 글자 씹힘과 극심한 UI 버벅임이 발생합니다.
   - 대화상자 내부 Edit 컨트롤에서 엔터 키로 검색을 실행하려면, 기본 대화상자 매니저가 엔터 키를 `IDOK`나 `DEFPUSHBUTTON`으로 가로채는 것을 방지하기 위해 Edit 컨트롤을 서브클래싱(`SetWindowSubclass`)하고 `WM_GETDLGCODE`에서 `DefSubclassProc(...) | DLGC_WANTALLKEYS`를 반환해야 합니다.
   - 특히 한글 IME 조합 중 엔터 키 입력 시 Windows는 `VK_RETURN` 대신 `VK_PROCESSKEY`(0xE5)를 전달하며 글자가 미완료(조합 중) 상태로 남아 `GetWindowTextW`에 반영되지 않는 문제가 발생합니다.
   - 이를 해결하기 위해 `WM_KEYDOWN`, `WM_KEYUP`, `WM_CHAR` 및 검색 갱신 루틴 진입 시 `ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0)`를 호출하여 조합 중인 글자를 버퍼에 즉시 강제 확정(Force-Commit)한 뒤 텍스트를 읽어야 합니다.
   - 또한 대화상자의 기본 `IDOK` 핸들러에서도 불필요한 포커스 비교 없이 즉시 리스트를 갱신하도록 처리하고, `EN_KILLFOCUS` 시에도 뷰를 갱신하도록 구성하여 사용 편의성을 극대화합니다.

14. **2단계 폴더 제외 파이프라인 (Pre-scan 디렉터리 스킵 + Post-scan 즉시 UI 필터링) 및 데이터 보존 (Visible Index Decoupling)**
   - **사전 탐색 단계 (Pre-scan)**: 탐색 시작 전 제외 폴더가 입력되어 있거나 엔터를 누른 경우, `recursive_directory_iterator` 순회 시 매칭되는 디렉터리에 대해 `it.disable_recursion_pending()`을 호출하여 하위 디렉터리 진입을 원천 차단합니다. 이를 통해 불필요한 디스크 I/O와 무거운 해시/pHash 연산을 0ms로 단축합니다.
   - **사후 탐색 단계 (Post-scan)**: 이미 결과 목록이 표시된 상태에서 제외 폴더를 입력하고 엔터를 치면, 전체 목록(`g_matches`)을 다시 스캔하지 않고 캐시된 경로 배열을 기준으로 메모리 내에서 즉시 매칭하여 화면에서 해당 폴더의 항목들을 실시간(0ms)으로 숨깁니다.
   - **시각적 인덱스 분리 (Safety)**: 필터링(텍스트 검색 및 폴더 제외) 시 원본 데이터 배열을 보존하면서 시각적으로 표시되는 인덱스 목록(`g_visibleIndices`)을 분리 관리합니다. 전체 선택/해제 및 휴지통 일괄 삭제 시 `g_visibleIndices`에 한정하여 동작하며, 삭제 완료 시에도 실제 삭제된 인덱스 집합(`deletedSet`)만을 원본 배열에서 제거하여 화면 밖의 숨겨진 폴더/파일이 영구 유실되는 것을 완벽히 방지합니다.




