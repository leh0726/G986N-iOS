# Galaxy S20+ (y2q) 화면 티어링 수정 인계

## 증상

GPU 가속(`gpu=1`)을 켠 뒤로 화면이 움직일 때 여러 조각으로 갈라져 보인다(티어링).

## 원인

`xnu-src/xnu-8796.121.2/iokit/Kernel/MiniIOMobileFramebuffer.cpp`에서 확인한 내용이다.

- GPU는 앱 화면을 메모리(1125×2436)에 그리는 데까지만 한다.
- 그 결과를 CPU가 `show()` → `drawLayer()`에서 1440×3200으로 늘린다.
- 늘린 결과를 패널이 지금 읽고 있는 단 하나의 버퍼(UEFI 프레임버퍼 `0x9c000000`)에 직접 덮어쓴다.
- 패널은 약 120 Hz(8 ms마다)로 이 버퍼를 읽는데, CPU 복사는 그보다 오래 걸린다.
- 화면 갱신 시점(vsync)에 맞추는 장치가 없다. 대략의 박자 추정(`gOnTheBeat`)뿐이다.
- 그래서 한 프레임 안에 새 화면과 옛 화면이 여러 줄로 섞인다.

## 해결 방향

1. **페이지 플리핑 (근본 해결):** 버퍼를 두 개 둔다. 한쪽에 프레임을 다 그린 뒤, 패널이 한 번 다 읽은 순간(vblank)에 파이프의 소스 주소를 다른 버퍼로 바꾼다.
2. **늘리기를 GPU로:** CPU 대신 GPU로 크기를 늘리면 복사 시간이 크게 줄어든다. 1번과 함께 쓰면 좋다.

## 하드웨어 정보

y2q 기기 트리 `qcom,mdss_mdp@ae00000` 기준이다.

| 블록 | 주소 |
|---|---|
| MDSS 기준 | `0xae00000` (크기 `0x84208`) |
| MDP top | `+0x1000` |
| CTL0~5 | `+0x2000`, `+0x2200`, … `+0x2a00` (CTL0 = 기본 화면) |
| 믹서 LM0~5 | `+0x45000`~`+0x4a000` |
| 파이프 VIG0~3 | `+0x5000`, `+0x7000`, `+0x9000`, `+0xb000` |
| 파이프 DMA0~3 | `+0x25000`, `+0x27000`, `+0x29000`, `+0x2b000` |
| INTF0~3 | `+0x6b000`(DP), **`+0x6b800`(INTF1, DSI, 기본 화면)**, `+0x6c000`(DSI), `+0x6c800`(DP) |
| 화면 버퍼 | `0x9c000000`, 1440×3200, 32비트 |

화면 버퍼 주소(`0x9c000000`)를 소스로 가진 파이프가 UEFI가 쓰는 파이프다.

## 먼저 돌고 있어야 하는 클럭

출처는 Linux 7.0.14의 `drivers/clk/qcom/gcc-sm8250.c`, `dispcc-sm8250.c`다.

| 클럭 | 주소 |
|---|---|
| GCC `gcc_disp_ahb_clk` | `0x10b00c` (Linux는 부팅할 때 켜고 계속 켜 둔다) |
| dispcc MDSS 전원 스위치 (`mdss_gdsc`) | `0xaf03000` |
| dispcc AHB 클럭 | `0xaf02080` |
| dispcc MDP 코어 클럭 | `0xaf0200c` |
| dispcc vsync 클럭 | `0xaf02024` |
| dispcc 픽셀 클럭 | `0xaf02004` |

**클럭이나 전원이 꺼진 블록을 읽으면 SoC가 멈출 수 있다.** 반드시 전원 스위치와 클럭을 먼저 확인해야 한다.

## 참고 자료

- **레지스터 의미:** Linux `drivers/gpu/drm/msm/disp/dpu1/`의 세 파일에 있다.
  - `dpu_hw_sspp.c`: 소스 주소, 크기, stride, 형식
  - `dpu_hw_ctl.c`: flush/start
  - `dpu_hw_intf.c`: 타이밍, 프레임·줄 카운터
- **Linux 소스:** `xnu-deps/build/ath11k/linux-7.0.14.tar.xz`에 이미 있다.
- **비슷한 기존 코드:** 레드우드용 읽기 전용 조사 `display_probe()`가 있다(`bsd/dev/redwood_adreno.c`, 부팅 인자 `display_probe=1`). 주소만 SM8250 값으로 바꾸면 같은 구조로 쓸 수 있다.

## 권장 순서

1. **읽기 전용 조사:**
   - 클럭과 전원 상태를 확인한다.
   - INTF1의 타이밍 레지스터와 프레임·줄 카운터를 100 ms 간격으로 두 번 읽어, 실제 갱신 빈도를 확인한다.
   - CTL0의 설정을 읽는다.
   - 파이프 8개의 소스 주소·크기·stride·형식을 읽어, UEFI가 쓰는 파이프를 찾는다.
2. **두 번째 버퍼 시험:** 같은 형식의 두 번째 버퍼를 만든다. 파이프 주소를 바꾼 뒤 CTL flush를 건다. 한 번만 바꿔 보고 화면이 그대로 나오는지 확인한다.
3. **프레임버퍼 드라이버에 넣기:** `MiniIOMobileFramebuffer.cpp`가 뒤쪽 버퍼에 그리고 vblank 때 주소를 바꾸도록 고친다. vblank는 INTF1의 줄 카운터로 판단한다.
4. **GPU 스케일링 (선택):** CPU 대신 GPU로 크기를 늘리도록 바꾼다.

## 주의

- 시험은 USB 모드(`./xl run`)에서 한다. boot 파티션에 쓰기 전에는 사용자에게 확인을 받는다.
- GPIO19~23과 QUP SE7(`0x99c000`)은 지문 센서용이다. 건드리면 폰이 리셋된다.
- 이미지는 `common/build.sh y2q FLASHBOOT=1 CRYPTEXES=flash`로 빌드한다. `userland/build.sh`를 직접 쓰면 안 된다.
- 커널을 바꾸면 끝에 `xnu/export.sh`를 돌려 `xnu/`에 반영한다.

## 함께 넘길 일: 디스플레이 끄기와 밝기 조정

같은 컨트롤러와 UEFI DisplayDxe가 관련돼 있다. 함수 위치는 `uefi/files/Binaries/y2q/QcomPkg/Drivers/DisplayDxe/DisplayDxe.efi` 기준이고, 이미지 기준 주소는 0이다.

- **`EFI_DISPLAY_POWER` 프로토콜:**
  - GUID `f352021d-9593-4432-bf04-67b9f3b76008`
  - 구조체 `0x453a0` = {rev `0x10000`, Set `0x3520`, Get `0x38d8`}
  - 함수가 This 인자를 받는다.
- **Qualcomm 디스플레이 전원 제어 표 (`0x45378`):** 함수에 This 인자가 없다. 이 표의 GUID는 아직 확정하지 않았다(설치 코드 `0x1c00`~`0x1d00` 부근).
  - rev `0x10000`
  - `0x2ca0`: 패널 전원 제어 (id, state)
  - `0x31c8`: 패널 전원 상태 (id≤9, out*)
  - `0x3240`: 밝기 제어 (id는 0이어야 함, level 0~100)
  - `0x3360`: 밝기 상태
- **계획:** 로더가 UEFI에 남아 있으니, 커널 요청을 공유 페이지로 받아 로더가 이 함수들을 부르게 한다. 배터리와 플래시 서비스가 이미 같은 방식이다.
- **iOS 쪽:** `MiniIOMobileFramebuffer`는 아직 selector 12(RequestPowerChange)와 밝기 요청을 무시하고 있다.

## 결과 (2026-10-25): 티어링 해결, 하드웨어 스케일링, 60 Hz

`MiniIOMobileFramebuffer.cpp`의 "display controller (y2q)" 부분에 구현했다. 사용자가 폰에서 "훨씬 낫다"고 확인했다.

### 조사에서 확인한 것

- 패널은 UEFI DisplayDxe의 **AMB622NP01**이다. DSI command mode, DSC 3:1, 1440×3200, 4 lanes, 1157 Mbit/s다. 한 프레임 전송에 약 8 ms가 걸린다.
- 펌웨어는 **파이프 두 개**를 쓴다. VIG0이 왼쪽 720열, VIG1이 오른쪽 720열이고 둘 다 `0x9c000000`을 stride 5760으로 읽는다. 믹서도 LM0, LM1로 source split이고, VIG1의 out_xy는 전체 좌표 720이다. CTL0의 flush 비트는 `0x3`이다.
- INTF1의 타이밍 레지스터는 0이다(command mode). 전송은 INTF1의 **tear check와 autorefresh**가 맡는다(Linux의 `has_intf_te`, `INTF_TEAR_*` `0x280`~`0x2b4`). ping-pong 블록의 tear check 레지스터는 쓰이지 않는다(모두 0이고, 써도 효과가 없다).
- 펌웨어의 autorefresh는 `0x80000002`, 즉 TE 펄스 두 번에 한 번이라 초당 30 프레임이다. TE는 GPIO66이고 초당 60회 들어온다.
- 파이프 주소와 스케일러 레지스터는 double buffer다. CTL flush 뒤 다음 전송 시작에 반영되고, 반영되면 flush 비트가 지워진다.
- 실행 중 `ml_io_map()`의 I/O map은 8 MB뿐이다. 18 MB 버퍼는 `ml_io_map_unmappable()`로 매핑한다.

### 구현

1. **버퍼 3개를 돌린다:** `0x9c000000`(펌웨어), `0x9d200000`(Display Reserved의 나머지 절반), 커널 메모리에서 잡은 4 GB 아래 연속 18 MB(write-combine)다.
   - 프레임은 화면에 나가는 버퍼도, 반영 대기 중인 버퍼도 아닌 곳에 그린다.
   - 대기 중인 플립은 새 프레임으로 교체한다. INTF1의 frame count로 그 사이에 하드웨어가 이미 가져갔는지 판단한다.
2. **확대는 VIG 파이프의 QSEED3(버전 3.0) 스케일러가 한다.** CPU는 1125×2436을 확대 없이 복사만 한다.
   - 프레임당 드라이버 시간이 19~27 ms에서 약 1.6 ms로 줄었다.
   - VIG1은 원본 562열부터 위상 0.5로 시작해 이음새를 맞춘다.
   - 펌웨어 값은 저장해 두고, 로더로 돌아갈 때 되돌린다(`MiniIOMobileFramebufferQuiesce()`, `loader_return.c`).
3. **autorefresh를 TE마다(`0x80000001`)로 바꿨다.** 패널이 초당 약 61 프레임을 받는다.

### 부팅 인자

| 인자 | 의미 |
|---|---|
| `fb_flip=0/2/3` | 버퍼 1개(예전 방식) / 2개 / 3개(기본) |
| `fb_hwscale=0` | CPU가 확대(예전 방식) |
| `fb_autorefresh=N` | TE N번마다 한 프레임(기본 1). 0이면 펌웨어 설정 그대로 |
| `fb_probe=1` | 컨트롤러 상태를 로더 로그에 남긴다 |
| `fb_probe=2` | 1에 더해, 위쪽 1/3이 초록인 두 번째 버퍼를 5초 보여 주는 플립 시험 |
| `fb_te=1`, `fb_kick=1` | ping-pong tear check, CTL0 START 직접 발행. 시험용이고 쓰지 않는다. kick은 진행 중인 전송과 겹쳐 화면이 깨졌다 |

5초마다 로더 로그에 `fb: N frames ...` 줄이 남는다. 그린 프레임 수, 플립 수, 교체된 플립 수, 패널에 보낸 프레임 수다.

### 알려진 제한

- 실행 중 `xl screenshot`은 `0x9c000000`만 읽는다. 그래서 지난 프레임이 나오고, 하드웨어 스케일링 중에는 1125 폭 그림이 1440 폭으로 잘못 보인다.
- 커널 패닉 화면은 로더로 돌아간 뒤에야 보인다.
- 시험 실행에는 `ufs=2 root_apfs=ufs_darwin`이 필요하다(없으면 launchd.ios가 없어 곧 끝난다). 화면을 다섯 손가락으로 1.5초 누르면 실행이 끝난다.
