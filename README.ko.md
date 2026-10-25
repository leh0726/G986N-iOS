[English](README.md) | **한국어**

# G986N-iOS — 갤럭시 S20+(SM-G986N)에서 돌아가는 iOS 16.5

이 저장소는 Apple의 오픈소스 **XNU 커널**(`xnu-8796.121.2`, iOS 16.5의 커널)과 **수정하지 않은 iOS 16.5 사용자 영역**을
**삼성 갤럭시 S20+ 5G(y2q, SM-G986N, 스냅드래곤 865)**에서 실행한다. OOSEX GANG의
[HackiOS](https://github.com/oosexgang/hackios-xnu-kernel)를 이 기기로 옮긴 것이다. HackiOS의 두 저장소인
hackios-xnu-kernel과 [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland)를 커밋 기록까지 합쳐 담았기
때문에, 한 번만 clone하면 전체 트리가 받아진다.

> **이 저장소에는 Apple의 파일이 하나도 없다.** `ios165/fetch.sh`가 Apple의 CDN에서 iOS 16.5 IPSW를 *내 컴퓨터로* 내려받아
> 필요한 부분만 꺼낸다. 이 프로젝트는 독립적인 연구 프로젝트이며 Apple Inc.와 관계가 없고, Apple의 허가나 보증을 받지 않았다.
> iOS, iPhone, Apple은 Apple Inc.의 상표다.

## y2q에서 되는 것

컴퓨터 없이 폰의 boot 파티션에서 iOS 16.5가 스스로 부팅한다(`xl install-autoboot`).

- **루트:** 폰 플래시에 있는 iOS 볼륨이다. iOS가 쓴 데이터는 재부팅해도 남는다.
- **GPU:** Adreno 650이 Mesa/freedreno로 CoreAnimation 화면을 그린다.
- **화면:** 디스플레이와 터치 스크린이 작동한다.
- **그 밖에:** 배터리 정보(MAX77705), CPU 코어 최대 8개.
- **아직 안 되는 것:** 밝기 조절, 화면 끄기.

## y2q에서 안 되는 것

- **Wi-Fi:** 네트워크가 검색되고 연결까지 되지만, 인터넷이 응답하지 않는다.
- **충전:** 배터리는 방전만 되고 충전은 안 된다. 기기를 충전하려면 TWRP로 부팅해야 한다.
- **밝기 조절, 화면 끄기**

## 저장소에 없는 것과 되살리는 방법

용량이 큰 파일, Apple의 파일, 폰에서 꺼낸 파일은 git에 넣지 않았다(`.gitignore`는 허용 목록 방식이다). 스크립트 하나가
이것들을 순서대로 내려받거나 빌드하고, 이미 있는 단계는 건너뛴다.

```sh
common/bootstrap.sh y2q
```

| 빠진 것 | 크기(약) | 만드는 방법 |
|---|---|---|
| iOS 16.5 (`ios165/`) | 18 GB | `ios165/fetch.sh`: Apple IPSW에서 내 컴퓨터로 추출 |
| Apple XNU 소스 (`xnu-src/`) | 0.5 GB | `xnu/apply.sh`: Apple 태그 + `xnu/redwood.patch` + `xnu/files/` |
| 커널 빌드 의존성과 SDK (`xnu-deps/`) | 1.2 GB | `xnu-deps/setup.sh` |
| UEFI 포팅을 얹은 Mu-Silicium (`Mu-Silicium/`) | 6 GB | `uefi/apply.sh`, 그다음 `build_uefi.py -d y2q` |
| 폰의 GPU·Wi-Fi 펌웨어와 MAC 주소 (`fw-gpu/`, `fw-wifi-y2q/`) | 5 MB | `tools/y2q-firmware.sh`: TWRP 상태의 폰에서 가져옴 |
| 이미지, 커널, boot 이미지 (`userland/*.apfs`, `out/`) | 2.5 GB | `common/build.sh y2q FLASHBOOT=1 CRYPTEXES=flash` |

**필요한 것:**
- Linux x86-64, 여유 공간 약 40 GB
- `iPhoneOS16.5.sdk`가 들어 있는 theos (커널과 프로그램 빌드용)
- Mu-Silicium `setup_env.sh`의 패키지들 (UEFI 빌드용, [uefi/README.md](uefi/README.md))
- `adb`, 그리고 TWRP가 설치된 갤럭시 S20+(SM-G986N)

## 폰에 설치하기

먼저 폰의 원래 boot 이미지를 백업해 둔다(Android 커널이 들어 있다). 폰을 TWRP로 부팅한 상태에서:

```sh
XL_DEVICE=y2q ./xl install-autoboot     # out/y2q-autoboot.img를 boot 파티션에 쓴다 (쓰기 전에 확인을 받는다)
```

그 뒤로는 폰이 스스로 iOS를 부팅한다. 로더가 2초 기다리는 동안 볼륨 키를 누르면 대신 USB 명령 모드로 들어가서, 컴퓨터에서
시험 실행을 할 수 있다(`XL_DEVICE=y2q ./xl run`, `./xl log` 등). Android로 돌아가려면 TWRP에서:

```sh
XL_DEVICE=y2q ./xl restore-boot BACKUP.img
```

## 파일 위치

| 경로 | 내용 |
|---|---|
| `common/devices/y2q.conf` | 기기 프로필: 주소, 부팅 인자, 실행에 필요한 것 |
| `common/` | `bootstrap.sh`, `setup.sh`, `build.sh`, `run.sh` |
| `xl` | USB로 폰의 로더를 다루는 도구 |
| `xnu/` | XNU 변경분: `redwood.patch`(Apple 파일 수정)와 `files/`(새 파일, 드라이버는 `bsd/dev/redwood_*.c`) |
| `xnuloader/` | UEFI에서 XNU를 시작하는 EFI 로더 |
| `uefi/` | UEFI 포팅(Mu-Silicium): `files/Platforms/Samsung/y2qPkg` |
| `wifi/` | Linux 드라이버 호환 계층과 brcmfmac 연결 코드 |
| `userland/` | init, 주입 라이브러리, launchd 작업, 이미지 빌더 |
| `docs/` | `y2q-wifi.md`, `y2q-display.md`, `PORTING.md` |

## 크레딧과 라이선스

- 이 모든 작업의 바탕은 OOSEX GANG([t.me/oosexgangdevs](https://t.me/oosexgangdevs))의 **HackiOS**다. 커널 작업, 로더,
  사용자 영역, redwood·QEMU 포팅이 모두 HackiOS에서 왔다.
- 커널 쪽(`xnu/`, `xnuloader/`, 스크립트)은 [Apple Public Source License 2.0](LICENSE)을 따른다. XNU 변경분은 XNU의
  파생물이기 때문이다. 사용자 영역 쪽(`userland/`, `ios165/`의 스크립트, 사용자 영역 도구)은 [MIT 라이선스](LICENSE-userland)를 따른다.
- 제3자 코드는 각자의 라이선스를 따른다([prebuilt/NOTICES.md](prebuilt/NOTICES.md) 참고).
  - 저장소에 들어 있는 것: libzbitmap(MIT), libdeflate(MIT)
  - 저장소에 없고 스크립트가 내려받는 것: Linux의 brcmfmac·ath11k(ISC, BSD-3-Clause-Clear), FreeBSD LinuxKPI(BSD-2-Clause), Mesa(MIT)
- `uefi/files/Binaries/`에는 각 폰의 펌웨어(삼성, 샤오미)에서 꺼낸 UEFI 드라이버가 들어 있다. Project Silicium이 다른 기기용으로
  배포하는 방식과 같다.
- `uefi/files/Resources/DTBs/`의 device tree는 폰에서 읽어 온 것이고, `tools/dtb-scrub.py`로 폰의 시리얼 번호를 지웠다.
