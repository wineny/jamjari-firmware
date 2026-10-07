#!/usr/bin/env python3
"""시연용 「자연스러운 하룻밤」 가짜 장면 만들기 (10/3, 전부 더미로 가기로 한 뒤).

`../만들기.py` 의 시험 장면 17개(직선·정답 채점용)와 달리, 이건 보여 주기용이다.
  - 걸음은 지나는 점들을 부드럽게 잇는 곡선, 걷는 속도도 조금씩 바뀐다.
  - 점은 몇 cm 씩 떨리고, 가끔 한두 프레임 안 보인다.
  - 가만히 누워 있으면 안 보인다(LD2450 이 실제로 그렇다).
  - 레이더 시야(좌우 60도)·보드 구역 밖으로 나가면 안 보인다.
정답 채점 파일은 만들지 않는다. 대신 머리에 「이야기」(누가 언제 무엇을)를 적는다.
「0-하룻밤전체」 = 장면 7개를 이어 붙인 한 파일(시연·보드 재생용). 1~7 은 장면마다 빈 침대에서 새로 시작한다.

침실: 남편 = 왼쪽(L, 슬롯 0) · 누리 = 오른쪽(R, 슬롯 1) · 고양이 = 슬롯 2.
문은 오른쪽 발치 너머. 머리 쪽으로는 못 다닌다. 고양이는 발치에서 올라와 누리 머리맡을 좌우로 오가며 잔다.
슬롯 = 이 점의 진짜 주인(그림용). 보드는 슬롯 번호를 주인으로 쓰지 않는다.

실행:  python3 만들기.py          # 전부
       python3 만들기.py 3 5      # 고른 장면만
"""

import json
import math
import random
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "장면"
DT = 0.089                                   # 녹화 프레임 간격(초)
BED = {"xmin": -800, "xmax": 800, "ymin": 300, "ymax": 2300, "split_x": 0, "edge_mm": 300}
CONFIG = {"LEAVE_DELAY_MS": 10000, "HOLD_MAX_MIN": 720, "MOVE_GAP_MS": 1000,
          "MOVE_MIN_MS": 2000, "FOLLOW_MS": 30000}
DOOR = (1575, -800)
ACTORS = ["남편", "누리", "고양이"]

Pt = tuple[float, float]


# ── 곡선과 움직임 ─────────────────────────────────────────

def spline(pts: list[Pt], n: int = 40) -> list[Pt]:
    """지나는 점들을 부드럽게 잇는다(Catmull-Rom)."""
    p = [pts[0]] + pts + [pts[-1]]
    out = []
    for i in range(1, len(p) - 2):
        p0, p1, p2, p3 = p[i - 1], p[i], p[i + 1], p[i + 2]
        for k in range(n):
            t = k / n
            out.append(tuple(0.5 * ((2 * p1[j]) + (-p0[j] + p2[j]) * t
                                    + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t * t
                                    + (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t ** 3) for j in (0, 1)))
    out.append(pts[-1])
    return out


class Actor:
    """한 주인의 위치 기록. 프레임 번호 → (x, y, 떨림 mm)."""

    def __init__(self, rng: random.Random):
        self.pos: dict[int, tuple[float, float, float]] = {}
        self.rng = rng
        self.off = 0.0          # 하룻밤 이어 붙일 때 장면 시작 시각

    def walk(self, t0: float, pts: list[Pt], speed: float, shake: float = 25) -> float:
        """t0 부터 pts 를 따라 speed(mm/s) 로 걷는다. 속도는 ±15% 로 천천히 출렁인다. 끝 시각을 돌려준다."""
        line = spline(pts)
        cum = [0.0]
        for a, b in zip(line, line[1:]):
            cum.append(cum[-1] + math.dist(a, b))
        s, t, i, phase = 0.0, t0 + self.off, 0, self.rng.uniform(0, 6)
        while True:
            while i < len(cum) - 2 and cum[i + 1] < s:
                i += 1
            f = 0 if cum[i + 1] == cum[i] else min(1, (s - cum[i]) / (cum[i + 1] - cum[i]))
            x = line[i][0] + (line[i + 1][0] - line[i][0]) * f
            y = line[i][1] + (line[i + 1][1] - line[i][1]) * f
            self.pos[round(t / DT)] = (x, y, shake)
            if s >= cum[-1]:
                return t - self.off
            s += speed * (1 + 0.15 * math.sin(phase + t * 1.7)) * DT
            t += DT

    def toss(self, t0: float, at: Pt, dur: float, to: Pt, shake: float = 12) -> float:
        """누운 채 뒤척임: at 에서 to 로 dur 초 동안 몸이 옮겨 간다(중간에 살짝 되돌림)."""
        k0, k1 = round((t0 + self.off) / DT), round((t0 + self.off + dur) / DT)
        for k in range(k0, k1 + 1):
            f = (k - k0) / max(1, k1 - k0)
            g = f + 0.25 * math.sin(f * math.pi * 2) * (1 - f)       # 갔다가 살짝 돌아왔다가 다시
            self.pos[k] = (at[0] + (to[0] - at[0]) * g, at[1] + (to[1] - at[1]) * g, shake)
        return t0 + dur

    def get_up(self, t0: float, lie: Pt, edge: Pt) -> float:
        """일어나 앉아 가장자리로 천천히(약 0.2 m/s) 옮긴다."""
        t = self.toss(t0, lie, 1.5, (lie[0] + (edge[0] - lie[0]) * 0.15, lie[1]), shake=15)
        return self.walk(t + 0.6, [(lie[0] + (edge[0] - lie[0]) * 0.15, lie[1]), edge], 220, shake=20)


# ── 보이는지 ─────────────────────────────────────────────

def visible(x: float, y: float) -> bool:
    """레이더 시야 좌우 60도 · 보드 구역(끝값에서 100mm 안)."""
    return 100 <= y <= 2900 and abs(x) <= 1400 and math.degrees(math.atan2(abs(x), y)) <= 60


def frames(actors: list[Actor], dur: float, rng: random.Random):
    last: list[tuple | None] = [None] * len(actors)
    for k in range(int(dur / DT) + 1):
        targets = []
        for i, a in enumerate(actors):
            p = a.pos.get(k)
            if p is None or not visible(p[0], p[1]) or rng.random() < 0.03:   # 3% 는 잠깐 놓침
                targets.append(None)
                if p is None:
                    last[i] = None
                continue
            x = p[0] + rng.gauss(0, p[2])
            y = p[1] + rng.gauss(0, p[2])
            speed = 0
            if last[i] is not None:
                lk, lx, ly = last[i]
                speed = round((math.hypot(p[0], p[1]) - math.hypot(lx, ly)) / ((k - lk) * DT) / 10)
            last[i] = (k, p[0], p[1])
            targets.append({"x": round(x), "y": round(y), "speed": speed})
        yield {"t": round(k * DT, 3), "targets": targets}


# ── 자리 ─────────────────────────────────────────────────

HB_LIE, NY_LIE = (-420, 1450), (420, 1450)          # 누운 자리(가슴 쯤)
HB_EDGE, NY_EDGE = (-780, 1300), (780, 1300)        # 걸터앉는 가장자리
CAT_FOOT = (250, 450)                               # 발치 자리
CAT_HEAD = [(300, 2150), (620, 2130), (450, 2200)]  # 누리 머리맡, 오른쪽·왼쪽 오감

# 문 ↔ 누리 자리 (도면: 오른쪽으로 내려와 발치 쪽으로 돌아 문)
NY_TO_DOOR = [NY_EDGE, (1000, 1050), (1150, 650), (1250, 200), (1450, -300), DOOR]
# 문 ↔ 남편 자리 (도면: 왼쪽으로 내려와 발치를 돌아 문)
HB_TO_DOOR = [HB_EDGE, (-1000, 1000), (-1080, 500), (-850, 50), (-300, -250), (400, -400), (1100, -500), DOOR]


def rev(p: list[Pt]) -> list[Pt]:
    return list(reversed(p))


# ── 장면 7개 (하룻밤 순서) ────────────────────────────────

def s1(hb: Actor, ny: Actor, cat: Actor) -> tuple[float, list[str]]:
    t = ny.walk(0, rev(NY_TO_DOOR), 900)
    t = ny.walk(t + 1.5, [NY_EDGE, (650, 1400), NY_LIE], 250, shake=18)     # 올라가 눕기(느리게)
    t2 = hb.walk(14, rev(HB_TO_DOOR), 950)
    t2 = hb.walk(t2 + 2.0, [HB_EDGE, (-620, 1420), HB_LIE], 230, shake=18)
    return max(t, t2) + 25, [f"0초 누리가 문에서 들어와 오른쪽으로 올라와 누움({t:.0f}초)",
                             f"14초 남편이 발치를 돌아 왼쪽으로 올라와 누움({t2:.0f}초)"]


def s2(hb, ny, cat):
    t = cat.walk(3, [(500, -300), (200, 120), (120, 260)], 700, shake=15)          # 발치로 와서
    t = cat.walk(t + 1.2, [(120, 260), (180, 380)], 1500, shake=10)                 # 폴짝
    t = cat.walk(t + 0.5, [(180, 380), (320, 500), (180, 520), CAT_FOOT], 180, shake=12)  # 빙글 돌며 자리 잡기
    t = cat.toss(t + 4, CAT_FOOT, 3, (270, 440), shake=8)                           # 꾹꾹이
    return t + 30, ["3초 고양이가 발치로 와서 침대에 폴짝", f"자리 잡고 꾹꾹이 뒤 잠({t:.0f}초)"]


def s3(hb, ny, cat):
    hb.toss(20, HB_LIE, 5, (-380, 1500))
    ny.toss(70, NY_LIE, 4, (460, 1420))
    cat.toss(110, (270, 440), 2.5, (240, 470), shake=8)
    hb.toss(150, (-380, 1500), 3.5, (-450, 1380), shake=15)       # 돌아눕기(크게)
    ny.toss(200, (460, 1420), 5, (400, 1480))
    ny.toss(260, (400, 1480), 4.5, (-150, 1450), shake=18)        # 누리가 남편 쪽으로 넘어감
    ny.toss(320, (-150, 1450), 4, (420, 1460), shake=18)          # 다시 제자리로
    hb.toss(380, (-450, 1380), 4.5, (150, 1500), shake=18)        # 남편이 누리 쪽으로 넘어감
    hb.toss(440, (150, 1500), 4, (-420, 1450), shake=18)          # 다시 제자리로
    return 480, ["20초 남편 뒤척임", "70초 누리 뒤척임", "110초 고양이 살짝 움직임",
                 "150초 남편 돌아눕기(크게)", "200초 누리 뒤척임",
                 "260초 누리가 남편 쪽으로 넘어감 → 320초 돌아옴",
                 "380초 남편이 누리 쪽으로 넘어감 → 440초 돌아옴"]


def s4(hb, ny, cat):
    t = cat.walk(10, [(270, 440), (200, 900), (120, 1300), (230, 1750), CAT_HEAD[0]], 350, shake=15)
    t = cat.walk(t + 25, [CAT_HEAD[0], CAT_HEAD[1]], 120, shake=10)        # 누리 머리맡 오른쪽으로
    t = cat.walk(t + 40, [CAT_HEAD[1], CAT_HEAD[2], (250, 2120)], 110, shake=10)  # 다시 왼쪽으로
    t = cat.walk(t + 35, [(250, 2120), CAT_HEAD[2]], 100, shake=10)
    return t + 30, ["10초 고양이가 침대 위로 걸어 올라와 누리 머리맡에",
                    "머리맡에서 오른쪽·왼쪽으로 몇 번 옮겨 가며 잠"]


def s5(hb, ny, cat):
    t = ny.get_up(30, (400, 1480), NY_EDGE)
    t = ny.walk(t + 1.0, NY_TO_DOOR, 850)
    cat.toss(50, CAT_HEAD[2], 2, (520, 2180), shake=8)              # 고양이 살짝 고개
    hb.toss(200, (-450, 1380), 3, (-420, 1440))
    t = ny.walk(330, rev(NY_TO_DOOR), 800)
    t = ny.walk(t + 1.5, [NY_EDGE, (640, 1420), NY_LIE], 240, shake=18)
    return t + 25, ["30초 누리 일어나 화장실(문으로)", "200초 남편 뒤척임",
                    f"330초 누리 돌아와 다시 누움({t:.0f}초)"]


def s6(hb, ny, cat):
    t = hb.get_up(20, (-420, 1440), HB_EDGE)
    t = hb.walk(t + 1.5, HB_TO_DOOR, 900)
    ny.toss(60, NY_LIE, 3, (450, 1430))
    return 110, ["20초 남편 기상, 발치를 돌아 문으로", "60초 누리 뒤척임"]


def s7(hb, ny, cat):
    t = ny.get_up(20, (450, 1430), NY_EDGE)
    t = ny.walk(t + 1.0, NY_TO_DOOR, 900)
    t = cat.walk(60, [(520, 2180), (480, 1600), (350, 900), (250, 400), (150, 200), (300, -300)], 450, shake=15)
    return t + 40, ["20초 누리 기상, 문으로", "60초 고양이가 내려와 발치로 나감 → 빈 침대"]


SCENES = [
    ("1", "잠자리들기", s1), ("2", "고양이따라올라옴", s2), ("3", "자다가뒤척임", s3),
    ("4", "고양이머리맡으로", s4), ("5", "누리화장실다녀옴", s5), ("6", "남편먼저기상", s6),
    ("7", "누리기상고양이내려감", s7),
]


# ── 그림 ─────────────────────────────────────────────────

def draw(name: str, story: list[str], fr: list[dict], dur: float, path: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle
    plt.rcParams["font.family"] = ["AppleGothic"]
    plt.rcParams["axes.unicode_minus"] = False
    b, e = BED, BED["edge_mm"]
    fig, (ax, tl) = plt.subplots(2, 1, figsize=(7, 10.5), dpi=110, gridspec_kw={"height_ratios": [3, 1.1]})
    ax.add_patch(Rectangle((b["xmin"], b["ymin"]), 1600, 2000, fc="#eef5ff", ec="#5577aa", lw=1.5))
    for r in [(-800, 300, e, 2000), (800 - e, 300, e, 2000), (-800, 300, 1600, e), (-800, 2300 - e, 1600, e)]:
        ax.add_patch(Rectangle(r[:2], r[2], r[3], fc="#ffe9c7", ec="none", alpha=0.6))
    ax.plot([0, 0], [300, 2300], color="#5577aa", ls="--", lw=1)
    ax.text(-400, 2360, "남편 (L)", ha="center", fontsize=11, color="#2a6fdb")
    ax.text(400, 2360, "누리 (R)", ha="center", fontsize=11, color="#d9822b")
    for s in (-1, 1):
        ax.plot([0, s * 2900 * math.sin(math.pi / 3)], [0, 2900 * math.cos(math.pi / 3)], color="#ccc", lw=0.8)
    ax.plot(0, 0, marker="^", ms=12, color="#333")
    ax.text(0, -170, "레이더", ha="center", fontsize=9)
    ax.add_patch(Rectangle((1350, -820), 450, 50, fc="#c0392b"))
    ax.text(1575, -720, "문", ha="center", fontsize=10, color="#c0392b")
    colors = ["#2a6fdb", "#d9822b", "#8e44ad"]
    for slot in range(3):
        pts = [(f["t"], f["targets"][slot]) for f in fr if f["targets"][slot]]
        if pts:
            ax.scatter([p["x"] for _, p in pts], [p["y"] for _, p in pts], s=6, color=colors[slot],
                       alpha=0.55, label=ACTORS[slot])
    ax.legend(loc="upper left", fontsize=8)
    ax.set_title(f"{name}  ({dur:.0f}초)", fontsize=13)
    ax.text(-1550, -950, "\n".join(story), fontsize=8, va="top")
    ax.set_xlim(-1600, 1800)
    ax.set_ylim(-1400, 2600)
    ax.set_aspect("equal")
    ax.set_xlabel("x (mm)  ← 왼쪽 · 오른쪽 →")
    tl.axhspan(-800, 800, color="#eef5ff")
    tl.axhline(0, color="#5577aa", ls="--", lw=0.8)
    for slot in range(3):
        ts = [f["t"] for f in fr]
        xs = [f["targets"][slot]["x"] if f["targets"][slot] else float("nan") for f in fr]
        tl.plot(ts, xs, ".", ms=2, color=colors[slot])
    tl.set_xlim(0, dur)
    tl.set_ylim(-1500, 1500)
    tl.set_xlabel("시각 (초) — 점이 없는 곳 = 안 보임(누워 있거나 시야 밖)")
    tl.set_ylabel("x (mm)")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def night(rng: random.Random) -> tuple[list[Actor], float, list[str]]:
    """장면 7개를 한 파일로 이어 붙인 하룻밤. 앞 장면의 상태(누가 누워 있나)가 그대로 이어진다."""
    actors = [Actor(rng), Actor(rng), Actor(rng)]
    off, story = 0.0, []
    for n, name, fn in SCENES:
        for a in actors:
            a.off = off
        dur, lines = fn(*actors)
        story.append(f"[{off:.0f}초~] {n}. {name}")
        off += dur
    return actors, off, story


def write(stem: str, title: str, actors: list[Actor], dur: float, story: list[str], rng: random.Random) -> None:
    fr = list(frames(actors, dur, rng))
    head = {"scene": stem, "bed": BED, "config": CONFIG, "actors": ACTORS, "story": story,
            "note": "시연용 자연 장면. 슬롯 = 진짜 주인(그림용), 보드는 안 씀. 정답 채점 없음."}
    with open(OUT / f"{stem}.jsonl", "w") as f:
        f.write(json.dumps(head, ensure_ascii=False) + "\n")
        for x in fr:
            f.write(json.dumps(x, separators=(",", ":")) + "\n")
    draw(title, story, fr, dur, OUT / f"{stem}.png")
    seen = sum(1 for x in fr for p in x["targets"] if p)
    print(f"✅ {stem}  {dur:.0f}초  프레임 {len(fr)}  보인 점 {seen}")


def main() -> None:
    OUT.mkdir(exist_ok=True)
    pick = set(sys.argv[1:])
    if not pick or "0" in pick:
        rng = random.Random(4021)
        actors, dur, story = night(rng)
        write("0-하룻밤전체", "0. 하룻밤 전체", actors, dur, story, rng)
    for n, name, fn in SCENES:
        if pick and n not in pick:
            continue
        rng = random.Random(int(n) * 7919)
        actors = [Actor(rng), Actor(rng), Actor(rng)]
        dur, story = fn(*actors)
        write(f"{n}-{name}", f"{n}. {name}", actors, dur, story, rng)


if __name__ == "__main__":
    main()
