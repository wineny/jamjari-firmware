#!/usr/bin/env bash
# 보드 추정 호스트 시험 (10/5 N): 같은 장면 → 보드 코드(fusion.cpp) 출력 = 맥 기준(합치기.py + push-pet-spots) 출력.
#   test/run_fusion.sh          # 장면 0 · 8
# 장면·60GHz 데이터는 f-60ghz 워크트리에서 읽기만 한다(JAMJARI_NATURAL 로 바꿀 수 있음).
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/../src"
NAT="${JAMJARI_NATURAL:-$DIR/../../nrf_jamjari_lm20-60GHz/dummy/자연}"
PY="${JAMJARI_PY:-$DIR/../../.venv/bin/python}"
OUT="${TMPDIR:-/tmp}/jamjari-fusion"
mkdir -p "$OUT"

clang++ -std=c++17 -O2 -Wall -Wextra -Wshadow -I "$SRC" \
	"$DIR/run_fusion.cpp" "$SRC/fusion.cpp" "$SRC/replay_frames.cpp" "$SRC/ld2450_parser.cpp" \
	-o "$OUT/run_fusion"

fail=0
for key in 0 8; do
	case $key in 0) stem=0-하룻밤전체 ;; 8) stem=8-고양이누리옆구리로 ;; esac
	"$PY" "$DIR/../dummy/보드재생.py" "$NAT/장면/$stem.jsonl" --g60 "$NAT/60GHz/$stem.jsonl" --dump "$OUT/$key.bin" >/dev/null
	"$OUT/run_fusion" "$OUT/$key.bin" >"$OUT/$key.board.txt"
	python3 "$DIR/fusion_ref.py" "$key" --src "$NAT" >"$OUT/$key.ref.txt"
	if diff -q "$OUT/$key.ref.txt" "$OUT/$key.board.txt" >/dev/null; then
		echo "✅ 장면 $key: $(grep -c '^S' "$OUT/$key.ref.txt")초 칸 상태 · 머문 자리 $(grep -c '^P' "$OUT/$key.ref.txt")개 모두 같음"
	else
		echo "❌ 장면 $key 다름 (처음 5줄):"
		diff "$OUT/$key.ref.txt" "$OUT/$key.board.txt" | head -5
		fail=1
	fi
done
# 동기화: 레이더+시각 프레임(34)이 12바이트 잘린 바로 뒤의 장면 끝(10)·60GHz(18)를 놓치지 않는가
python3 - "$OUT/sync.bin" <<'PY'
import sys
r = b"\xAA\xFF\x07\x00" + bytes(28) + b"\x55\xCC"
g = b"\xAA\xFF\x06\x00" + bytes(12) + b"\x55\xCC"
e = b"\xAA\xFF\x08\x00" + bytes(4) + b"\x55\xCC"
open(sys.argv[1], "wb").write(r + r[:22] + e + r[:10] + g + b"\x00\xAA" + r)
PY
got=$("$OUT/run_fusion" --types "$OUT/sync.bin" 2>/dev/null | tr '\n' ' ')
if [[ "$got" == "07 08 06 07 " ]]; then
	echo "✅ 동기화: 잘린 프레임 뒤 장면 끝·60GHz 살림 ($got)"
else
	echo "❌ 동기화: 07 08 06 07 이어야 하는데 $got"
	fail=1
fi
exit $fail
