#!/usr/bin/env python3
"""레이더 2차 규칙 시험용 가짜 장면 만들기 (D).

정답 표 `정답표.md`(3판) 를 그대로 옮긴다. 장면마다 세 파일을 `장면/` 에 쓴다.
  NN-이름.jsonl       첫 줄 = 머리(침대·설정값), 그 아래 = 녹화와 같은 프레임 {"t", "targets"}
  NN-이름.정답.json   occ 바뀜 + 사건 줄 정답 (채점.py 가 읽음)
  NN-이름.png         침대 위에 점을 시간 색으로 찍은 그림

실행:  python3 만들기.py            # 전부
       python3 만들기.py 04 09      # 고른 장면만

점은 직선으로만 움직인다(흔들림 없음). 사람마다 슬롯을 고정한다(L=0, R=1, 고양이=2).
"""

import json
import math
import sys
from collections.abc import Iterator
from pathlib import Path

Pt = tuple[float, float]
Seg = tuple[float, float, Pt, Pt]
When = float | list[float]          # 시각 하나 또는 허용 창 [a, b]

HERE = Path(__file__).resolve().parent
OUT = HERE / "장면"
DT = 0.089                                  # 녹화 프레임 간격(초)

BED = {"xmin": -800, "xmax": 800, "ymin": 300, "ymax": 2300, "split_x": 0, "edge_mm": 300}
CONFIG = {"LEAVE_DELAY_MS": 10000, "HOLD_MAX_MIN": 720, "MOVE_GAP_MS": 1000,
          "MOVE_MIN_MS": 2000, "FOLLOW_MS": 30000}
TOL = {"t_s": 0.3, "xy_mm": 50}
# 보드는 1초 틱으로 타이머를 본다(B, 10/3) → 타이머로 생기는 바뀜(10초 대기 뒤 0)은 1초 늦을 수 있다
TIMER_LATE_S = 1.1


# ── 점 움직임 ─────────────────────────────────────────────

def seg(t0: float, t1: float, p0: Pt, p1: Pt) -> Seg:
    """t0 이상 t1 미만 동안 p0 → p1 직선. 끝에 닿으면 다음 토막이 이어받는다."""
    return (t0, t1, p0, p1)


def enter_l(t: float = 0.0) -> list[Seg]:
    """들어옴 L: 침대 밖 2초 → 침대 안 3초 → 누움(사라짐)."""
    return [seg(t, t + 2, (-1100, 1300), (-850, 1300)),
            seg(t + 2, t + 5, (-780, 1300), (-400, 1300))]


def exit_l(t: float) -> list[Seg]:
    """L 가운데에서 왼쪽으로 나감: 침대 안 3초 → 밖 2초 → 사라짐."""
    return [seg(t, t + 3, (-400, 1300), (-780, 1300)),
            seg(t + 3, t + 5, (-850, 1300), (-1100, 1300))]


def return_l(t: float) -> list[Seg]:
    """밖에서 2초 걸어와 침대 안 3초 → 누움."""
    return [seg(t, t + 2, (-1100, 1300), (-850, 1300)),
            seg(t + 2, t + 5, (-780, 1300), (-400, 1300))]


def exit_r(t: float) -> list[Seg]:
    return [seg(t, t + 3, (400, 1300), (780, 1300)),
            seg(t + 3, t + 5, (850, 1300), (1100, 1300))]


def fid_l(t: float, dur: float = 3.0) -> list[Seg]:
    return [seg(t, t + dur, (-400, 1500), (-350, 1500))]


def fid_r(t: float, dur: float = 3.0) -> list[Seg]:
    return [seg(t, t + dur, (400, 1500), (450, 1500))]


TWO_R = [seg(42, 45, (780, 1300), (400, 1300))]    # 둘이 잠: R 은 띠에서 바로 나타남


# ── 정답 줄 ──────────────────────────────────────────────

def mv(start: When, end: When, side: str, size: int | None = None, count: int | None = None,
       x: int | None = None, y: int | None = None) -> dict:
    return {"kind": "move", "start": start, "end": end, "side": side,
            "size": size, "count": count, "x": x, "y": y}


def ev(kind: str, at: When, side: str, x: int | None = None, y: int | None = None,
       start: When | None = None) -> dict:
    """on / off (start=end=at) · away (start~at)."""
    return {"kind": kind, "start": at if start is None else start, "end": at, "side": side, "x": x, "y": y}


def late(t: float) -> list[float]:
    """타이머로 생기는 시각: t 부터 1초 틱 늦음까지 허용."""
    return [t - TOL["t_s"], t + TIMER_LATE_S]


ENTER_L_EV = [ev("on", 2, "L", -780, 1300), mv(2, 5, "L", 3, 1, -780, 1300)]
TWO_EV = ENTER_L_EV + [mv(42, 45, "R", 3, 1, 780, 1300)]


# ── 장면 17개 (정답표.md 3판 + 14b) ────────────────────────────────

SCENES = [
    dict(n="01", name="가운데사라짐", dur=605,
         L=enter_l(),
         occ=[(2, 1)], occ_end=1, events=ENTER_L_EV),

    dict(n="02", name="옆으로나가밖에서보임", dur=120,
         L=enter_l() + exit_l(60),
         occ=[(2, 1), (late(73), 0)], occ_end=0,
         events=ENTER_L_EV + [mv(60, 63, "L", 3, 1, -400, 1300),
                              ev("off", late(73), "L", -850, 1300)]),

    dict(n="03", name="가장자리띠에서사라짐", dur=120,
         L=enter_l() + [seg(60, 63, (-400, 1300), (-720, 1300))],
         occ=[(2, 1), ([74.42 - TOL["t_s"], 74.42 + TIMER_LATE_S], 0)], occ_end=0,
         events=ENTER_L_EV + [mv(60, 63, "L", 3, 1, -400, 1300),
                              ev("off", [74.42 - TOL["t_s"], 74.42 + TIMER_LATE_S], "L", -720, 1300)]),

    dict(n="04", name="나갔다10초안에돌아옴", dur=130,
         L=enter_l() + exit_l(60) + return_l(68),
         occ=[(2, 1)], occ_end=1,
         events=ENTER_L_EV + [mv(60, 63, "L", 3, 1, -400, 1300),
                              ev("away", 70, "L", -850, 1300, start=63),
                              mv(70, 73, "L", 3, 1, -780, 1300)]),

    dict(n="05", name="화장실5분뒤돌아옴", dur=420,
         L=enter_l() + exit_l(60) + return_l(367),
         occ=[(2, 1), (late(73), 0), (369, 1)], occ_end=1,
         events=ENTER_L_EV + [mv(60, 63, "L", 3, 1, -400, 1300),
                              ev("off", late(73), "L", -850, 1300),
                              ev("on", 369, "L", -780, 1300),
                              ev("away", 369, "L", -850, 1300, start=63),
                              mv(369, 372, "L", 3, 1, -780, 1300)]),

    dict(n="06", name="아침기상발치로나감", dur=180,
         L=enter_l() + [seg(60, 63, (-400, 1300), (-350, 1300)),
                        seg(120, 123, (-400, 1300), (-200, 320)),
                        seg(123, 125, (-200, 250), (-150, 200))],
         occ=[(2, 1), (late(133), 0)], occ_end=0,
         events=ENTER_L_EV + [mv(60, 63, "L", 1, 1, -400, 1300),
                              mv(120, 123, "L", 3, 1, -400, 1300),
                              ev("off", late(133), "L", -200, 250)]),

    dict(n="07", name="왼쪽만", dur=200,
         L=enter_l() + fid_l(120), R=TWO_R,
         occ=[(2, 1)], occ_end=1,
         events=TWO_EV + [mv(120, 123, "L", 1, 1, -400, 1500)]),

    dict(n="08", name="이어서양쪽방향", dur=260,
         L=enter_l() + fid_l(120) + fid_l(217), R=TWO_R + fid_r(137) + fid_r(200),
         occ=[(2, 1)], occ_end=1,
         events=TWO_EV + [mv(120, 123, "L", 1, 1, -400, 1500),
                          mv(137, 140, "L>R", 1, 1, 400, 1500),
                          mv(200, 203, "R", 1, 1, 400, 1500),
                          mv(217, 220, "R>L", 1, 1, -400, 1500)]),

    dict(n="09", name="동시그뒤14초", dur=200,
         L=enter_l() + fid_l(120, 4), R=TWO_R + fid_r(122, 4) + fid_r(140),
         occ=[(2, 1)], occ_end=1,
         events=TWO_EV + [mv(120, 126, "LR", 1, 2, -400, 1500),
                          mv(140, 143, "R", 1, 1, 400, 1500)]),

    dict(n="10", name="40초뒤오른쪽", dur=230,
         L=enter_l() + fid_l(120), R=TWO_R + fid_r(163),
         occ=[(2, 1)], occ_end=1,
         events=TWO_EV + [mv(120, 123, "L", 1, 1, -400, 1500),
                          mv(163, 166, "R", 1, 1, 400, 1500)]),

    dict(n="11", name="너무짧은움직임", dur=120,
         L=enter_l() + [seg(60, 61.5, (-400, 1500), (-375, 1500))],
         occ=[(2, 1)], occ_end=1, events=ENTER_L_EV),

    dict(n="12", name="끊김0.6초와1.2초", dur=180,
         L=enter_l() + [seg(60, 62.5, (-400, 1500), (-370, 1500)),
                        seg(63.1, 65.6, (-370, 1500), (-340, 1500)),
                        seg(120, 122.5, (-400, 1500), (-370, 1500)),
                        seg(123.7, 126.2, (-370, 1500), (-340, 1500))],
         occ=[(2, 1)], occ_end=1,
         events=ENTER_L_EV + [mv(60, 65.6, "L", 1, 1, -400, 1500),
                              mv(120, 122.5, "L", 1, 1, -400, 1500),
                              mv(123.7, 126.2, "L", 1, 1, -370, 1500)]),

    dict(n="13", name="고양이침대밖만", dur=60,
         C=[seg(0, 15, (1000, 1200), (1350, 2400)),
            seg(15, 30, (1350, 2400), (900, 2500)),
            seg(33, 41.5, (-150, 200), (150, 200)),
            seg(41.5, 50, (150, 200), (0, 150))],
         occ=[], occ_end=0, events=[]),

    dict(n="14", name="안전장치1분", dur=200, config={"HOLD_MAX_MIN": 1},
         L=enter_l() + [seg(55, 58, (-400, 1500), (-350, 1500))],
         occ=[(2, 1), ([128 - TOL["t_s"], 128 + 2 * TIMER_LATE_S], 0)], occ_end=0,
         events=ENTER_L_EV + [mv(55, 58, "L", 1, 1, -400, 1500),
                              ev("off", [128 - TOL["t_s"], 128 + 2 * TIMER_LATE_S], "L")]),

    # 두 칸 다 켜진 채 안전장치: 타이머는 칸별이 아니라 「침대 안 아무 점」 기준 → 마지막 R(45) + 60 + 10
    dict(n="14b", name="안전장치1분두사람", dur=200, config={"HOLD_MAX_MIN": 1},
         L=enter_l(), R=TWO_R,
         occ=[(2, 1), ([115 - TOL["t_s"], 115 + 2 * TIMER_LATE_S], 0)], occ_end=0,
         events=TWO_EV + [ev("off", [115 - TOL["t_s"], 115 + 2 * TIMER_LATE_S], "LR")]),

    # 63.012 = 프레임 708 의 시각 → 이 프레임에서 x 가 정확히 0 (R 로 셈)
    dict(n="15", name="혼자칸넘김", dur=180,
         L=enter_l() + [seg(60, 63.012, (-400, 1500), (0, 1500)),
                        seg(63.012, 66, (0, 1500), (400, 1500))] + exit_r(120),
         occ=[(2, 1), (late(133), 0)], occ_end=0,
         events=ENTER_L_EV + [mv(60, 63, "L", 3, 1, -400, 1500),
                              mv(63, 66, "L>R", 3, 1, 0, 1500),
                              mv(120, 123, "R", 3, 1, 400, 1300),
                              ev("off", late(133), "R", 850, 1300)]),

    dict(n="16", name="둘중한명만나감", dur=260,
         L=enter_l() + exit_l(120), R=TWO_R + exit_r(200),
         occ=[(2, 1), (late(213), 0)], occ_end=0,
         events=TWO_EV + [mv(120, 123, "L", 3, 1, -400, 1300),
                          mv(200, 203, "R", 3, 1, 400, 1300),
                          ev("off", late(213), "R", 850, 1300)]),
]


# ── 프레임 만들기 ─────────────────────────────────────────

def point_at(segs: list[Seg], t: float) -> tuple | None:
    for t0, t1, (x0, y0), (x1, y1) in segs:
        if t0 - 1e-9 <= t < t1 - 1e-9:
            f = (t - t0) / (t1 - t0)
            return x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, (t1 - t0), (x0, y0, x1, y1)
    return None


def radial_speed(x0: float, y0: float, x1: float, y1: float, dur: float) -> int:
    """레이더 쪽으로 오면 음수 (cm/s, 대략값 — 규칙은 안 씀)."""
    return round((math.hypot(x1, y1) - math.hypot(x0, y0)) / dur / 10)


def frames(sc: dict) -> Iterator[dict]:
    slots = [sc.get("L", []), sc.get("R", []), sc.get("C", [])]
    for k in range(int(sc["dur"] / DT) + 1):
        t = round(k * DT, 3)
        targets = []
        for segs in slots:
            p = point_at(segs, t)
            if p is None:
                targets.append(None)
            else:
                x, y, dur, (x0, y0, x1, y1) = p
                targets.append({"x": round(x), "y": round(y), "speed": radial_speed(x0, y0, x1, y1, dur)})
        yield {"t": t, "targets": targets}


def check_view(sc: dict) -> None:
    """모든 점이 레이더 시야(좌우 60도)·보드 구역(끝값에서 100mm 안) 안인지."""
    for fr in frames(sc):
        for p in fr["targets"]:
            if not p:
                continue
            ang = math.degrees(math.atan2(abs(p["x"]), p["y"]))
            assert ang <= 60, f'{sc["n"]} t={fr["t"]} 시야 밖 {p} ({ang:.0f}도)'
            assert abs(p["x"]) <= 1400 and 100 <= p["y"] <= 2900, f'{sc["n"]} t={fr["t"]} 구역 끝 {p}'


# ── 그림 ─────────────────────────────────────────────────

def draw(sc: dict, path: str) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle
    plt.rcParams["font.family"] = ["AppleGothic"]
    plt.rcParams["axes.unicode_minus"] = False

    b = BED
    fig, (ax, tl) = plt.subplots(2, 1, figsize=(7, 10.2), dpi=110, gridspec_kw={"height_ratios": [3, 1.15]})
    e = b["edge_mm"]
    ax.add_patch(Rectangle((b["xmin"], b["ymin"]), b["xmax"] - b["xmin"], b["ymax"] - b["ymin"],
                           fc="#eef5ff", ec="#5577aa", lw=1.5))
    for r in [(b["xmin"], b["ymin"], e, b["ymax"] - b["ymin"]), (b["xmax"] - e, b["ymin"], e, b["ymax"] - b["ymin"]),
              (b["xmin"], b["ymin"], b["xmax"] - b["xmin"], e), (b["xmin"], b["ymax"] - e, b["xmax"] - b["xmin"], e)]:
        ax.add_patch(Rectangle(r[:2], r[2], r[3], fc="#ffe9c7", ec="none", alpha=0.7))
    ax.plot([b["split_x"]] * 2, [b["ymin"], b["ymax"]], color="#5577aa", ls="--", lw=1)
    ax.text(-400, b["ymax"] + 60, "L", ha="center", fontsize=14, color="#5577aa")
    ax.text(400, b["ymax"] + 60, "R", ha="center", fontsize=14, color="#5577aa")
    for s in (-1, 1):   # 시야 60도
        ax.plot([0, s * 2900 * math.sin(math.pi / 3)], [0, 2900 * math.cos(math.pi / 3)], color="#ccc", lw=0.8)
    ax.plot(0, 0, marker="^", ms=12, color="#333")
    ax.text(0, -170, "레이더", ha="center", fontsize=9)

    pts = [(fr["t"], p) for fr in frames(sc) for p in fr["targets"] if p]
    if pts:
        sct = ax.scatter([p["x"] for _, p in pts], [p["y"] for _, p in pts], c=[t for t, _ in pts],
                         cmap="viridis", s=9, vmin=0, vmax=sc["dur"])
        fig.colorbar(sct, ax=ax, fraction=0.04, pad=0.02, label="시각 (초)")
    # 보이는 토막마다 시작 시각 글자 + 끝 화살표 (같은 길을 오가도 순서가 보이게)
    for slot, segs in enumerate([sc.get("L", []), sc.get("R", []), sc.get("C", [])]):
        for k, (t0, t1, p0, p1) in enumerate(segs):
            joined = k > 0 and abs(segs[k - 1][1] - t0) < 1e-6
            if not joined:
                ax.annotate(f"{t0:g}s", p0, xytext=(4, 6 + 9 * slot), textcoords="offset points", fontsize=7.5,
                            color="#333", bbox={"fc": "white", "ec": "none", "alpha": 0.7, "pad": 0.5})
            ax.annotate("", p1, xytext=(p0[0] + (p1[0] - p0[0]) * 0.85, p0[1] + (p1[1] - p0[1]) * 0.85),
                        arrowprops={"arrowstyle": "-|>", "color": "#333", "lw": 0.8})

    lines = []
    for x in sc["events"]:
        when = f'@{fmt_t(x["end"])}' if x["kind"] in ("on", "off") else f'{fmt_t(x["start"])}~{fmt_t(x["end"])}'
        lines.append(f'{x["kind"]} {when} {x["side"]}')
    ax.set_title(f'{sc["n"]} {sc["name"]}  ({sc["dur"]}초)', fontsize=13)
    ax.text(-1500, -480, "정답: " + (" · ".join(lines) if lines else "사건 줄 없음"), fontsize=7.5, wrap=True, va="top")
    ax.set_xlim(-1600, 1600)
    ax.set_ylim(-700, 2900)
    ax.set_aspect("equal")
    ax.set_xlabel("x (mm)  ← 왼쪽 · 오른쪽 →")
    ax.set_ylabel("y (mm) 레이더에서 멀어짐")
    timeline(sc, tl, b)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def timeline(sc: dict, tl, b: dict) -> None:
    """아래 칸: 시간 → x. 침대 L/R·띠를 가로 띠로, 정답 사건 시각을 세로선으로."""
    e = b["edge_mm"]
    tl.axhspan(b["xmin"], b["xmax"], color="#eef5ff")
    tl.axhspan(b["xmin"], b["xmin"] + e, color="#ffe9c7", alpha=0.7)
    tl.axhspan(b["xmax"] - e, b["xmax"], color="#ffe9c7", alpha=0.7)
    tl.axhline(b["split_x"], color="#5577aa", ls="--", lw=0.8)
    names = ["L 사람", "R 사람", "고양이"]
    colors = ["#2a6fdb", "#d9822b", "#888"]
    for slot in range(3):
        ts, xs = [], []
        for fr in frames(sc):
            p = fr["targets"][slot]
            ts.append(fr["t"])
            xs.append(p["x"] if p else float("nan"))
        if any(x == x for x in xs):
            tl.plot(ts, xs, color=colors[slot], lw=2, label=names[slot])
    kind_color = {"on": "#2a9d4b", "off": "#c0392b", "away": "#8e44ad", "move": "#bbb"}
    for x in sc["events"]:
        if x["kind"] == "move":
            continue
        for t in (x["end"] if isinstance(x["end"], list) else [x["end"]]):
            tl.axvline(t, color=kind_color[x["kind"]], lw=1, ls=":")
        tl.text(x["end"][0] if isinstance(x["end"], list) else x["end"], b["xmax"] + 120, x["kind"],
                color=kind_color[x["kind"]], fontsize=8)
    tl.set_xlim(0, sc["dur"])
    tl.set_ylim(-1600, 1600)
    tl.set_xlabel("시각 (초)")
    tl.set_ylabel("x (mm)")
    tl.legend(loc="lower right", fontsize=7)


def fmt_t(v: When) -> str:
    if isinstance(v, list):
        return f"{v[0]:g}~{v[1]:g}"
    return f"{v:g}"


# ── 쓰기 ─────────────────────────────────────────────────

def stem(sc: dict) -> str:
    return f'{sc["n"]}-{sc["name"]}'


def write(sc: dict) -> None:
    check_view(sc)
    config = {**CONFIG, **sc.get("config", {})}
    base = OUT / stem(sc)
    with open(f"{base}.jsonl", "w") as f:
        f.write(json.dumps({"scene": stem(sc), "bed": BED, "config": config}, ensure_ascii=False) + "\n")
        for fr in frames(sc):
            f.write(json.dumps(fr, separators=(",", ":")) + "\n")
    answer = {
        "scene": stem(sc), "tol": {**TOL, "timer_late_s": TIMER_LATE_S},
        "occ": [{"t": t, "v": v} for t, v in sc["occ"]],
        "occ_end": sc["occ_end"],
        "events": [{k: v for k, v in e.items() if v is not None} for e in sc["events"]],
        "note": "값 [a,b] = 그 사이면 맞음. 빠진 칸 = 판정 안 함. 같은 순간 줄 순서는 판정 안 함.",
    }
    Path(f"{base}.정답.json").write_text(json.dumps(answer, ensure_ascii=False, indent=1))
    draw(sc, f"{base}.png")
    print(f"✅ {stem(sc)}  프레임 {int(sc['dur'] / DT) + 1}")


def main() -> None:
    OUT.mkdir(exist_ok=True)
    pick = set(sys.argv[1:])
    for sc in SCENES:
        if not pick or sc["n"] in pick:
            write(sc)


if __name__ == "__main__":
    main()
