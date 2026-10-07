#!/usr/bin/env python3
"""가짜 장면(또는 녹화)을 레이더 지도 화면으로 재생한다 (D).

원본 `실기/레이더_지도.html`·`실기/트래커.py` 는 읽기만 하고 고치지 않는다.
화면을 내보낼 때 작은 스크립트를 덧붙여 침대 칸(L/R·가장자리 띠)·장면 시각·정답 occ 를 겹쳐 그린다.

실행:  python3 재생.py 04                  # 장면 04 를 실제 속도로
       python3 재생.py 05 --speed 10       # 10배 빠르게
       python3 재생.py 01 --from 50        # 50초부터
       → 브라우저 http://localhost:8770   (이 맥에서만 열림, 끝내려면 Ctrl+C)
"""

import argparse
import json
import queue
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import importlib.util

HERE = Path(__file__).resolve().parent
MAP_DIR = HERE.parent.parent   # 이 저장소의 한 칸 위(트래커.py·레이더_지도.html 이 있는 폴더)
BOARD_ZONE = {"xmin": -1500, "xmax": 1500, "ymin": 0, "ymax": 3000}   # 보드 Kconfig 기본 구역

_spec = importlib.util.spec_from_file_location("트래커", MAP_DIR / "트래커.py")
_tracker_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tracker_mod)
Tracker = _tracker_mod.Tracker

clients: list[queue.Queue] = []
clients_lock = threading.Lock()
header: dict = {}

# 원본 화면 위에 덧그리는 스크립트. draw() 를 감싸 한 번 더 그린다(원본 파일은 그대로).
OVERLAY = """
<script>
let sceneInfo = null;
es.addEventListener('message', e => { const d = JSON.parse(e.data); if (d.scene_t !== undefined) sceneInfo = d; });
const _origDraw = draw;
draw = function () {
  _origDraw();
  if (!sceneInfo) return;
  const g = geom(), b = sceneInfo.bed, e = b.edge_mm;
  const rect = (x0, y0, x1, y1, fill, stroke) => {
    const [a, c] = [toPx(g, x0, y1), toPx(g, x1, y0)];
    if (fill) { ctx.fillStyle = fill; ctx.fillRect(a[0], a[1], c[0] - a[0], c[1] - a[1]); }
    if (stroke) { ctx.strokeStyle = stroke; ctx.lineWidth = 2; ctx.strokeRect(a[0], a[1], c[0] - a[0], c[1] - a[1]); ctx.lineWidth = 1; }
  };
  rect(b.xmin, b.ymin, b.xmax, b.ymax, 'rgba(140,155,255,0.10)', '#8c9bff');
  rect(b.xmin, b.ymin, b.xmin + e, b.ymax, 'rgba(255,180,107,0.12)');
  rect(b.xmax - e, b.ymin, b.xmax, b.ymax, 'rgba(255,180,107,0.12)');
  rect(b.xmin + e, b.ymin, b.xmax - e, b.ymin + e, 'rgba(255,180,107,0.12)');
  rect(b.xmin + e, b.ymax - e, b.xmax - e, b.ymax, 'rgba(255,180,107,0.12)');
  const [sx, sy0] = toPx(g, b.split_x, b.ymin), [, sy1] = toPx(g, b.split_x, b.ymax);
  ctx.setLineDash([5, 5]); ctx.strokeStyle = '#8c9bff'; ctx.beginPath(); ctx.moveTo(sx, sy0); ctx.lineTo(sx, sy1); ctx.stroke(); ctx.setLineDash([]);
  ctx.font = 'bold 16px Pretendard, sans-serif'; ctx.fillStyle = '#8c9bff'; ctx.textAlign = 'center';
  let p = toPx(g, (b.xmin + b.split_x) / 2, b.ymax); ctx.fillText('L', p[0], p[1] - 8);
  p = toPx(g, (b.xmax + b.split_x) / 2, b.ymax); ctx.fillText('R', p[0], p[1] - 8);
  ctx.textAlign = 'left'; ctx.font = '15px Pretendard, sans-serif'; ctx.fillStyle = '#e6e8f5';
  ctx.fillText(`▶ ${sceneInfo.scene} · ${sceneInfo.scene_t.toFixed(1)}초 / ${sceneInfo.dur.toFixed(0)}초 · ${sceneInfo.speed}배`, 16, 26);
  ctx.fillStyle = sceneInfo.want_occ ? '#6fe0a8' : '#ffb46b';
  ctx.fillText(`정답 occ: ${sceneInfo.want_occ ? '있음 (1)' : '비어 있음 (0)'}`, 16, 48);
  ctx.fillStyle = '#b8bde0'; ctx.font = '13px Pretendard, sans-serif';
  sceneInfo.recent.forEach((s, i) => ctx.fillText(s, 16, 70 + i * 18));
};
</script>
"""


def load_scene(arg: str) -> tuple[dict, list[dict], dict | None]:
    path = Path(arg)
    if not path.exists():
        hits = sorted((HERE / "장면").glob(f"{arg}-*.jsonl"))
        if not hits:
            raise SystemExit(f"❌ 장면 {arg} 를 못 찾았어요 (dummy/장면/{arg}-*.jsonl)")
        path = hits[0]
    lines = path.read_text().splitlines()
    head = json.loads(lines[0])
    if "targets" in head:            # 머리 없는 녹화 파일
        head, body = {"scene": path.stem}, lines
    else:
        body = lines[1:]
    frames = [json.loads(x) for x in body]
    t0 = frames[0]["t"]
    for fr in frames:
        fr["t"] = round(fr["t"] - t0, 3)
    answer_path = path.with_name(path.stem + ".정답.json")
    answer = json.loads(answer_path.read_text()) if answer_path.exists() else None
    return head, frames, answer


def mid(v: float | list[float]) -> float:
    return (v[0] + v[1]) / 2 if isinstance(v, list) else v


def want_occ(answer: dict | None, t: float) -> int:
    v = 0
    for c in (answer or {}).get("occ", []):
        if mid(c["t"]) <= t:
            v = c["v"]
    return v


def recent_events(answer: dict | None, t: float) -> list[str]:
    out = []
    for e in (answer or {}).get("events", []):
        if mid(e["end"]) <= t:
            when = f'@{mid(e["end"]):g}' if e["kind"] in ("on", "off") else f'{mid(e["start"]):g}~{mid(e["end"]):g}'
            out.append(f'정답 줄: {e["kind"]} {when} {e["side"]}')
    return out[-4:]


def broadcast(msg: dict) -> None:
    data = json.dumps(msg)
    with clients_lock:
        for q in clients:
            if q.qsize() < 50:
                q.put(data)


def player(head: dict, frames: list[dict], answer: dict | None, speed: float, start: float, loop: bool) -> None:
    bed = head.get("bed", {"xmin": 0, "xmax": 0, "ymin": 0, "ymax": 0, "split_x": 0, "edge_mm": 0})
    dur = frames[-1]["t"]
    while True:
        while not clients:                      # 화면이 열릴 때까지 기다렸다 시작
            time.sleep(0.2)
        tracker = Tracker()
        wall0 = time.time()
        for n, fr in enumerate(f for f in frames if f["t"] >= start):
            st = fr["t"]
            wait = wall0 + (st - start) / speed - time.time()
            if wait > 0:
                time.sleep(wait)
            now = time.time()
            inside = [p if p and in_zone(p) else None for p in fr["targets"]]
            ignored = [p for p in fr["targets"] if p and not in_zone(p)]
            tracks = []
            for tr in tracker.update(inside, st):
                if tr["seen"]:
                    tracks.append({**tr, "first": now - (st - tr["first"]) / speed})
            broadcast({"t": now, "targets": fr["targets"], "tracks": tracks, "ignored": ignored,
                       "zone": BOARD_ZONE, "frames": n + 1,
                       "scene": head.get("scene", "?"), "scene_t": st, "dur": dur, "speed": speed, "bed": bed,
                       "want_occ": want_occ(answer, st), "recent": recent_events(answer, st)})
        print(f"⏹  {head.get('scene')} 끝" + (" — 처음부터 다시" if loop else ""))
        if not loop:
            return
        start = 0.0


def in_zone(p: dict) -> bool:
    z = BOARD_ZONE
    return z["xmin"] <= p["x"] <= z["xmax"] and z["ymin"] <= p["y"] <= z["ymax"]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_: object) -> None:
        pass

    def do_POST(self) -> None:
        self.send_error(403)            # 재생 중엔 구역 바꾸기 없음

    def do_GET(self) -> None:
        if self.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            q: queue.Queue = queue.Queue()
            with clients_lock:
                clients.append(q)
            try:
                while True:
                    try:
                        data = q.get(timeout=5)
                    except queue.Empty:
                        data = json.dumps({"ping": True})
                    self.wfile.write(f"data: {data}\n\n".encode())
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with clients_lock:
                    clients.remove(q)
            return
        html = (MAP_DIR / "레이더_지도.html").read_text()
        body = html.replace("</body>", OVERLAY + "</body>").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("scene", help="장면 번호(04) 또는 jsonl 경로(녹화도 됨)")
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--from", dest="start", type=float, default=0.0, help="이 초부터")
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--loop", action="store_true", help="끝나면 처음부터 다시")
    a = ap.parse_args()
    head, frames, answer = load_scene(a.scene)
    threading.Thread(target=player, args=(head, frames, answer, a.speed, a.start, a.loop), daemon=True).start()
    print(f"▶ {head.get('scene')} · {frames[-1]['t']:.0f}초 · {a.speed}배 → http://localhost:{a.port} (화면을 열면 시작, Ctrl+C 로 끝)")
    try:
        ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
