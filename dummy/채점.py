#!/usr/bin/env python3
"""B 재생 결과를 D 정답과 비교해 장면별 통과/실패를 낸다 (D).

실행:  python3 채점.py                                   # radar-v2 test/결과/ 채점
       python3 채점.py --결과 ../../radar-v2/test/결과-틀림  # 일부러 틀린 결과 채점

정답 `장면/NN-이름.정답.json`:
  값이 [a, b] 면 그 사이면 맞음. 빠진 칸은 판정 안 함. 같은 순간 줄의 순서는 안 봄.
결과 `NN-이름.결과.json`: {"occ":[{"ms","v"}], "events":["JJ1,EV,seq,kind,start,end,size,x,y,count,flags,side"], "occ_end"}
"""

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_RESULT = HERE.parent / "test" / "결과"


def parse_ev(line: str) -> dict:
    f = line.split(",")
    if len(f) < 12 or f[0] != "JJ1" or f[1] != "EV":
        raise ValueError(f"사건 줄 형식이 아님: {line}")
    return {"kind": f[3], "start": int(f[4]) / 1000, "end": int(f[5]) / 1000, "size": int(f[6]),
            "x": int(f[7]), "y": int(f[8]), "count": int(f[9]), "side": f[11], "line": line}


def t_ok(want: float | list[float], got: float, tol: float) -> bool:
    if isinstance(want, list):
        return want[0] <= got <= want[1]
    return abs(got - want) <= tol


def ev_problems(want: dict, got: dict, tol: dict) -> list[str]:
    bad = []
    for k in ("start", "end"):
        if k in want and not t_ok(want[k], got[k], tol["t_s"]):
            bad.append(f"{k} {got[k]:.2f} (정답 {want[k]})")
    for k in ("size", "count"):
        if k in want and got[k] != want[k]:
            bad.append(f"{k} {got[k]} (정답 {want[k]})")
    for k in ("x", "y"):
        if k in want and abs(got[k] - want[k]) > tol["xy_mm"]:
            bad.append(f"{k} {got[k]} (정답 {want[k]})")
    return bad


def grade(answer: dict, result: dict) -> list[str]:
    """틀린 점 목록. 빈 목록이면 통과."""
    tol = answer["tol"]
    out = []

    # occ 바뀜: 정답 순서대로 하나씩 짝짓기
    got_occ = [(c["ms"] / 1000, c["v"]) for c in result.get("occ", [])]
    want_occ = answer["occ"]
    for i, w in enumerate(want_occ):
        if i >= len(got_occ):
            out.append(f"occ → {w['v']} @{w['t']} 가 없음")
            continue
        t, v = got_occ[i]
        if v != w["v"] or not t_ok(w["t"], t, tol["t_s"]):
            out.append(f"occ {i + 1}번째 바뀜: {v} @{t:.2f} (정답 {w['v']} @{w['t']})")
    for t, v in got_occ[len(want_occ):]:
        out.append(f"occ 남는 바뀜: {v} @{t:.2f}")
    if result.get("occ_end") != answer["occ_end"]:
        out.append(f"끝 occ {result.get('occ_end')} (정답 {answer['occ_end']})")

    # 사건 줄: 같은 kind·side 중 가장 잘 맞는 것과 짝짓기
    got = [parse_ev(x) for x in result.get("events", [])]
    used = set()
    for w in answer["events"]:
        cands = [(len(ev_problems(w, g, tol)), i) for i, g in enumerate(got)
                 if i not in used and g["kind"] == w["kind"] and g["side"] == w["side"]]
        if not cands:
            out.append(f"빠진 줄: {w['kind']} {w['side']} {w.get('start')}~{w.get('end')}")
            continue
        n, i = min(cands)
        used.add(i)
        if n:
            out.append(f"{w['kind']} {w['side']} {w.get('start')}~{w.get('end')}: " + ", ".join(ev_problems(w, got[i], tol)))
    for i, g in enumerate(got):
        if i not in used:
            out.append(f"남는 줄: {g['line']}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--결과", dest="result_dir", type=Path, default=DEFAULT_RESULT)
    ap.add_argument("--정답", dest="answer_dir", type=Path, default=HERE / "장면")
    a = ap.parse_args()

    answers = sorted(a.answer_dir.glob("*.정답.json"))
    passed = 0
    for ap_path in answers:
        stem = ap_path.name.removesuffix(".정답.json")
        res_path = a.result_dir / f"{stem}.결과.json"
        if not res_path.exists():
            print(f"⚪ {stem}: 결과 파일 없음")
            continue
        problems = grade(json.loads(ap_path.read_text()), json.loads(res_path.read_text()))
        if problems:
            print(f"❌ {stem}")
            for p in problems:
                print(f"     - {p}")
        else:
            passed += 1
            print(f"✅ {stem}")
    print(f"\n통과 {passed} / {len(answers)}  ({a.result_dir})")
    return 0 if passed == len(answers) else 1


if __name__ == "__main__":
    sys.exit(main())
