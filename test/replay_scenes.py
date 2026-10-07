#!/usr/bin/env python3
"""D 의 가짜 장면(jsonl)을 침대 칸 규칙(run_bed_rules)에 돌려 결과 JSON 을 쓴다.

장면 파일: 1번째 줄 = 머리 {"scene", "bed": {...}, "config": {...}}, 2번째 줄부터 프레임 {"t": 초, "targets": [...]}.
결과 파일: {"scene", "occ": [{"ms", "v"}...] (바뀐 순간만, 처음 0), "events": [JJ1,EV 줄...], "occ_end"}.
머리에 없는 설정값은 Kconfig 기본값. 구역은 Kconfig JAMJARI_ZONE_* 기본값.

사용:
  test/replay_scenes.py [장면 폴더] [--out 폴더] [--set FOLLOW_MS=10000 ...]
  기본 장면 폴더 = ../dummy-data/dummy/장면 (워크트리 옆), 기본 출력 = test/결과 (--set 이 있으면 test/결과-틀림)
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
BIN = HERE / "run_bed_rules"
CONFIG_KEYS = ["LEAVE_DELAY_MS", "HOLD_MAX_MIN", "MOVE_GAP_MS", "MOVE_MIN_MS", "FOLLOW_MS"]


def kconfig_defaults() -> dict[str, int]:
    text = (REPO / "Kconfig").read_text()
    out = {}
    for m in re.finditer(r"config JAMJARI_(\w+)\n(?:\t.*\n)*?\tdefault (-?\d+)", text):
        out[m.group(1)] = int(m.group(2))
    return out


def to_input(head: dict, frames: list[dict], defaults: dict[str, int], overrides: dict[str, int]) -> str:
    cfg = {k: defaults[k] for k in CONFIG_KEYS}
    cfg.update(head.get("config", {}))
    cfg.update(overrides)
    b = head["bed"]
    lines = [
        f"Z {defaults['ZONE_XMIN']} {defaults['ZONE_XMAX']} {defaults['ZONE_YMIN']} {defaults['ZONE_YMAX']}",
        f"C {b['xmin']} {b['xmax']} {b['ymin']} {b['ymax']} {b['split_x']} {b['edge_mm']} "
        f"{cfg['LEAVE_DELAY_MS']} {cfg['HOLD_MAX_MIN'] * 60 * 1000} {cfg['MOVE_GAP_MS']} {cfg['MOVE_MIN_MS']} "
        f"{cfg['FOLLOW_MS']}",
    ]
    for f in frames:
        pts = [p for p in f["targets"] if p]
        ms = round(f["t"] * 1000)
        xy = " ".join(f"{p['x']} {p['y']}" for p in pts)
        lines.append(f"F {ms} {len(pts)} {xy}")
    return "\n".join(lines) + "\n"


def run_scene(path: Path, defaults: dict[str, int], overrides: dict[str, int]) -> dict:
    rows = [json.loads(l) for l in path.read_text().splitlines() if l.strip()]
    head, frames = rows[0], rows[1:]
    proc = subprocess.run([str(BIN)], input=to_input(head, frames, defaults, overrides),
                          capture_output=True, text=True, check=True)
    if proc.stderr:
        sys.stderr.write(f"{path.name}: {proc.stderr}")
    occ, events = [], []
    for line in proc.stdout.splitlines():
        tag, rest = line.split(" ", 1)
        if tag == "OCC":
            ms, v = rest.split()
            occ.append({"ms": int(ms), "v": int(v)})
        elif tag == "EV":
            events.append(rest)
    return {"scene": head.get("scene", path.stem), "occ": occ, "events": events,
            "occ_end": occ[-1]["v"] if occ else 0}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("scenes", nargs="?", default=str(REPO.parent / "dummy-data" / "dummy" / "장면"))
    ap.add_argument("--out")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=값")
    args = ap.parse_args()

    overrides = {}
    for s in args.set:
        k, v = s.split("=", 1)
        if k not in CONFIG_KEYS:
            sys.exit(f"--set 은 {', '.join(CONFIG_KEYS)} 중 하나: {k}")
        overrides[k] = int(v)
    out_dir = Path(args.out) if args.out else HERE / ("결과-틀림" if overrides else "결과")
    out_dir.mkdir(parents=True, exist_ok=True)

    subprocess.run([str(HERE / "build_host.sh")], check=True, stdout=subprocess.DEVNULL)
    defaults = kconfig_defaults()
    scenes = sorted(p for p in Path(args.scenes).glob("*.jsonl"))
    if not scenes:
        sys.exit(f"장면 파일이 없습니다: {args.scenes}")
    for p in scenes:
        res = run_scene(p, defaults, overrides)
        name = p.name[:-len(".jsonl")]
        (out_dir / f"{name}.결과.json").write_text(json.dumps(res, ensure_ascii=False, indent=1) + "\n")
        print(f"{name}: occ 변화 {len(res['occ'])}개, 사건 {len(res['events'])}줄, 끝 occ {res['occ_end']}")
    if overrides:
        print(f"설정 덮어씀: {overrides}")
    print(f"결과: {out_dir}")


if __name__ == "__main__":
    main()
