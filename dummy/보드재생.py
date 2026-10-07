#!/usr/bin/env python3
"""더미 장면을 보드에 레이더처럼 흘려 넣는다 (시연용, 10/3).

보드는 `./build.sh --replay` 로 만든 재생 펌웨어여야 한다. 그 펌웨어는 레이더 선 대신
디버거 USB 의 VCOM0(uart30, 115200)에서 LD2450 과 똑같은 30바이트 프레임을 받는다.
이 스크립트는 장면 jsonl 의 프레임을 그 모양으로 바꿔 장면 시각에 맞춰 보낸다.
보드의 판단·Matter·로그(VCOM1)는 진짜 레이더 때와 똑같이 돈다.

실행:  ../.venv/bin/python dummy/보드재생.py dummy/자연/장면/0-하룻밤전체.jsonl
       ... --speed 10        # 10배 빠르게 (보드 타이머는 실제 시간이라 10초 대기 같은 규칙은 그대로)
       ... --from 480        # 480초부터
       ... --loop            # 끝나면 처음부터 다시
       ... --port /dev/cu.usbmodemXXXX   # 포트 직접 지정 (J-Link 포트만)
       ... --g60 dummy/자연/60GHz/8-고양이누리옆구리로.jsonl   # 보드 추정(10/5 N): 60GHz 좌·우를 섞고 장면 시각을 붙여 보냄
       ... --g60 <60GHz.jsonl> --dump out.bin   # 보드 대신 파일로 (호스트 시험 test/run_fusion 입력)
끝내기: Ctrl+C (끝나면 빈 프레임을 몇 장 보내 「안 보임」으로 둔다)

🔴 /dev/cu.usbmodem1101(다른 ESP32)은 절대 열지 않는다.
"""

import argparse
import json
import math
import subprocess
import sys
import time
from pathlib import Path

import serial

BAUD = 115200
HEADER = b"\xAA\xFF\x03\x00"
FOOTER = b"\x55\xCC"
RESOLUTION = 360                      # LD2450 실측에서 늘 360
NRFUTIL = Path.home() / ".local/bin/nrfutil"
FORBIDDEN = "/dev/cu.usbmodem1101"


def encode_signed(v: int) -> bytes:
    """LD2450 부호 방식: 상위 바이트 MSB 1 = 양수, 0 = 음수. 값은 15비트."""
    mag = min(abs(int(v)), 0x7FFF)
    raw = mag | (0x8000 if v >= 0 else 0)
    return raw.to_bytes(2, "little")


def encode_frame(targets: list[dict | None]) -> bytes:
    body = b""
    for p in (targets + [None, None, None])[:3]:
        if p is None:
            body += b"\x00" * 8
        else:
            body += (encode_signed(p["x"]) + encode_signed(p["y"])
                     + encode_signed(p.get("speed", 0)) + RESOLUTION.to_bytes(2, "little"))
    return HEADER + body + FOOTER


def u32(v: int) -> bytes:
    return int(v).to_bytes(4, "little")


def encode_radar_t(t: float, targets: list[dict | None]) -> bytes:
    """보드 추정용 레이더 프레임: AA FF 07 00 | t_ms | 타깃 3×8 | 55 CC (src/replay_frames.h)"""
    return b"\xAA\xFF\x07\x00" + u32(round(t * 1000)) + encode_frame(targets)[4:-2] + FOOTER


def encode_g60(sec: int, rows: dict) -> bytes:
    """60GHz 좌·우 한 초: AA FF 06 00 | sec | L(in_bed breath heart move) | R | 55 CC. 없는 값 0xFF"""
    body = b""
    for side in ("L", "R"):
        r = rows[side]
        body += bytes([r["in_bed"], 0xFF if r["breath"] is None else r["breath"],
                       0xFF if r["heart"] is None else r["heart"], r["move"]])
    return b"\xAA\xFF\x06\x00" + u32(sec) + body + FOOTER


def encode_end() -> bytes:
    return b"\xAA\xFF\x08\x00" + u32(0) + FOOTER


def encode_start() -> bytes:
    """재생 시작: 보드 추정을 새로 시작(이전 재생이 Ctrl+C 로 끊겼어도 안 섞이게)"""
    return b"\xAA\xFF\x09\x00" + u32(0) + FOOTER


def fusion_stream(frames: list[dict], g60_path: Path) -> list[tuple[float, bytes]]:
    """(장면 시각, 보낼 바이트) 목록. 합치기.py run() 과 같은 순서: 초 s 의 60GHz 는 t ≥ s 인 첫 레이더 프레임 바로 앞."""
    g60: dict[int, dict] = {}
    for line in g60_path.read_text().splitlines()[1:]:
        r = json.loads(line)
        g60.setdefault(int(r["t"]), {})[r["side"]] = r
    out = [(frames[0]["t"], encode_start())]
    sec = math.ceil(frames[0]["t"])
    for f in frames:
        while f["t"] >= sec and sec in g60:
            out.append((f["t"], encode_g60(sec, g60[sec])))
            sec += 1
        out.append((f["t"], encode_radar_t(f["t"], f["targets"])))
    out.append((frames[-1]["t"], encode_end()))
    return out


def jlink_ports() -> list[tuple[int | None, str]]:
    """J-Link(SEGGER) 기기의 (vcom 번호, 포트) 목록. 다른 USB 시리얼은 안 나온다."""
    out = subprocess.run([str(NRFUTIL), "device", "list", "--json"], capture_output=True, text=True).stdout
    ports = []
    for line in out.splitlines():
        if not line.startswith("{"):
            continue
        msg = json.loads(line)
        data = msg.get("data") or {}
        if msg.get("type") == "task_end":
            data = data.get("data") or {}
        for dev in data.get("devices", []):
            traits = dev.get("traits") or {}
            if not (traits.get("jlink") or traits.get("seggerUsb")):
                continue
            for p in dev.get("serialPorts") or []:
                ports.append((p.get("vcom"), p["comName"].replace("/dev/tty.", "/dev/cu.", 1)))
    return ports


def find_vcom0(ports: list[tuple[int | None, str]]) -> str:
    """LM20 DK 는 로그가 VCOM1, VCOM0 이 비어 있다."""
    found = sorted({name for vcom, name in ports if vcom == 0})      # nrfutil 이 tty·cu 로 두 번 줄 때가 있다
    if len(found) != 1:
        sys.exit(f"DK 의 VCOM0 을 하나로 못 골랐습니다: {found or '없음'} — --port 로 지정하세요.")
    return found[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scene", type=Path)
    ap.add_argument("--port")
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--from", dest="start", type=float, default=0.0)
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--g60", type=Path, help="60GHz 좌·우 jsonl — 보드 추정 재생(장면 시각을 붙인 프레임)")
    ap.add_argument("--dump", type=Path, help="보드 대신 이 파일에 바이트를 쓴다(--g60 과 함께)")
    a = ap.parse_args()

    lines = a.scene.read_text().splitlines()
    head = json.loads(lines[0])
    frames = [json.loads(x) for x in lines[1:]]
    frames = [f for f in frames if f["t"] >= a.start]
    if not frames:
        sys.exit(f"--from {a.start:g} 이 장면 길이보다 깁니다.")
    if a.g60:
        stream = fusion_stream(frames, a.g60)
    else:
        stream = [(f["t"], encode_frame(f["targets"])) for f in frames]
    if a.dump:
        a.dump.write_bytes(b"".join(b for _, b in stream))
        print(f"✅ {a.dump}  {len(stream)}개 프레임")
        return 0
    ports = jlink_ports()
    port = (a.port or find_vcom0(ports)).replace("/dev/tty.", "/dev/cu.", 1)
    if port == FORBIDDEN or port not in [name for _, name in ports]:
        sys.exit(f"{port} 는 DK(J-Link) 포트가 아니라 열지 않습니다.")

    print(f"▶ {head['scene']} → {port}  ({frames[-1]['t'] - frames[0]['t']:.0f}초, {a.speed:g}배속)")
    for line in head.get("story", []):
        print(f"   {line}")
    with serial.Serial(port, BAUD, timeout=1, write_timeout=2, exclusive=True) as ser:
        try:
            while True:
                t0, w0 = stream[0][0], time.monotonic()      # --loop 도 매 바퀴 시작 프레임부터 다시
                last_print = -1
                for t, data in stream:
                    wait = w0 + (t - t0) / a.speed - time.monotonic()
                    if wait > 0:
                        time.sleep(wait)
                    ser.write(data)
                    if int(t) // 30 != last_print:
                        last_print = int(t) // 30
                        print(f"   {t:.0f}초", flush=True)
                if not a.loop:
                    break
        except KeyboardInterrupt:
            print("\n멈춤")
        for _ in range(5):
            ser.write(encode_frame([None, None, None]))
            time.sleep(0.1)
    print("■ 끝")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
