#!/usr/bin/env python3
"""정답 기준(파이썬) 파서 러너 — ld2450_점검.py 의 read_frames/parse_frame/decode_signed 를
그대로 불러 .bin 을 같은 CSV 포맷으로 뽑는다. C++ 호스트 러너(run_cpp_parser)의 출력과
compare.py 가 프레임 단위로 비교한다.

사용: reference_parser.py <입력.bin> <출력.csv>
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]  # .../실기
CHECK_SCRIPT = REPO_ROOT / "ld2450_점검.py"


def load_reference_module():
    spec = importlib.util.spec_from_file_location("ld2450_check", CHECK_SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"모듈을 못 불러왔습니다: {CHECK_SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class _BytesSerial:
    """serial.Serial 흉내 — read(n) 만 있으면 되는 read_frames() 를 그대로 재사용하기 위한 어댑터."""

    def __init__(self, data: bytes) -> None:
        self._data = data
        self._pos = 0

    def read(self, n: int) -> bytes:
        chunk = self._data[self._pos : self._pos + n]
        self._pos += len(chunk)
        return chunk


def run(input_path: Path, output_path: Path) -> int:
    m = load_reference_module()
    data = input_path.read_bytes()
    ser = _BytesSerial(data)

    # read_frames() 는 time.time() 기준 deadline 으로 도는데, 데이터가 다 떨어지면
    # read()가 계속 b''를 내며 sleep(0.01)로 대기만 한다. 2초면 이 고정 크기 입력을
    # 소진하고도 남는 시간이라 안전하게 끝난다(무한 대기 없음).
    frames = list(m.read_frames(ser, seconds=2))

    with output_path.open("w", encoding="utf-8") as out:
        out.write(
            "frame_index,live_count,"
            "t1_present,t1_x,t1_y,t1_speed,"
            "t2_present,t2_x,t2_y,t2_speed,"
            "t3_present,t3_x,t3_y,t3_speed,dropped\n"
        )
        for idx, (frame, junk) in enumerate(frames):
            targets = m.parse_frame(frame)
            live = sum(1 for t in targets if t)
            cells = [str(idx), str(live)]
            for t in targets:
                if t:
                    cells += ["1", str(t["x"]), str(t["y"]), str(t["speed"])]
                else:
                    cells += ["0", "0", "0", "0"]
            cells.append(str(junk))  # 이 프레임까지 누적으로 버린 바이트
            out.write(",".join(cells) + "\n")

    print(f"프레임 {len(frames)}개, 마지막 누적 junk {frames[-1][1] if frames else 0}바이트", file=sys.stderr)
    return 0


def main() -> int:
    if len(sys.argv) != 3:
        print(f"사용법: {sys.argv[0]} <입력.bin> <출력.csv>", file=sys.stderr)
        return 2
    return run(Path(sys.argv[1]), Path(sys.argv[2]))


if __name__ == "__main__":
    sys.exit(main())
