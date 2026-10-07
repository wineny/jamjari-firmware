#!/usr/bin/env python3
"""C++ 호스트 파서(run_cpp_parser)와 파이썬 정답 파서(reference_parser.py)를
같은 .bin 에 대해 돌려 프레임 단위로 대조한다.

비교 항목: 프레임 수, 프레임별 슬롯 3개의 존재 여부·x·y·speed, 누적 버린 바이트 수.
불일치가 있으면 첫 몇 건을 표로 찍고 실패로 끝낸다(종료 코드 1).

사용: compare.py <입력.bin> [기대 프레임 수]
"""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
CPP_BIN = TEST_DIR / "run_cpp_parser"
REF_SCRIPT = TEST_DIR / "reference_parser.py"
PYTHON = TEST_DIR.parents[1] / ".venv" / "bin" / "python3"  # 실기/.venv


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def run_case(bin_path: Path, label: str) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    with tempfile.TemporaryDirectory() as tmp:
        cpp_csv = Path(tmp) / "cpp.csv"
        py_csv = Path(tmp) / "py.csv"

        subprocess.run([str(CPP_BIN), str(bin_path), str(cpp_csv)], check=True, capture_output=True)
        subprocess.run(
            [str(PYTHON), str(REF_SCRIPT), str(bin_path), str(py_csv)], check=True, capture_output=True
        )

        cpp_rows = read_csv_rows(cpp_csv)
        py_rows = read_csv_rows(py_csv)
    return cpp_rows, py_rows


FIELDS_TO_COMPARE = [
    "live_count",
    "t1_present", "t1_x", "t1_y", "t1_speed",
    "t2_present", "t2_x", "t2_y", "t2_speed",
    "t3_present", "t3_x", "t3_y", "t3_speed",
    "dropped",  # 누적 버린 바이트 (C++ FrameSyncState.dropped vs 파이썬 read_frames junk)
]


def compare(label: str, bin_path: Path, expected_frames: int | None) -> bool:
    cpp_rows, py_rows = run_case(bin_path, label)
    ok = True

    print(f"── {label} ({bin_path.name}) ──")
    print(f"  프레임 수  C++={len(cpp_rows)}  Python={len(py_rows)}", end="")
    if expected_frames is not None:
        print(f"  (예상 {expected_frames})", end="")
    print()

    if len(cpp_rows) != len(py_rows):
        ok = False
        print("  ❌ 프레임 수 불일치")
    if expected_frames is not None and len(py_rows) != expected_frames:
        ok = False
        print(f"  ❌ 파이썬 기준 프레임 수가 예상과 다릅니다 (구성 스크립트 확인 필요)")

    mismatches = 0
    for i, (c, p) in enumerate(zip(cpp_rows, py_rows)):
        diffs = [f for f in FIELDS_TO_COMPARE if c.get(f) != p.get(f)]
        if diffs:
            mismatches += 1
            if mismatches <= 5:
                print(f"  ❌ frame {i}: 불일치 필드 {diffs}")
                print(f"     C++   : { {f: c.get(f) for f in diffs} }")
                print(f"     Python: { {f: p.get(f) for f in diffs} }")
    if mismatches:
        ok = False
        print(f"  총 불일치 프레임 {mismatches}개")

    if ok:
        print("  ✅ 일치")
    print()
    return ok


def main() -> int:
    if len(sys.argv) < 2:
        print(f"사용법: {sys.argv[0]} <입력.bin> [기대 프레임 수]", file=sys.stderr)
        return 2
    bin_path = Path(sys.argv[1])
    expected = int(sys.argv[2]) if len(sys.argv) > 2 else None
    ok = compare(bin_path.stem, bin_path, expected)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
