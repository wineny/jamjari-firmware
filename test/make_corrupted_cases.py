#!/usr/bin/env python3
"""고의로 망가뜨린 LD2450 바이트 스트림을 만든다 — 재동기화 테스트용 픽스처.

실제 녹화(자료/레이더-원본-0929.bin)에서 뽑은 진짜 프레임들을 재료로 써서, 세 가지
오염 케이스를 만든다. 산출물은 test/cases/*.bin 에 커밋해 두고(결정적으로 재생성
가능), compare.py 가 이 파일들에 대해 C++/파이썬 결과를 대조한다.

케이스
  1. junk_insert        : 정상 프레임 사이에 헤더 패턴이 아닌 쓰레기 바이트를 끼워 넣는다.
  2. header_cut_mid      : 헤더 4바이트 중 2바이트만 오고 그 뒤로 전혀 다른 정상 프레임이
                           바로 이어진다 (헤더 도중 끊김).
  3. footer_corrupt_embedded_header : 정상 프레임의 푸터를 깨뜨리면서, 그 안(오프셋10)에
                           새 헤더 AA FF 03 00 을 심어 둔다. 뒤이어 그 새 헤더가 요구하는
                           나머지 바이트(진짜 타깃 데이터 + 정상 푸터)를 붙여, "푸터가
                           깨지면 30바이트를 통째로 버릴지, 헤더 4바이트만 버리고 남은
                           바이트에서 새 헤더를 다시 찾을지"가 갈리는 지점을 정확히 겨냥한다.
                           ld2450_점검.py read_frames() 는 후자라서 이 프레임을 "복구"한다.

사용: make_corrupted_cases.py [출력 디렉터리 = ./cases]
"""

from __future__ import annotations

import sys
from pathlib import Path

HEADER = b"\xAA\xFF\x03\x00"
FOOTER = b"\x55\xCC"
FRAME_LEN = 30

REPO_ROOT = Path(__file__).resolve().parents[2]  # .../실기
RAW_BIN = REPO_ROOT / "자료" / "레이더-원본-0929.bin"


def load_clean_frames(path: Path) -> list[bytes]:
    data = path.read_bytes()
    start = data.find(HEADER)
    if start < 0:
        raise RuntimeError(f"헤더를 못 찾았습니다: {path}")
    body = data[start:]
    frames = [body[i : i + FRAME_LEN] for i in range(0, len(body) - FRAME_LEN + 1, FRAME_LEN)]
    for f in frames:
        assert f[:4] == HEADER and f[-2:] == FOOTER, "원본 프레임이 깨져 있습니다"
    return frames


def make_junk_insert(frames: list[bytes]) -> bytes:
    """정상 프레임 20개 사이, 10번째 뒤에 헤더 패턴이 아닌 쓰레기 37바이트를 끼운다."""
    chunk = frames[:20]
    junk = bytes((i * 37 + 5) % 251 for i in range(37))  # 0xAA/0xFF 조합이 안 나오게 고정 패턴
    assert HEADER not in junk, "쓰레기 바이트에 우연히 헤더가 섞였다 — 패턴을 바꿀 것"
    return b"".join(chunk[:10]) + junk + b"".join(chunk[10:])


def make_header_cut_mid(frames: list[bytes]) -> bytes:
    """정상 프레임 10개 뒤, 헤더 앞 2바이트(AA FF)만 오고 끊긴 뒤 다른 정상 프레임 10개가 이어진다."""
    lead = frames[:10]
    cut = HEADER[:2]  # AA FF 만 오고 03 00 은 안 옴
    tail = frames[10:20]
    return b"".join(lead) + cut + b"".join(tail)


def make_footer_corrupt_embedded_header(frames: list[bytes]) -> bytes:
    """10번째 프레임 자리에 '푸터 깨짐 + 오프셋10에 헤더 파묻힘' 블록을 심는다.

    파묻힌 헤더 뒤로 진짜 타깃 데이터(24바이트) + 정상 푸터(2바이트)가 오도록 만들어서,
    "헤더 4바이트만 버리고 재탐색"하면 이 프레임이 원래 있던 11번째 프레임과 같은
    내용으로 복구되게 짠다.
    """
    lead = frames[:10]
    donor = frames[11]  # 이 프레임의 타깃 페이로드를 "파묻힌 헤더" 뒤에 재사용한다
    payload = donor[4:28]  # 24바이트 = 타깃 3개 × 8바이트

    block = bytearray(FRAME_LEN)
    block[0:4] = HEADER  # 정상적으로 매칭되는 진짜 헤더 시작
    block[4:10] = bytes([0x11]) * 6  # 의미 없는 더미(버려질 부분)
    embed_offset = 10
    block[embed_offset : embed_offset + 4] = HEADER  # 파묻힌 헤더
    block[14:30] = payload[0:16]  # payload 앞 16바이트
    # block[28:30] 은 아래에서 명시적으로 "틀린 푸터"로 덮는다
    block[28:30] = b"\x00\x00"
    assert bytes(block[28:30]) != FOOTER

    phantom_tail = bytearray(10)
    phantom_tail[0:8] = payload[16:24]  # payload 나머지 8바이트
    phantom_tail[8:10] = FOOTER  # 파묻힌 헤더가 만드는 프레임의 진짜 푸터

    tail = frames[12:32]
    return b"".join(lead) + bytes(block) + bytes(phantom_tail) + b"".join(tail)


def main() -> int:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "cases"
    out_dir.mkdir(parents=True, exist_ok=True)

    frames = load_clean_frames(RAW_BIN)

    cases = {
        "junk_insert.bin": make_junk_insert(frames),
        "header_cut_mid.bin": make_header_cut_mid(frames),
        "footer_corrupt_embedded_header.bin": make_footer_corrupt_embedded_header(frames),
    }
    for name, data in cases.items():
        (out_dir / name).write_bytes(data)
        print(f"{name}: {len(data)}바이트")

    return 0


if __name__ == "__main__":
    sys.exit(main())
