# jamjari-firmware — 잠자리 (Matter 재실 센서, nRF54LM20 DK)

한 침대에서 자는 사람과 반려동물의 움직임을 읽어 **Matter** 로 내보내는 센서 펌웨어. Nordic **nRF54LM20 DK** 에서 **Thread** 로 동작하고, SmartThings·Home Assistant 같은 Matter 컨트롤러에 붙는다.

*English summary: Firmware for "Jamjari", a bed-presence sensor on the Nordic nRF54LM20 DK. It reads an HLK-LD2450 24GHz radar over UART, tracks people in a configurable zone, and publishes occupancy over Matter-over-Thread (Occupancy Sensing + the draft Ambient Context Sensing 0x0431 cluster). Built with nRF Connect SDK v3.4.1. In the demo, radar input is replayed from a virtual scene and the 60GHz sensor is virtual. Apache-2.0, except files derived from the NCS sample, which stay under Nordic 5-Clause (see NOTICE).*

## Matter 기기 구성

| endpoint | 기기 | 클러스터 | 비고 |
|---|---|---|---|
| 1 | Occupancy Sensor (0x0107) | Occupancy Sensing `0x0406`, Ambient Context Sensing `0x0431`, Identify, Descriptor | 재실 = 구역 안 트래커 사람 수. 0x0431 은 초안 클러스터(`HumanActivityDetected` = 재실과 같은 값) |
| 2 | Light Sensor (0x0106) | Illuminance Measurement `0x0400`, Identify, Descriptor | 시연용 가상 조도(5 lux 고정) |
| 3 | Temperature Sensor (0x0302) | Temperature Measurement `0x0402`, Identify, Descriptor | 시연용 가상 온도(버튼·셸로 변경) |

(`src/default_zap/occupancy_sensor.zap` 기준. endpoint 0 은 Thread 기기 기본 루트 노드.)

## 이 저장소의 범위

- **시연에서 레이더 입력은 가상 장면 재생이고, 60GHz 센서는 가상 센서다.** `./build.sh --replay` 빌드는 레이더 대신 호스트가 USB 로 넣는 장면(`dummy/`)을 받는다. 실제 LD2450 은 배선·빌드가 되어 있지만 시연 영상의 입력은 재생 장면이다.
- LD2450 을 읽는 코드(`src/ld2450.cpp`)는 이 앱 안의 **UART 리더**이다. 정식 Zephyr 센서 드라이버가 아니다.
- 보드 업로드(웹으로 직접 전송) 토큰은 셸 명령(`jj token ...`)으로 보드 설정에 넣는다. **저장소에는 토큰 값이 없다.**
- 펌웨어에 OpenThread·Matter·`jj` 셸이 켜져 있어서, USB 로 보드를 잡은 사람은 셸로 설정을 볼 수 있다. 시연 보드용 설정이다.
- 온보딩 QR·수동 코드(`온보딩/`)는 SDK 시험값(VID 0xFFF1 / PID 0x8000)이다.

## 빌드 (요약)

nRF Connect SDK **v3.4.1**(`build.sh`·`sysbuild`·`VERSION` 기준), 보드 `nrf54lm20dk/nrf54lm20b/cpuapp`. `nrfutil sdk-manager` 로 툴체인을 잡고 `/opt/nordic/ncs/v3.4.1` 에 SDK 가 있다고 가정한다.

```bash
./build.sh             # 증분 빌드
./build.sh --pristine  # 처음부터
./build.sh --replay    # 시연용: 레이더 대신 재생 장면을 받는 빌드
```

자세한 사정(소스를 `~/ncs-build/` 로 복사해 빌드하는 이유, ZAP 다시 만들기)은 아래 「상세 메모」.

## 테스트

보드 없이 호스트(clang++, python3)에서 C++ 모듈을 파이썬 기준 구현과 대조한다.

```bash
test/run_test.sh      # LD2450 파서 + 구역·트래커 + 유령 점
test/run_fusion.sh    # 보드 안 추정(fusion.cpp) vs 파이썬 기준
```

주의: 이 테스트는 이 저장소 **바깥의 개발용 파일**(원본 녹화 `자료/*.bin`, 기준 구현 `트래커.py`·`ld2450_점검.py`, `.venv`, `60GHz` 장면 폴더)을 상대경로(`../..`)로 읽는다. 그 파일들은 이 저장소에 들어 있지 않아 저장소만 받아서는 돌지 않는다. 실제 녹화에서 뽑은 오염 입력 몇 개는 `test/cases/` 에 있다.

## 라이선스

- 기본 **Apache-2.0** (`LICENSE`).
- **일부 파일은 Nordic 5-Clause**(`LicenseRef-Nordic-5-Clause`): nRF Connect SDK 의 `contact_sensor` 샘플에서 온 파일(SPDX 헤더가 붙어 있음). 헤더를 그대로 두며 Nordic 칩에서만 쓸 수 있다. 자세한 내용은 `NOTICE`.

---

# 상세 메모 (개발 기록)

# nrf_jamjari — 잠자리 재실 센서 펌웨어 (nRF54LM20 DK)

> 보드는 **nRF54LM20 DK**(`nrf54lm20dk/nrf54lm20b/cpuapp`). 처음엔 nRF5340DK 용으로 만들어서 아래에 5340 이야기가 일부 남아 있다.
> **DK 글씨는 0부터 센다**(BUTTON 0~3, LED 0~3). 코드의 `DK_BTN2` = 보드 글씨 「BUTTON 1」, `DK_LED3` = 「LED 2」. 이 문서의 BTN·LED 번호는 코드 기준(1부터)이다.

HLK-LD2450 레이더를 UART 로 읽어서 Matter **Occupancy Sensor**(device type 0x0107, Occupancy Sensing cluster 0x0406, endpoint 1)로 내보낸다.
**지켜볼 구역 안에서 트래커가 살려 둔 사람이 1명 이상**이면 `Occupancy=1`, 10초 동안 0명이면 `Occupancy=0`. LED2 가 같은 상태를 보여 준다.
(예전엔 「레이더 타깃이 하나라도 있으면 1」이었는데, LD2450 이 약 4m 거리에 유령 점을 계속 내서 영원히 1 이었다. 아래 「구역 조정」 참고.)


시작점은 SDK 샘플 `nrf/samples/matter/contact_sensor` (v3.4.1). 바꾼 파일:

| 파일 | 내용 |
|---|---|
| `src/default_zap/occupancy_sensor.zap` | endpoint 1 을 Occupancy Sensor 로 교체 (Boolean State → Occupancy Sensing, Identify 에 TriggerEffect 추가). 편집 후 `west zap-generate` 로 `zap-generated/` 생성 |
| `src/ld2450.cpp/.h` | LD2450 UART 리더. 프레임 파싱은 `../ld2450_점검.py` 를 그대로 옮김 |
| `src/tracker.cpp/.h` | 구역 필터 + 사람 이어 붙이기. `../트래커.py`·`../레이더_지도.py`(in_zone) 를 그대로 옮김. 동적 할당 없음(트랙 16칸) |
| `src/app_task.cpp` | 레이더 프레임 → 구역·트래커 → Occupancy 속성 갱신 (10초 유지 타이머) + 1초 요약 로그 |
| `Kconfig` | `JAMJARI_ZONE_*`(구역), `JAMJARI_RADAR_SUMMARY_LOG`(1초 요약 로그 on/off) |
| `flash.sh` / `log.sh` | 올리기(확인 후) / 로그 보기. J-Link 달린 DK 포트만 고른다 (`tools/find_dk.py`) |
| `온보딩/` | SmartThings 등록용 QR(`matter-qr.png`)과 수동 코드 |
| `boards/nrf54lm20dk_nrf54lm20b_cpuapp.overlay` | 지금 보드. uart21 을 P1.04/P1.05 로 켬(uart20 은 콘솔) |
| `boards/nrf5340dk_nrf5340_cpuapp.overlay` | 예전 nRF5340DK 용(uart1, 같은 핀) |
| `build.sh` | 빌드 스크립트 (아래 「빌드」 참고) |

## 빌드

```bash
./build.sh             # 증분 빌드
./build.sh --pristine  # 처음부터
```

**왜 스크립트인가:** 이 폴더 경로의 한글(`지능형홈`, `실기`) 때문에 Matter 라이브러리(GN) 빌드가
`UnicodeDecodeError` 로 깨진다. CMake 가 경로를 REALPATH 로 풀어서 심볼릭 링크로도 못 피한다.
그래서 `build.sh` 가 소스를 `~/ncs-build/nrf_jamjari/` 로 rsync 한 뒤 거기서 빌드하고,
이 폴더의 `build` 는 `~/ncs-build/nrf_jamjari/build` 를 가리키는 링크로 만든다.
**코드는 항상 이 폴더에서 고친다** (`~/ncs-build` 쪽은 매번 덮어써진다).

ZAP 을 다시 생성할 때 (`.zap` 을 고친 뒤):

```bash
eval "$(~/.local/bin/nrfutil sdk-manager toolchain env --as-script sh --ncs-version v3.4.1)"
cd /opt/nordic/ncs/v3.4.1
west zap-generate -z "<이 폴더>/src/default_zap/occupancy_sensor.zap"
python3 "<이 폴더>/src/default_zap/patch_0431.py"   # 🔴 꼭 같이. 안 하면 빌드가 깨진다
```

`zap-generated/Clusters.matter` 는 스크립트가 알아서 `src/default_zap/occupancy_sensor.matter` 로 옮긴다(따로 mv 할 필요 없음).

`.zap` 은 SDK 의 zcl.json 이 아니라 **프로젝트 쪽 `src/default_zap/zcl/zcl.json`** 을 쓴다. SDK zcl.json 을 복사해서
`ambient-context-sensing-cluster.xml`(0x0431) 한 줄을 더한 것이고, SDK XML 은 상대경로(`../../../…/opt/nordic/ncs/v3.4.1/…`)로 가리킨다.
SDK 위치가 바뀌거나 이 폴더를 옮기면 그 상대경로를 다시 계산할 것. (zap-cli 는 xmlRoot 절대경로를 못 읽었다.)

## 0x0431 Ambient Context Sensing (endpoint 1, 초안 클러스터)

Matter 다음 규격 초안에만 있는 클러스터(SDK v3.4.1 에 ZAP 정의·서버 코드 없음)를 ep1 레이더 옆에 붙였다.

- 속성: `HumanActivityDetected`(0x0000, bool) = **Occupancy 와 같은 값**(사람 있으면 1). `HumanActivityType`(0x0001) = Other(0) 고정.
  FeatureMap = 1(HA), ClusterRevision = 1. 선택 속성 HoldTime·HoldTimeLimits 는 뺐다(구조체라 서버 코드가 필요).
- 서버 코드 없이 ZAP 속성 저장소(RAM)만 쓴다: `CMakeLists.txt` 의 `EXTERNAL_CLUSTERS AMBIENT_CONTEXT_SENSING_CLUSTER`,
  빈 `MatterAmbientContextSensingPluginServerInitCallback()`(`src/zcl_callbacks.cpp`), 값은 `ReportOccupancy` 가 `emberAfWriteAttribute` 로 쓴다.
- SmartThings·HA 는 이 클러스터를 모른다. 읽기: `chip-tool any read-by-id 0x0431 0 <노드> 1`.

## 배선 (LD2450 ↔ nRF54LM20 DK)

| LD2450 | nRF54LM20 DK |
|---|---|
| 5V | 5V |
| GND | GND |
| TX | **P1.05** (보드 RX) |
| RX | **P1.04** (보드 TX) |

- 핀은 DK 보드에 찍힌 `P1.04`·`P1.05` 글씨를 보고 꽂는다. 시연 재생 빌드(`--replay`)는 이 선 대신 디버거 USB VCOM0 에서 프레임을 받는다 — 그때 「레이더 신호 없음」은 `dummy/보드재생.py` 가 안 돌아서다.
- 아래 두 줄(uart1·Arduino 헤더)은 nRF5340DK 때 이야기다. nRF54LM20 DK 는 uart21 을 쓴다.
- UART: uart1, 256000bps 8N1 (실제 253968bps, 아래 참고), 흐름제어 없음.
- 기본 uart1 핀 P1.00/P1.01(Arduino D0/D1)은 쓰지 않는다. 네트워크 코어 UART 로 넘겨지고(gpio_fwd) DK 디버거 VCOM 에도 연결돼 있어서 레이더 TX 와 부딪친다.
- P1.04/P1.05 는 보드 정의에서 LED·버튼·QSPI 플래시·콘솔 어느 것과도 겹치지 않는다(보드 dts 확인).
- 로그 콘솔은 그대로 uart0 (USB VCOM, 115200).

## 구역 조정 (유령 점 거르기)

LD2450 은 아무것도 없는 곳에 speed=480 으로 고정된 「유령 점」을 계속 낸다. 9/29 실측: 레이더 약 4m 앞
(맥 쪽 지도에서 본 값 x≈-650, y≈3814 / 2인 녹화 파일에선 x≈0~300, y≈3900). 그래서 **구역 밖 점은 사람으로 치지 않는다.**

기본 구역: `x -1500 ~ 1500`, `y 0 ~ 3000` (mm, 경계 포함). y 는 레이더 정면으로 멀어지는 방향, x 는 좌우.
바꾸려면 `prj.conf` 끝에 필요한 줄만 넣고 다시 빌드한다:

```
CONFIG_JAMJARI_ZONE_XMIN=-1200
CONFIG_JAMJARI_ZONE_XMAX=1200
CONFIG_JAMJARI_ZONE_YMIN=300
CONFIG_JAMJARI_ZONE_YMAX=2500
```

- 값 고르는 법: 맥에서 `../레이더_지도.py` 로 지도를 띄우고 침대만 덮는 사각형을 잡으면 `../구역.json` 에 같은 이름(xmin·xmax·ymin·ymax)으로 저장된다. 그 숫자를 그대로 옮긴다.
- 유령이 구역 안으로 들어오면(방이 작아 ymax 를 4m 넘게 잡아야 할 때) 레이더 방향을 틀거나 ymax 를 유령 거리보다 짧게.
- 부팅 로그 첫머리에 `Zone x[..] y[..] mm` 로 실제 적용된 값이 찍힌다.
- 호스트 테스트는 `Kconfig` 의 기본값을 읽어 쓴다. 기본값을 바꾸면 `test/run_test.sh` 도 다시 돌릴 것.

## 올리기·로그 (내일. 아직 실행하지 않음)

```bash
./flash.sh            # DK 확인 → y 눌러야 west flash --erase
./flash.sh --recover  # "칩이 잠겨 있다" 고 나오면
./log.sh              # 로그 보기 (읽기 전용, logs/ 에 저장)
```

1초마다 요약 한 줄 (끄려면 `CONFIG_JAMJARI_RADAR_SUMMARY_LOG=n`, 로그 레벨은 `CONFIG_CHIP_APP_LOG_LEVEL`):

```
I: radar 10 fps, drop 0B, qdrop 0, raw 2, zone 1: A(-120,850), occ 1
```

| 칸 | 뜻 |
|---|---|
| `fps` | 지난 1초 받은 정상 프레임 수 (LD2450 은 보통 약 10) |
| `drop` | 지난 1초 버린 바이트 (배선 잡음·보드레이트 어긋남이면 늘어남) |
| `qdrop` | 처리가 밀려 버린 프레임 |
| `raw` | 레이더가 준 원시 타깃 수 (유령 포함) |
| `zone N: 이름(x,y)` | 구역 안 사람 수와 위치. 이름 뒤 `?` 는 이번 프레임엔 안 잡혔지만 1.5초 동안 살려 둔 사람 |
| `occ` | 지금 Matter 로 내보내는 Occupancy |

5초 넘게 프레임이 없으면 `W: 레이더 신호 없음 — 배선/5V 확인` 이 5초마다 찍힌다.

## 프레임 줄 · 레이더 지도 (녹화용)

`CONFIG_JAMJARI_FRAME_LOG=y`(기본)면 레이더 한 장마다 로그 포트에 한 줄을 찍는다.

```
I: F <ms> <슬롯1> <슬롯2> <슬롯3> <트랙 수> <occ>
   슬롯 = x,y,speed (mm, mm/s, 구역·트래커 전 원시값) 또는 빈 슬롯 -
   끝 두 숫자 = 보드가 그 프레임을 처리한 뒤의 판단 (붙잡은 트랙 수, Occupancy 0/1)
```

맥에서 지도·녹화:

```bash
cd ..   # 실기/
.venv/bin/python 레이더_지도.py --source nrf --log 자료/T1-$(date +%m%d-%H%M).jsonl
# 브라우저 http://localhost:8765 — 오른쪽 「보드 판단」 칸이 조명이 따르는 값
```

- 🔴 `log.sh` 와 동시에 못 연다(둘 다 잠금을 건다). 지도를 켜기 전에 log.sh 를 끈다.
- `/dev/cu.usbmodem1101`(다른 ESP32)은 어느 경로로도 열지 않는다.
- 지도의 구역은 `구역.json`, 보드의 구역은 Kconfig 라 서로 다를 수 있다.

## 🔴 SmartThings 에서 기기를 지우면 보드가 공장 초기화된다 (10/4 실측)

마지막 컨트롤러(fabric)가 빠지면 보드가 설정을 다 지운다 — **직접 전송 토큰·Thread 가 사라지고 보드 id 가 바뀐다**(서버 이름은 기본값이라 남음).
`--keep` 으로 구워도 소용없다. 다시 등록한 뒤 토큰을 다시 넣는다(값은 화면에 안 나옴):

```bash
cd ..   # 실기/
.venv/bin/python jj_shell.py jj token @real-token
.venv/bin/python jj_shell.py jj ping        # 로그에 upload: ... HTTP 200
```

10/4 밤부터는 SmartThings + HA(맥 matter-server node 2) 두 곳에 붙어 있어서, 한쪽만 지우면 초기화되지 않는다.

## 시연용 가상 조도 (endpoint 2) · 감지 멈춤은 BTN2 만

endpoint 2 = **Light Sensor**(0x0106, Identify·Descriptor·Illuminance Measurement 0x0400). 실제 센서는 없다. **5 lux 고정**
(MeasuredValue 6991 = 10000×log10(5)+1, Min 1 / Max 65534). 코드에서 값을 바꾸지 않는다.

**왜 넣었나 (10/4 실측):** SmartThings 기본 드라이버는 기기 하나를 미리 정해 둔 「모양(profile)」 하나로만 보여 준다.
- ep2 가 On/Off Plug-in Unit(감지 멈춤 스위치)이면 **Matter Switch** 드라이버가 잡아서 플러그로만 보였다(동작 감지·온도 숨음).
- Matter Sensor 드라이버로 바꿔도 처음 잡힌 모양에 동작 감지가 없어서 온도만 보였다.
- Matter Sensor 드라이버엔 「동작+온도」 모양이 없고 **「동작+조도+온도」(`motion-illuminance-temperature`)는 있다**
  (SmartThingsEdgeDrivers `matter-sensor` 의 fingerprints.yml·device_configuration.lua). 그래서 스위치를 빼고 조도를 더했다.

**감지 멈춤**은 Matter 에서 뺐고 DK **BTN2** 로만 뒤집는다(저장 안 함 — 재부팅하면 감지 중). 멈춤 중엔 **LED3** 이 켜진다.
멈춤이면 endpoint 1 Occupancy 를 1초 안에 0 으로 내리고 판단을 멈춘다. `JJ1,EV`·`F` 줄·1초 `radar ...` 요약도 안 낸다.
다시 켜면 트래커·침대 칸 상태를 새로 시작한다(열려 있던 사건은 버림, seq 는 이어 감). 바뀔 때와 부팅 때 한 줄:

```
JJ1,PAUSE,<1|0>,<uptime_ms>     1 = 멈춤, 0 = 감지 중
```

## 시연용 가상 온도 (endpoint 3)

endpoint 3 = **Temperature Sensor**(0x0302, Identify·Descriptor·Temperature Measurement 0x0402). 실제 센서는 없다.
`../nrf_virtual_temp`(nRF5340 용으로 따로 만들던 것)의 값 바꾸는 코드를 그대로 옮겼다.

- 시작 25.00도(`CONFIG_JAMJARI_TEMP_START_CENTI=2500`). 범위 **−10~40도**(ZAP Min/MaxMeasuredValue −1000/4000). 넘는 값은 범위 끝으로 자른다.
- DK **BTN3** = +1도, **BTN4** = −1도(`CONFIG_JAMJARI_TEMP_STEP_CENTI=100`). BTN3 은 ICD UAT 와 겹치므로 ICD 를 켜면 빌드가 막힌다(`#error`).
- 셸: `temp`(지금 값) · `temp 28` · `temp 27.5` · `temp up` · `temp down`. 바뀔 때 `Virtual temperature 28.00 C` 한 줄.
- 값은 저장하지 않는다. 재부팅하면 25.00도로 돌아간다.

## 호스트 테스트

`test/run_test.sh` — 보드 없이 맥에서 C++ 모듈을 파이썬 기준 구현과 대조한다.
파서(프레임·버린 바이트 수), 구역+트래커(프레임마다 트랙 수·이름·좌표·속도), 유령 점 차단.
녹화엔 프레임 도착 시각이 없어서 **균일 간격을 가정**한다(1인 녹화 60초/1668 ≈ 36ms, 2인 녹화 106ms).

## 아직 검증 못 한 것

1. **빌드만 확인했다.** 보드에서 한 번도 안 돌려 봤다. Matter 커미셔닝, Occupancy 보고, LD2450 수신 전부 미검증.
2. **보드레이트.** Zephyr 드라이버가 256000 을 거부해서 DT 는 250000 으로 두고, 시작할 때 UARTE1 BAUDRATE 레지스터에 `0x04104000` 을 직접 쓴다(드라이버의 `UARTE_GET_CUSTOM_BAUDRATE` 공식 = 16MHz/63 = 253968bps, 오차 -0.8%). UART 허용 오차 안이지만 실제로 프레임이 깨지지 않는지는 확인해야 한다. 안 되면 `src/ld2450.cpp` 의 나눗수 63 → 62(258064bps, +0.8%)로 바꿔 볼 것 (바로 아래 `static_assert` 값도 `0x04210000` 으로 같이).
3. **신호 전압.** nRF5340 입력 최대치는 VDD+0.3V 다. DK 의 VDD 가 3.0V 면 LD2450 의 3.3V TX 는 딱 한계선이다. 보드 받으면 DK 의 VDD 설정을 확인하고, 불안하면 LD2450 TX → P1.05 사이에 1kΩ 직렬 저항을 넣는다.
4. **ICD(저전력 폴링).** 원본 샘플 설정(Thread SED/LIT ICD)을 그대로 두었다. 상태 변화가 컨트롤러에 늦게(최대 수 초) 도착할 수 있다. 데모에서 느리면 `Kconfig` 의 ICD 기본값을 끈다.
5. **OccupancySensorType 속성.** FeatureMap 은 Radar 로 내보내지만, 옛 속성 `OccupancySensorType` 은 SDK 기본 처리(PIR)로 남는다. 홈 앱이 이 값을 보고 다르게 보여 주는지는 모른다.
6. **정확도.** 레이더가 잠든 사람(거의 안 움직임)을 계속 타깃으로 잡는지, 10초 유지 시간이 적당한지는 실측해야 한다. 가만히 있으면 점이 사라지는 경향이 있어서(9/29) 트랙은 1.5초, 점유는 그 뒤 10초를 버틴다 — 사람이 나간 뒤 Occupancy=0 까지 약 11.5초.
7. **트래커 시간 간격.** 파이썬과의 일치는 녹화에 균일 간격을 가정해서 확인했다. 보드에선 실제 도착 시각(k_uptime_get)을 쓴다. 게이트 700mm 는 「0.1초에 이만큼 안 튄다」 기준이라 프레임이 자주 빠지면(qdrop) 같은 사람이 새 이름으로 갈릴 수 있다.

## 고친 내역
- 10/5: 제목·배선·오버레이 표를 nRF54LM20 DK 로 고침, DK 버튼·LED 글씨가 0부터라는 것과 재생 빌드의 「레이더 신호 없음」 설명 추가.
