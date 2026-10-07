#!/usr/bin/env bash
# 시연: 같은 더미 장면을 지도(브라우저)와 보드(USB)에 동시에 흘린다.
#   지도 = 맥이 그리는 침대 위 점(심사위원에게 「지금 무슨 상황인지」)
#   보드 = 진짜 Matter 신호 → SmartThings·조명 (「그래서 집이 어떻게 반응하는지」)
# 보드에는 시연 빌드가 올라가 있어야 한다: ./build.sh --replay && ./flash.sh --replay --keep
#
# 사용:  ./시연.sh                                   # 장면 1 (잠자리 들기)
#        ./시연.sh dummy/자연/장면/0-하룻밤전체.jsonl
# 보드 타이머는 실제 시간이라 배속 없이 1배로만 돌린다.
set -euo pipefail
cd "$(dirname "$0")"
SCENE="${1:-dummy/자연/장면/1-잠자리들기.jsonl}"
PY=../.venv/bin/python

"$PY" dummy/재생.py "$SCENE" --port 8770 &
MAP=$!
trap 'kill $MAP 2>/dev/null || true' EXIT
sleep 2
open -a Aside "http://localhost:8770"   # 지도는 화면을 열면 시작한다 (Aside 는 open -a 라야 탭이 보인다)
sleep 1
"$PY" dummy/보드재생.py "$SCENE"
sleep 3
