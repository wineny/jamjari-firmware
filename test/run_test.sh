#!/usr/bin/env bash
# LD2450 C++ 파서(호스트 빌드) vs 파이썬 정답 파서(ld2450_점검.py) 전체 검증.
#
# 1. C++ 호스트 바이너리 빌드 (Zephyr 없이, clang++ c++17)
# 2. 실제 녹화(자료/레이더-원본-0929.bin)로 대조
# 3. 오염 입력 3종 생성 후 대조 (헤더 중간 끊김 / 푸터 깨짐+헤더 파묻힘 / 쓰레기 바이트 삽입)
# 4. 구역 필터+트래커(tracker.h) vs 파이썬(트래커.py + 레이더_지도.py in_zone) 프레임별 대조
#    + 유령 점 차단 확인 + 점유 판정(occupancy.h) 타임라인
#
# 사용: ./run_test.sh
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$DIR/../.."
RAW_BIN="$REPO/자료/레이더-원본-0929.bin"
PY="$REPO/.venv/bin/python3"

echo "== 1. 호스트 바이너리 빌드 =="
"$DIR/build_host.sh"
echo

echo "== 2. 오염 입력 픽스처 생성 =="
"$PY" "$DIR/make_corrupted_cases.py" "$DIR/cases"
echo

FAIL=0

echo "== 3. 실제 녹화 대조 (기대 프레임 1668개) =="
"$PY" "$DIR/compare.py" "$RAW_BIN" 1668 || FAIL=1
"$PY" "$DIR/compare.py" "$REPO/자료/레이더-원본-2인-0929.bin" 568 || FAIL=1   # 2~3명 동시 검출 포함

echo "== 4. 오염 입력 대조 =="
"$PY" "$DIR/compare.py" "$DIR/cases/junk_insert.bin" 20 || FAIL=1
"$PY" "$DIR/compare.py" "$DIR/cases/header_cut_mid.bin" 20 || FAIL=1
"$PY" "$DIR/compare.py" "$DIR/cases/footer_corrupt_embedded_header.bin" || FAIL=1

echo "== 5. 구역 필터 + 트래커 대조 (C++ vs 트래커.py) =="
# 녹화에 프레임 도착 시각이 없어서 녹화마다 균일 간격을 가정한다.
#   1인 녹화: 60초 / 1668프레임 ≈ 35.97ms (이전 회차부터 쓰던 가정)
#   2인 녹화: ≈ 0.106초/프레임 (568프레임 ≈ 60초)
# 간격을 바꿔도 C++ 와 파이썬이 같아야 하므로, 트랙이 끊기기 쉬운 250ms 로도 한 번 더 돌린다.
TRK="$DIR/compare_tracker.py"
TWO="$REPO/자료/레이더-원본-2인-0929.bin"
ONE_MS=$(awk 'BEGIN { printf "%.6f", 60000.0 / 1668 }')
"$PY" "$TRK" "$RAW_BIN" "$ONE_MS" || FAIL=1
"$PY" "$TRK" "$RAW_BIN" "$ONE_MS" --zone none || FAIL=1
"$PY" "$TRK" "$TWO" 106 --zone ghost-report || FAIL=1
"$PY" "$TRK" "$TWO" 106 --zone none || FAIL=1
"$PY" "$TRK" "$TWO" 250 --zone none || FAIL=1

echo "== 6. 유령 점 (-650, 3814, speed 480) 차단 =="
"$PY" "$TRK" --ghost || FAIL=1

if [[ "$FAIL" -eq 0 ]]; then
	echo "=== 전체 통과 ==="
else
	echo "=== 실패 있음 (위 ❌ 참조) ==="
fi
exit "$FAIL"
