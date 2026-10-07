#!/usr/bin/env bash
# nRF54LM20 DK 에 잠자리 펌웨어를 올린다.
#
# 1. 빌드 결과가 있는지 확인 (없으면 ./build.sh 먼저)
# 2. 연결된 J-Link(DK)만 찾는다 — nrfutil device list 는 읽기 전용
#    다른 ESP32 같은 다른 USB 기기는 J-Link 가 아니라서 후보에 안 나온다
# 3. 어떤 DK 에 무엇을 할지 보여 주고 y 를 눌러야만 west flash 실행
#
# 사용:  ./flash.sh            # 처음 올릴 때: 칩 전체를 지우고 올림 (--erase)
#        ./flash.sh --recover  # "칩이 잠겨 있다(APPROTECT)" 고 나올 때
#        ./flash.sh --keep     # 지우지 않고 덮어쓰기 (이미 한 번 올린 뒤, 페어링 정보 유지)
#        ./flash.sh --replay --keep  # 시연 빌드(./build.sh --replay)를 올림
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
ROOT="${JAMJARI_BUILD_ROOT:-$HOME/ncs-build}"
if [[ "${1:-}" == "--replay" ]]; then
	ROOT="$ROOT/replay"
	shift
	echo "▶ 시연 빌드를 올립니다 (레이더 대신 맥 USB 로 더미를 받음)"
fi
BUILD="$ROOT/nrf_jamjari_lm20/build"
NCS=/opt/nordic/ncs/v3.4.1
NRFUTIL="$HOME/.local/bin/nrfutil"

MODE_ARGS=(--erase)
MODE_TEXT="칩 전체 지우고 올리기 (--erase)"
case "${1:-}" in
	"") ;;
	--recover) MODE_ARGS=(--recover); MODE_TEXT="잠금 해제 + 전체 지우고 올리기 (--recover)" ;;
	--keep) MODE_ARGS=(); MODE_TEXT="지우지 않고 덮어쓰기" ;;
	*) echo "사용법: $0 [--recover | --keep]"; exit 2 ;;
esac

if [[ ! -f "$BUILD/nrf_jamjari_lm20/zephyr/zephyr.signed.hex" ]]; then
	echo "빌드 결과가 없습니다: $BUILD"
	echo "먼저 ./build.sh 를 실행하세요."
	exit 1
fi

echo "연결된 DK(J-Link) 찾는 중…"
DKS="$("$NRFUTIL" device list --json 2>/dev/null | python3 "$SRC/tools/find_dk.py")"
COUNT=$(printf '%s' "$DKS" | grep -c . || true)

if [[ "$COUNT" -eq 0 ]]; then
	echo "❌ J-Link 가 달린 DK 를 못 찾았습니다."
	echo "   - USB 는 「nRF5340 USB」 라고 적힌 포트 말고, 디버거(J-Link)용 USB 포트에 꽂습니다."
	echo "   - DK 의 POWER 스위치가 ON 인지 확인합니다."
	echo "   지금 보이는 기기:"
	"$NRFUTIL" device list 2>/dev/null | sed 's/^/     /'
	exit 1
fi
if [[ "$COUNT" -gt 1 ]]; then
	echo "❌ DK 가 ${COUNT}대 보입니다. 하나만 꽂고 다시 실행하세요."
	printf '%s\n' "$DKS" | awk -F'\t' '{ print "   시리얼 " $1 "  포트 " $3 }'
	exit 1
fi

SERIAL=$(printf '%s' "$DKS" | cut -f1)
PORTS=$(printf '%s' "$DKS" | cut -f3)

echo
echo "  대상 DK   : J-Link 시리얼 $SERIAL  (포트 $PORTS)"
echo "  할 일     : $MODE_TEXT"
echo "  올릴 파일 : $BUILD  (mcuboot · factory data · 앱)"
echo
read -r -p "이 DK 에 올릴까요? [y/N] " ANSWER
if [[ "$ANSWER" != "y" && "$ANSWER" != "Y" ]]; then
	echo "취소했습니다. 아무것도 쓰지 않았습니다."
	exit 0
fi

eval "$("$NRFUTIL" sdk-manager toolchain env --as-script sh --ncs-version v3.4.1)"
export ZEPHYR_BASE="$NCS/zephyr"
cd "$NCS"
west flash -d "$BUILD" --dev-id "$SERIAL" ${MODE_ARGS[@]+"${MODE_ARGS[@]}"}

echo
echo "✅ 올리기 끝. 로그 보기: $SRC/log.sh"
