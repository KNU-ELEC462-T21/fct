# fct

**한국어** | [English](README.en.md)

**File Compression Transfer** — 병렬 멀티 TCP 연결, 적응형 zlib 청크 압축, CRC32·SHA-256 무결성 검증 및 비트맵 기반 이어받기를 지원하는 C 기반 파일 전송 CLI 도구입니다.

## 빌드 방법

Ubuntu 24.04 환경 기준으로 아래 명령을 실행합니다.

### 의존성 설치

`sudo apt-get`으로 `gcc`, `clang-format`, `zlib1g-dev` 패키지를 설치합니다(표준 POSIX `pthread` 및 `zlib` 라이브러리 함께 링크).

```sh
make init
```

### 컴파일

프로젝트 루트에서 `make`(또는 `make all`)를 실행하면 `build/fct` 실행 파일이 생성됩니다.

```sh
make
```

### 테스트, 벤치마크, 포맷팅 및 정리

- **테스트 실행**: 단위 테스트 및 종단간(end-to-end) 통합 테스트 실행 (`network_test`, `cli_test`, `cli_extended_test`, `progress_test`, `test_file_writer`, `file_reader_test`, `compress_test`, `checksum_test`, `bitmap_test`, `worker_test`, `integration_test`)
  ```sh
  make test
  ```
- **벤치마크 실행**: `build/fct` 빌드 후 `fct`(`-t 1`, `-t 4`, 비압축 파일 대상 `-z`)와 `rsync`(`raw`, `-z` zstd, `-z` zlib-1) 및 3단계 압축-전송-해제(`gzip -1`, `zstd`)를 비교하는 종단간 벤치마크 스위트 실행
  ```sh
  make bench
  ```
  - **테스트 워크로드**: `compressible.log`, `mixed.tar`, `archive.gz`, `random.bin`
  - **네트워크 프로파일** (Linux 네트워크 네임스페이스 및 `tc netem` 기반): `wan_100m` (`100 Mbps` / `20 ms RTT`), `lan_1g` (`1 Gbps` / `2 ms RTT`), `unlimited`
  - **사전 요구 사항**: `python3`, `ip netns`·`tc` 실행을 위한 `sudo` 권한, `rsync`, `gzip`, `zstd`
  - **출력 및 옵션**: 결과는 `bench/results/`에 `raw_runs.csv`, `summary.json`, `report.md` 및 SVG 차트로 저장되며, `python3 bench/run_bench.py --size-mb 256 --runs 3 --warmup 1 --profiles ... --files ... --output-dir ...` 등으로 세부 옵션을 조정할 수 있습니다.
- **코드 포맷팅**: `clang-format`으로 `src/` 및 `include/` 소스 포맷팅
  ```sh
  make format
  ```
- **빌드 산출물 삭제**: `build/` 디렉터리 제거
  ```sh
  make clean
  ```

## 실행 방법

```sh
./build/fct -s [-p port] [-d output_dir]
./build/fct -c server_ip -f file_path [-p port] [-t workers] [-z]
./build/fct -h
```

항상 서버를 먼저 실행한 뒤 클라이언트로 파일을 전송합니다.

1. **기본 실행** (포트 `9000`, 현재 디렉터리 수신)

   ```sh
   # 수신 측 (서버)
   ./build/fct -s

   # 송신 측 (클라이언트)
   ./build/fct -c 127.0.0.1 -f ./example.txt
   ```

2. **포트·수신 디렉터리 지정 및 병렬 워커(`-t 4`) 사용**

   ```sh
   # 수신 측 (서버): /tmp/recv 디렉터리는 미리 존재해야 함
   ./build/fct -s -p 9100 -d /tmp/recv

   # 송신 측 (클라이언트): 4개의 데이터 워커로 병렬 전송
   ./build/fct -c 192.168.0.10 -f ./archive.tar -p 9100 -t 4
   ```

3. **강제 압축(`-z`) 사용**

   ```sh
   ./build/fct -c 192.168.0.10 -f ./data.bin -p 9100 -t 4 -z
   ```

## 사용 방법

### CLI 옵션

| 옵션 | 설명 | 대상 | 기본값 / 제한 |
| --- | --- | --- | --- |
| `-s` | 서버 모드 (파일 1개를 수신한 뒤 종료) | 서버 | `-c`와 상호 배타적 |
| `-c server_ip` | 클라이언트 모드 (지정한 IPv4 주소로 파일 전송) | 클라이언트 | `-s`와 상호 배타적, 유효한 IPv4 주소 필수 |
| `-f file_path` | 전송할 입력 파일 경로 | 클라이언트 | 클라이언트 모드 필수 |
| `-p port` | TCP 포트 번호 | 공통 | `1–65535` (기본값: `9000`) |
| `-d output_dir` | 수신 파일을 저장할 디렉터리 | 서버 | 미리 존재해야 함 (기본값: `.`) |
| `-t workers` | 병렬 데이터 워커 수 | 클라이언트 | `1–8` (기본값: `1`, 실제 협상 워커 수는 `min(workers, missing_chunks)`로 신규 전송 시 `min(workers, total_chunks)`, 빈 파일이나 이미 완료된 이어받기 세션은 `0`) |
| `-z` | zlib 청크 강제 압축 (자동 압축 생략 우회) | 클라이언트 | 기본 비활성 (자동 모드) |
| `-h` | 사용법 도움말 출력 | 공통 | — |

### 옵션 제약 사항

- `-s`와 `-c`는 상호 배타적이며 둘 중 하나를 반드시 지정해야 합니다.
- 클라이언트(`-c`) 모드에서는 `-f`가 필수이며 `-d`를 사용할 수 없습니다.
- 서버(`-s`) 모드에서는 `-f`, `-t`, `-z`를 사용할 수 없습니다.

### 압축 동작 방식

- 기본 청크 크기는 `2 MiB`이며, 네트워크 대역폭 대비 처리 속도를 우선하기 위해 zlib `Z_BEST_SPEED`(레벨 1)를 사용합니다.
- **자동 모드 (`-z` 미지정)**: 이미 압축된 확장자(`.zip`, `.gz`, `.tgz`, `.mp4`) 또는 매직 바이트(ZIP, GZIP, MP4)를 가진 파일과 빈 파일은 압축을 생략(`RAW`)합니다. 그 외 파일은 첫 번째 청크를 시험 압축해 원본 크기 대비 `95%` 이하로 줄어드는 경우에만 자동 압축(`AUTO`)을 활성화하며, `AUTO` 모드에서도 각 청크가 실제로 원본보다 작아진 경우에만 압축 데이터를 전송합니다.
- **강제 압축 (`-z` 지정)**: 자동 압축 생략을 건너뛰고 모든 청크를 압축해 전송(`FORCE`)합니다.

### 전송 흐름, 무결성 검증, 이어받기 및 진행률

- **핸드셰이크 및 병렬 전송**: 클라이언트가 원본 파일의 SHA-256 해시를 계산한 뒤 `HELLO` 제어 프레임과 파일 메타데이터(`file_sha256` 포함)를 전송합니다. 서버는 세션을 열거나 기존 세션을 이어받아 협상된 워커 수와 세션 토큰이 담긴 `READY` 프레임과 완료 청크 비트맵으로 응답합니다. 워커가 `2`개 이상 협상되면 클라이언트는 세션 토큰으로 `DATA` TCP 연결을 등록하고 아직 완료되지 않은 청크만 병렬로 전송합니다.
- **청크 단위 CRC32 검증 및 자동 재전송**: 각 청크 헤더에는 압축 해제 기준 원본 데이터의 IEEE CRC32가 포함됩니다. 각 전송 라운드는 연결을 닫지 않고 모든 데이터 채널에 `EOR` 마커를 보내 종료합니다. CRC32 검증에 실패한 청크가 있으면 서버가 완료된 청크를 체크포인트로 저장한 뒤 갱신된 비트맵과 함께 `RETRY` 제어 프레임을 전송하여 최대 `3`회(`FCT_MAX_CHUNK_RETRIES = 3`) 누락된 청크만 선택적으로 재전송합니다.
- **이어받기 (Resumable Transfer)**: 서버는 수신 디렉터리 내에 파일 크기·청크 크기·파일 SHA-256·파일명으로부터 결정적으로 파생된 `flock` 잠금 숨김 디렉터리(`.fct-<sha256>`)를 생성하고, 부분 데이터(`data.part`)와 CRC32로 검증된 체크포인트 비트맵(`data.bitmap`, `mkstemp` + `fsync` + `rename` 기반 원자적 저장)을 기록합니다. 최소 1개 이상의 청크가 완료된 상태에서 전송이 중단되면 체크포인트가 보존되며, 동일한 파일로 서버와 클라이언트를 다시 실행하면 완료된 청크를 건너뛰고 자동으로 이어받습니다. 같은 파일명이라도 크기나 SHA-256이 다른 버전은 별도의 세션 디렉터리를 사용합니다.
- **전체 파일 SHA-256 검증 및 원자적 게시**: 모든 청크가 완료되면 서버는 조립된 `data.part`의 SHA-256 해시를 메타데이터의 `file_sha256`과 대조합니다(불일치 시 체크섬 오류를 반환하고 세션을 폐기). 검증을 통과하면 `fsync()`로 디스크에 반영한 뒤 `linkat()`으로 최종 파일명에 원자적으로 게시하고(동일한 이름의 파일이 이미 존재하면 덮어쓰지 않고 오류 반환), 수신 디렉터리를 동기화한 다음 세션 디렉터리를 정리합니다.
- **진행률 출력**: 클라이언트는 `stderr`에 진행률(`Progress`), 전송 속도(`MiB/s`), 압축 절감률(`Saved`), 남은 예상 시간(`ETA`)을 최대 초당 1회 출력합니다(분모가 `0`인 항목은 `—` 표시). 이어받기 시 `Progress`는 기존에 이어받은 바이트를 포함하여 표시하고, 전송 속도·`Saved`·`ETA`는 현재 세션에서 전송된 데이터를 기준으로 계산합니다.
- **종료 상태**: 클라이언트는 서버로부터 유효한 `RESULT_OK` 제어 프레임을 수신한 경우에만 종료 코드 `0`으로 종료하며, 프로토콜·저장소·전송·체크섬 오류가 발생하면 0이 아닌 종료 코드를 반환합니다.

## 프로젝트 구조

```text
fct/
├── include/            # 공용 프로토콜(common.h) 및 모듈 헤더
├── src/
│   ├── main.c          # 진입점, 핸드셰이크, 이어받기·재전송·SHA-256 검증 오케스트레이션
│   ├── cli.c           # CLI 옵션 파싱 및 도움말 출력
│   ├── network.c       # TCP 소켓 연결, 타임아웃 I/O 및 제어·메타·청크·비트맵 프레임 직렬화
│   ├── worker.c        # CRC32 검증 및 선택적 청크 전송을 지원하는 멀티 TCP 병렬 워커
│   ├── compress.c      # 적응형/강제 zlib 청크 압축·복원
│   ├── checksum.c      # IEEE CRC32 및 FIPS 180-4 SHA-256 체크섬 계산
│   ├── bitmap.c        # 완료 청크 비트맵 관리 및 원자적 체크포인트 파일 영속화
│   ├── file_reader.c   # 청크 단위 파일 읽기
│   ├── file_writer.c   # 비순차 청크 쓰기, 공간 사전 할당 및 기존 부분 파일 재열기
│   └── progress.c      # 진행률·전송 속도·압축 절감률·ETA 및 이어받기 진행률 출력
├── tests/              # 모듈별 단위 테스트 및 종단간 통합 테스트
├── bench/              # fct vs rsync / 단계별 압축 벤치마크 (generate_data.py, netns_env.py, run_bench.py, generate_report.py)
└── Makefile            # 의존성 설치, 빌드, 테스트, 벤치마크, 포맷팅
```

## 팀 소개 및 역할

| 이름 | GitHub | 담당 역할 |
| --- | --- | --- |
| 정희균 | [@Segyun](https://github.com/Segyun) | TCP 네트워크 전송·제어/비트맵 프레임 프로토콜, 멀티 TCP 병렬·선택적 재전송 워커, `fct` vs `rsync` 벤치마크 스위트 |
| 서준우 | [@junwoo00](https://github.com/junwoo00) | 파일 라이터(`file_writer`), 확장 CLI 옵션(`-t`, `-z`), 진행률 추적(`progress`), 이어받기 세션·무결성 검증·안전한 파일 게시 통합 |
| 정언우 | [@eonuo9](https://github.com/eonuo9) | 파일 리더(`file_reader`), 적응형 청크 압축(`compress`), CRC32·SHA-256 체크섬(`checksum`) 및 영속 청크 비트맵(`bitmap`) 구현 |

## 라이선스

[MIT License](LICENSE)

---

이 프로젝트는 경북대학교 시스템프로그래밍 수업에서 진행한 프로젝트입니다.
