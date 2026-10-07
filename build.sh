#!/usr/bin/env bash
# 잠자리 펌웨어 빌드.
#
# 이 폴더 경로에 한글이 있어서 Matter(GN) 빌드가 깨진다
# (write_buildconfig_header.py 가 UnicodeDecodeError). CMake 가 소스 경로를 REALPATH 로
# 풀기 때문에 심볼릭 링크로도 못 피한다. 그래서 소스를 ASCII 경로로 복사해 거기서 빌드하고,
# 이 폴더의 build 는 그 빌드 디렉터리를 가리키는 링크로 둔다.
#
# 사용:  ./build.sh            # 증분 빌드
#        ./build.sh --pristine # 처음부터 다시
#        ./build.sh --replay [--pristine]  # 시연용: 레이더 대신 맥이 USB 로 더미를 넣는 빌드
#              (빌드 폴더가 따로다: <빌드 뿌리>/replay. 올리기는 ./flash.sh --replay --keep)
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"

REPLAY=0
ARGS=()
for a in "$@"; do
	case "$a" in
		--replay) REPLAY=1 ;;
		--pristine) ARGS+=("$a") ;;
		*) echo "사용법: $0 [--replay] [--pristine]"; exit 2 ;;
	esac
done
set -- ${ARGS[@]+"${ARGS[@]}"}
ROOT="${JAMJARI_BUILD_ROOT:-$HOME/ncs-build}"
if [[ "$REPLAY" == 1 ]]; then
	ROOT="$ROOT/replay"     # 시연 빌드는 늘 따로 (설정 캐시가 레이더용 빌드에 섞이지 않게)
fi
MIRROR="$ROOT/nrf_jamjari_lm20"
NCS=/opt/nordic/ncs/v3.4.1

mkdir -p "$MIRROR"
rsync -a --delete --exclude build --exclude build.sh "$SRC/" "$MIRROR/"

eval "$("$HOME/.local/bin/nrfutil" sdk-manager toolchain env --as-script sh --ncs-version v3.4.1)"
export ZEPHYR_BASE="$NCS/zephyr"

PRISTINE=()
if [[ "${1:-}" == "--pristine" ]]; then
	PRISTINE=(-p always)
fi

cd "$NCS"
# 레이더용 빌드는 EXTRA 값을 매번 비운다(시연 설정이 CMake 캐시에 남아 있어도 안 섞이게)
EXTRA=(-- "-DEXTRA_DTC_OVERLAY_FILE=" "-DEXTRA_CONF_FILE=")
if [[ "$REPLAY" == 1 ]]; then
	EXTRA=(-- "-DEXTRA_DTC_OVERLAY_FILE=$MIRROR/replay/replay.overlay" "-DEXTRA_CONF_FILE=$MIRROR/replay/replay.conf")
fi
west build ${PRISTINE[@]+"${PRISTINE[@]}"} -b nrf54lm20dk/nrf54lm20b/cpuapp -s "$MIRROR" -d "$MIRROR/build" ${EXTRA[@]+"${EXTRA[@]}"}

ln -sfn "$MIRROR/build" "$SRC/build"
echo "빌드 결과: $MIRROR/build  (링크: $SRC/build)"
