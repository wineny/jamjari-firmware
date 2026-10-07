#!/usr/bin/env python3
"""C++ 구역 필터+트래커(src/tracker.cpp, 호스트 빌드 run_tracker)와
파이썬 기준 구현(실기/트래커.py + 실기/레이더_지도.py 의 in_zone)을 같은 녹화에 넣고
프레임마다 살아 있는 트랙 수·이름·좌표·속도·seen 이 같은지 비교한다.

프레임 시각 가정: 녹화에 도착 시각이 없으므로 녹화마다 「균일 간격」으로 본다.
    now_ms = int(frame_index * interval_ms)   (C++ 쪽도 같은 식)
파이썬 트래커는 초 단위로 쓰도록 만들어졌지만, 경계값(정확히 1.5초)에서 부동소수 오차로
C++ 와 갈리지 않도록 ms 단위 시각 + Tracker(keep_s=KEEP_S*1000) 로 돌린다. 알고리즘은 그대로다.

추가 확인
  - tracker.h 상수(kGateMm·kKeepMs)가 트래커.py(GATE_MM·KEEP_S)와 같은지
  - 기본 구역이 Kconfig 기본값과 같은지(구역 값은 Kconfig 에서 읽는다)
  - C++ overflow(트랙 칸 부족)가 0 인지 — 0 이 아니면 파이썬과 동작이 달라진다

사용:
  compare_tracker.py <입력.bin> <interval-ms> [--zone default|none]   녹화 비교
  compare_tracker.py --ghost                                          유령 점 차단 확인
"""

from __future__ import annotations

import csv
import importlib.util
import re
import subprocess
import sys
import tempfile
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
FW_DIR = TEST_DIR.parent
REPO = FW_DIR.parent  # 실기/
CPP_BIN = TEST_DIR / "run_tracker"
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(TEST_DIR))

import 트래커  # noqa: E402
from reference_parser import _BytesSerial, load_reference_module  # noqa: E402


def load_map_module():
    """레이더_지도.py 의 in_zone 을 그대로 쓰기 위해 불러온다(서버는 main() 에서만 뜬다)."""
    spec = importlib.util.spec_from_file_location("radar_map", REPO / "레이더_지도.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def kconfig_zone() -> dict[str, int]:
    text = (FW_DIR / "Kconfig").read_text(encoding="utf-8")
    zone = {}
    for key in ("xmin", "xmax", "ymin", "ymax"):
        m = re.search(rf"config JAMJARI_ZONE_{key.upper()}\b.*?\n\s*default (-?\d+)", text, re.S)
        if not m:
            raise SystemExit(f"Kconfig 에서 JAMJARI_ZONE_{key.upper()} 기본값을 못 찾았습니다")
        zone[key] = int(m.group(1))
    return zone


def check_constants() -> bool:
    header = (FW_DIR / "src" / "tracker.h").read_text(encoding="utf-8")
    gate = int(re.search(r"kGateMm = (\d+)", header).group(1))
    keep = int(re.search(r"kKeepMs = (\d+)", header).group(1))
    ok = gate == 트래커.GATE_MM and keep == round(트래커.KEEP_S * 1000)
    mark = "✅" if ok else "❌"
    print(f"{mark} 상수  C++ gate={gate} keep={keep}ms  /  Python GATE_MM={트래커.GATE_MM} KEEP_S={트래커.KEEP_S}")
    return ok


def python_rows(bin_path: Path, interval_ms: float, zone: dict | None) -> list[dict[str, str]]:
    parser = load_reference_module()
    radar_map = load_map_module()
    radar_map.zone = zone
    tracker = 트래커.Tracker(gate_mm=트래커.GATE_MM, keep_s=트래커.KEEP_S * 1000)
    rows = []
    frames = parser.read_frames(_BytesSerial(bin_path.read_bytes()), seconds=2)
    for idx, (frame, _junk) in enumerate(frames):
        targets = parser.parse_frame(frame)
        now = int(idx * interval_ms)
        # 레이더_지도.py reader() 와 같은 두 줄
        inside = [p if p and radar_map.in_zone(p) else None for p in targets]
        tracks = tracker.update(inside, now)
        rows.append({
            "frame_index": str(idx),
            "now_ms": str(now),
            "raw_live": str(sum(1 for t in targets if t)),
            "alive": str(len(tracks)),
            "overflow": "0",
            "tracks": ";".join(
                f"{t['id']}:{t['x']}:{t['y']}:{t['speed']}:{int(t['seen'])}" for t in tracks),
        })
    return rows


def cpp_rows(bin_path: Path, interval_ms: float, zone: dict | None) -> tuple[list[dict[str, str]], str]:
    zone_arg = "none" if zone is None else f"{zone['xmin']},{zone['xmax']},{zone['ymin']},{zone['ymax']}"
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "cpp.csv"
        res = subprocess.run([str(CPP_BIN), str(bin_path), repr(interval_ms), zone_arg, str(out)],
                             check=True, capture_output=True, text=True)
        with out.open(newline="", encoding="utf-8") as f:
            return list(csv.DictReader(f)), res.stderr.strip().splitlines()[-1]


FIELDS = ["frame_index", "now_ms", "raw_live", "alive", "overflow", "tracks"]


def compare(bin_path: Path, interval_ms: float, zone: dict | None) -> tuple[bool, list[dict[str, str]]]:
    label = "구역 없음" if zone is None else f"구역 {zone}"
    print(f"── {bin_path.name} · {interval_ms:.4f}ms/프레임(균일 가정) · {label} ──")
    cpp, summary = cpp_rows(bin_path, interval_ms, zone)
    py = python_rows(bin_path, interval_ms, zone)
    ok = len(cpp) == len(py)
    print(f"  프레임 수  C++={len(cpp)}  Python={len(py)}")
    mismatches = 0
    for c, p in zip(cpp, py):
        diffs = [f for f in FIELDS if c[f] != p[f]]
        if diffs:
            mismatches += 1
            if mismatches <= 5:
                print(f"  ❌ frame {c['frame_index']}: {diffs}")
                print(f"     C++   : { {f: c[f] for f in diffs} }")
                print(f"     Python: { {f: p[f] for f in diffs} }")
    matched = min(len(cpp), len(py)) - mismatches
    ok = ok and mismatches == 0
    names = sorted({t.split(":")[0] for r in py for t in r["tracks"].split(";") if t})
    print(f"  일치 프레임 {matched}/{len(py)} · 등장 이름 {len(names)}개 {''.join(names)[:40]}")
    print(f"  C++ 요약: {summary}")
    print("  ✅ 일치" if ok else f"  ❌ 불일치 {mismatches}프레임")
    print()
    return ok, cpp


# ─── 유령 점 ───────────────────────────────────────────────────────────────
GHOST = (-650, 3814, 480)  # 실측 유령 점 (x mm, y mm, speed mm/s)


def encode_signed(v: int) -> bytes:
    """ld2450_점검.py decode_signed() 의 역: MSB 1 = 양수."""
    raw = abs(v) & 0x7FFF
    return bytes([raw & 0xFF, (raw >> 8) | (0x80 if v >= 0 else 0)])


def ghost_frame() -> bytes:
    x, y, speed = GHOST
    slot = encode_signed(x) + encode_signed(y) + encode_signed(speed // 10) + (360).to_bytes(2, "little")
    return b"\xAA\xFF\x03\x00" + slot + b"\x00" * 16 + b"\x55\xCC"


def ghost_check(zone: dict) -> bool:
    ok = True
    print(f"── 유령 점 {GHOST} 차단 확인 ──")
    radar_map = load_map_module()
    radar_map.zone = zone
    inside = radar_map.in_zone({"x": GHOST[0], "y": GHOST[1]})
    print(f"  {'✅' if not inside else '❌'} 기본 구역 {zone} 에서 in_zone(유령) = {inside}")
    ok &= not inside

    with tempfile.TemporaryDirectory() as tmp:
        synth = Path(tmp) / "ghost_only.bin"
        synth.write_bytes(ghost_frame() * 300)  # 유령만 300프레임(≈30초 @100ms)
        parser_frames = load_reference_module().parse_frame(ghost_frame())
        decoded = parser_frames[0]
        good = (decoded["x"], decoded["y"], decoded["speed"]) == GHOST
        print(f"  {'✅' if good else '❌'} 합성 프레임 디코드 = ({decoded['x']}, {decoded['y']}, {decoded['speed']})")
        ok &= good

        for z, want_alive, want_occ in ((zone, "0", "0"), (None, "1", "1")):
            same, rows = compare(synth, 100.0, z)
            ok &= same
            alive = {r["alive"] for r in rows}
            occ = {r["occupied"] for r in rows}
            good = alive == {want_alive} and occ == {want_occ}
            what = "기본 구역" if z else "구역 없음(예전 동작)"
            print(f"  {'✅' if good else '❌'} {what}: 트랙 수 {sorted(alive)}, Occupancy {sorted(occ)} "
                  f"(기대 {want_alive}/{want_occ})")
            ok &= good
    print()
    return ok


def recording_ghost_report(bin_path: Path, interval_ms: float, zone: dict) -> bool:
    """실제 녹화에서 speed=480 인 먼 점(y>ymax)이 기본 구역에서 트랙이 되지 않는지."""
    cpp, _ = cpp_rows(bin_path, interval_ms, zone)
    raw, _ = cpp_rows(bin_path, interval_ms, None)
    far480 = sum(1 for r in raw for t in r["tracks"].split(";")
                 if t and t.split(":")[3] == "480" and int(t.split(":")[2]) > zone["ymax"])
    outside = sum(1 for r in cpp for t in r["tracks"].split(";")
                  if t and not (zone["xmin"] <= int(t.split(":")[1]) <= zone["xmax"]
                                and zone["ymin"] <= int(t.split(":")[2]) <= zone["ymax"]))
    ghost_only = sum(1 for r in cpp if r["raw_live"] != "0" and r["alive"] == "0")
    ok = far480 > 0 and outside == 0
    print(f"── {bin_path.name} 의 유령 점 ──")
    print(f"  구역 없이 돌리면 y>{zone['ymax']}·speed=480 트랙이 {far480}프레임에 생김(녹화에 유령이 있다는 증거)")
    print(f"  기본 구역에서는 구역 밖 트랙 {outside}개 · 레이더 점은 있지만 사람 0명인 프레임 {ghost_only}개")
    print("  ✅ 걸러짐" if ok else "  ❌ 확인 실패")
    print()
    return ok


def main() -> int:
    zone = kconfig_zone()
    ok = check_constants()
    if sys.argv[1:] == ["--ghost"]:
        ok &= ghost_check(zone)
        return 0 if ok else 1
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    bin_path, interval = Path(sys.argv[1]), float(sys.argv[2])
    mode = sys.argv[4] if len(sys.argv) > 4 and sys.argv[3] == "--zone" else "default"
    same, _ = compare(bin_path, interval, None if mode == "none" else zone)
    ok &= same
    if mode == "ghost-report":
        ok &= recording_ghost_report(bin_path, interval, zone)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
