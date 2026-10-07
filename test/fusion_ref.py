#!/usr/bin/env python3
"""보드 추정(fusion) 기준 출력 — 맥 쪽 구현을 그대로 불러 한 줄 형식으로 찍는다 (10/5 N).

  칸 상태   = f-60ghz `dummy/자연/합치기.py` 의 run() 결과 그대로
  머문 자리 = `화면데이터.py` pack() 의 초당 점 이름 + `web/scripts/push-pet-spots.ts` stays() 를 파이썬으로 옮긴 것
f-60ghz 쪽 파일은 읽기만 한다(고치지 않음, .pyc 도 안 씀). 보드 호스트 시험(run_fusion)이 같은 형식으로 찍어 비교한다.

출력 (한 줄씩):
  S <초> <L상태> <L단서> <R상태> <R단서> <고양이자리> <조명>
     상태 = E 비어 있음 · H 사람 · P 반려동물로 추정 · U 모름(화면 「구분 안 됨」), 단서 = - · A · B · AB
  P <순위> <from초> <to초> <x> <y> <머문초>

실행:  python3 test/fusion_ref.py 8                # 장면 8 (stdout)
       python3 test/fusion_ref.py 0 --src <dummy/자연 폴더>
"""

import argparse
import importlib
import math
import sys
from pathlib import Path

sys.dont_write_bytecode = True

DEFAULT_SRC = Path(__file__).resolve().parents[2] / "nrf_jamjari_lm20-60GHz/dummy/자연"
SCENES = {"0": "0-하룻밤전체", "8": "8-고양이누리옆구리로"}
STATE = {"비어 있음": "E", "사람": "H", "반려동물로 추정": "P", "모름": "U"}
# 고양이 자리: 합치기.py cat_at(region 문자열) → 짧은 글자
CAT_AT = {None: "-", "seat:L": "L", "seat:R": "R", "pet:머리맡": "head", "pet:발치": "foot", "bed": "bed", "out": "-"}

# push-pet-spots.ts 의 값 그대로
MIN_STAY_S = 600
MAX_Q = 3
SAME_SPOT_MM = 300
BRIEF_S = 60
PET_WHO = 2


def stays(sec: list[dict]) -> list[dict]:
    """push-pet-spots.ts stays() 를 그대로 옮김."""
    stops = []
    last = None
    for t, s in enumerate(sec):
        p = next((q for q in s["r"] if q[2] == PET_WHO), None)
        if p is None:
            continue
        if last and t - last["t"] > 1:
            stops.append({"at": last["t"], "x": last["x"], "y": last["y"], "until": t})
        last = {"t": t, "x": p[0], "y": p[1]}
    if last:
        stops.append({"at": last["t"], "x": last["x"], "y": last["y"], "until": len(sec) - 1})

    groups = []
    for s in (s for s in stops if s["until"] - s["at"] >= BRIEF_S):
        g = next((g for g in groups if math.hypot(s["x"] - g["x"], s["y"] - g["y"]) < SAME_SPOT_MM
                  and s["at"] - g["to"] <= MIN_STAY_S), None)
        if g:
            g["to"] = s["until"]
            g["still"] += s["until"] - s["at"]
        else:
            groups.append({"from": s["at"], "to": s["until"], "x": s["x"], "y": s["y"], "still": s["until"] - s["at"]})
    out = [g for g in groups if g["still"] >= MIN_STAY_S]
    out.sort(key=lambda g: -g["still"])           # JS sort 도 안정 정렬
    return out[:MAX_Q]


def clues(c: list[str]) -> str:
    return "".join(c) or "-"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("scene", choices=sorted(SCENES))
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    a = ap.parse_args()
    sys.argv = sys.argv[:1]                       # 이름붙이기-시험.py 가 불러올 때 sys.argv 를 NEAR·MARGIN 으로 읽는다
    sys.path.insert(0, str(a.src))
    hp = importlib.import_module("합치기")
    hd = importlib.import_module("화면데이터")
    stem = SCENES[a.scene]

    # 칸 상태: 합치기.py run() 을 그대로 돌린 결과 (통합/ 파일이 아니라 지금 코드로)
    rows = hp.run(stem)
    # 고양이 자리는 run() 결과의 pet.where 로는 원래 region 을 다시 못 찾으니 run() 을 한 번 더 따라 돈다
    cat_at = cat_trace(hp, stem)
    for r, c in zip(rows, cat_at):
        print(f"S {int(r['t'])} {STATE[r['L']['state']]} {clues(r['L']['clues'])} "
              f"{STATE[r['R']['state']]} {clues(r['R']['clues'])} {CAT_AT.get(c, c)} {int(r['light'])}")

    # 머문 자리: 화면데이터 pack() 의 초당 점(이름 포함). pack() 은 통합/<장면>.jsonl 길이를 쓰므로 rows 와 길이가 같아야 한다
    sec = hd.pack(stem)["sec"]
    if len(sec) != len(rows):
        sys.exit(f"통합/{stem}.jsonl 이 지금 합치기.py 결과와 길이가 다릅니다({len(sec)} vs {len(rows)}). 합치기.py 를 다시 돌리세요.")
    for i, g in enumerate(stays(sec), 1):
        print(f"P {i} {g['from']} {g['to']} {g['x']} {g['y']} {g['still']}")


def cat_trace(hp, stem: str) -> list:
    """run() 과 같은 순서로 돌며 초마다 judge 직전의 cat_at 을 모은다."""
    _, frames = hp.load(hp.HERE / "장면" / f"{stem}.jsonl")
    _, g60rows = hp.load(hp.HERE / "60GHz" / f"{stem}.jsonl")
    g60 = {}
    for r in g60rows:
        g60.setdefault(int(r["t"]), {})[r["side"]] = r
    m, out, sec = hp.Merger(), [], 0
    for f in frames:
        while f["t"] >= sec and sec in g60:
            m.judge(sec, g60[sec])
            out.append(m.cat_at)
            sec += 1
        m.radar(f["t"], [(p["x"], p["y"]) for p in f["targets"] if p])
    return out


if __name__ == "__main__":
    main()
