# 📸 Picto (픽토)

> **초고속 네이티브 C++ Win32 다중 스레드 사진/동영상 정리 & 중복·리사이즈 미디어 정리 도구**

[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-blue.svg)](https://microsoft.com)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17%20%2F%20Win32-green.svg)](https://isocpp.org)
[![Build](https://img.shields.io/badge/Build-Visual%20Studio%202022-orange.svg)](https://visualstudio.microsoft.com/)

**Picto**는 대량의 사진과 동영상을 초고속으로 분석하여 날짜별로 자동 분류하고, 중복 파일 및 축소(리사이즈/썸네일)된 이미지를 시각적 지각 해시(pHash)로 정밀하게 탐지하여 안전하게 정리하는 올인원 윈도우 데스크톱 애플리케이션입니다.

---

## ✨ 핵심 기능 (Key Features)

### 1. 🚀 다중 스레드 초고속 날짜별 미디어 자동 분류
- **스레드 풀 병렬 I/O**: CPU 코어 수에 맞춘 최적 스레드 풀 및 락프리 원자적 인덱스 분배
- **3단계 하이브리드 날짜 추출 파이프라인**:
  1. **EXIF 메타데이터 (최우선)**: JPEG APP1 바이너리 직접 파싱 및 Windows Imaging Component (WIC) 쿼리 리더 결합
  2. **파일명 정규식 패턴 (차순위)**: `IMG_YYYYMMDD_HHMMSS`, `Screenshot_YYYY-MM-DD`, `KakaoTalk_YYYYMMDD` 등 지원
  3. **파일 시스템 타임스탬프 (최후)**: 생성 일자 및 수정 일자 폴백
- **날짜별 폴더 구조 자동 생성**: `YYYY/YYYY-MM` 계층 폴더 생성 및 고속 이동/복사

### 2. ⚡ RAM 기반 3단계 제로카피 중복 파일 탐색기
대용량 파일이나 4K/8K 영상의 무분별한 디스크 I/O 병목을 해결한 3단계 파이프라인:
- **1단계 (RAM 크기 버킷 필터링)**: 고유한 파일 크기를 가진 95% 이상의 파일은 디스크 읽기 0ms로 건너뜀
- **2단계 (고속 샘플 해시)**: 동일 크기 후보군만 앞/뒤 16KB를 읽어 64비트 정수 연산 기반 샘플 해시 비교
- **3단계 (MMF 제로카피 무결성 검증)**: 메모리 매핑 파일(`CreateFileMapping` / `MapViewOfFile`)과 CPU 벡터화 `memcmp` 비교로 100% 무결성 보장

### 3. 🖼️ 지각 해시(pHash) 기반 축소(리사이즈) 사진 탐색기
- **2D-DCT 지각 해시 (Perceptual Hash)**: 해상도가 변경되거나 압축된 이미지의 시각적 주파수 지문 추출
- **하드웨어 가속 비교**: CPU 내장 `__popcnt64` 명령어로 수만 쌍의 해밍 거리를 수 밀리초(ms) 만에 연산
- **WIC Fant & stb_image 하이브리드 파이프라인**: WIC 하드웨어 스케일러를 최우선 활용하고 손상/비표준 포맷은 SIMD 가속 `stb_image` 및 `stb_image_resize2`로 안정적 처리
- **스마트 축소본 판별**: 원본과 2배 이상 용량/픽셀 수 차이가 나는 실제 축소본만 선별 감지

### 4. 🔍 2단계 폴더 제외 및 실시간 검색 필터
- **다중 콤마(`,`) 구분 폴더 제외**: `temp, backup, D:\Archive` 등 여러 제외 경로 동시 입력 지원
- **사전 탐색 단계 (Pre-scan)**: 탐색 시작 전 제외 폴더가 지정되면 디렉터리 순회 시 `it.disable_recursion_pending()`을 호출하여 하위 디렉터리 진입을 원천 차단 (0ms)
- **사후 필터 단계 (Post-scan)**: 결과가 나온 상태에서 엔터 입력 시 캐시된 메모리 경로를 매칭하여 0ms 만에 즉시 목록에서 숨김 처리
- **한글 IME 엔터 최적화**: `ImmNotifyIME` 및 `ImmGetVirtualKey`를 적용하여 한글 조합 중 첫 번째 엔터 입력 시 즉시 확정 및 갱신

### 5. 🖥️ 반응형 듀얼 미리보기 & 전체화면 UI
- **무깜빡임 더블 버퍼링 미리보기**: `SS_OWNERDRAW` Static 컨트롤과 `WM_DRAWITEM` 기반으로 창 크기 조절 시에도 종횡비를 유지하는 레터박스 렌더링
- **원클릭 전체화면 토글**: `🗖 전체화면` ↔ `🗗 창 복원` 지원 및 2열 반응형 레이아웃 재배치
- **Per-Monitor V2 고DPI 지원**: 다중 모니터 해상도 및 배율 변경 시 비트맵 흐림 없이 선명한 UI 유지

### 6. 🛡️ 휴지통(Recycle Bin) 안전 삭제 연동
- **복원 가능한 안전 삭제**: 영구 삭제(`DeleteFileW`) 대신 `SHFileOperationW` + `FOF_ALLOWUNDO` 플래그를 사용하여 삭제된 모든 파일은 Windows 휴지통으로 이동
- **시각적 인덱스 분리 (`Visible Index Decoupling`)**: 제외 필터로 숨겨진 항목은 전체 선택 및 일괄 삭제 대상에서 완벽히 배제되어 안전하게 보호

### 7. 🗺️ 대화형 GPS 위치 지도 시각화
- WIC 메타데이터 리더를 통한 사진 EXIF 위도/경도(GPS) 좌표 추출
- 단일 파일 지도 보기 및 폴더 일괄 GPS 스캔 지원
- OpenStreetMap + Leaflet.js 기반 대화형 웹 지도 연동

---

## 🛠️ 빌드 방법 (Build Instructions)

환경 변수 등록 없이 Visual Studio 2022 Community의 MSBuild를 사용하여 빌드할 수 있습니다.

```powershell
# PowerShell에서 64비트 릴리스 빌드 실행
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" picto.sln /p:Configuration=Release /p:Platform=x64
```

- 빌드 성공 시 실행 파일은 `x64\Release\picto.exe`에 생성됩니다.
- 독립 실행형 네이티브 바이너리로 외부 런타임 설치 없이 단독 실행이 가능합니다.

---

## 📂 프로젝트 구조

```
picto/
├── picto.sln                # Visual Studio 2022 솔루션 파일
├── picto.vcxproj            # MSBuild 프로젝트 파일
├── picto.vcxproj.filters    # 프로젝트 필터 설정
├── picto.cpp                # 메인 UI 및 다중 스레드 백엔드 로직
├── picto.h                  # 메인 헤더 및 전역 구조체 정의
├── framework.h              # Win32/WIC/IMM32/C++ 표준 헤더 구성
├── perceptual_hash.h        # 2D-DCT 지각 해시(pHash) 알고리즘 구현
├── Resource.h               # 리소스 ID 정의
├── picto.rc                 # 다이얼로그 및 UI 리소스 스크립트
├── stb_image.h              # 고성능 이미지 디코딩 라이브러리
├── stb_image_resize2.h      # 고속 이미지 리사이즈 라이브러리
├── stb_impl.cpp             # stb 라이브러리 구현체
├── picto.ico                # 애플리케이션 메인 아이콘
└── README.md                # 프로젝트 문서
```

---

## 📄 라이선스 (License)

본 프로젝트는 오픈소스 라이선스에 따라 자유롭게 사용할 수 있습니다.
- `stb_image`, `stb_image_resize2`: Public Domain / MIT License
