#!/usr/bin/env bash
# nRF54LM20 DK 의 로그 콘솔(USB VCOM1, 115200)을 연다.
#
# 포트는 nrfutil 이 「J-Link(SEGGER) 디버거가 달린 기기」라고 알려 준 것만 쓴다.
# /dev/cu.usbmodem1101 (다른 ESP32) 같은 다른 보드는 후보에 안 나오고, 직접 지정해도 거부한다.
#
# 사용:  ./log.sh                  # 읽기 전용으로 로그 보기 + logs/ 에 파일로 저장 (Ctrl+C 로 끝)
#        ./log.sh --shell          # 키 입력도 되는 screen 으로 열기 (Matter 셸 명령용. 끝: Ctrl+A 누르고 K, y)
#        ./log.sh /dev/cu.usbmodemXXXX   # 포트 직접 지정 (J-Link 포트일 때만 허용)
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
PY="$SRC/../.venv/bin/python3"
NRFUTIL="$HOME/.local/bin/nrfutil"
BAUD=115200

SHELL_MODE=0
WANT_PORT=""
for arg in "$@"; do
	case "$arg" in
		--shell) SHELL_MODE=1 ;;
		/dev/*) WANT_PORT="${arg/\/dev\/tty./\/dev\/cu.}" ;;
		*) echo "사용법: $0 [--shell] [/dev/cu.usbmodemXXXX]"; exit 2 ;;
	esac
done

# J-Link 기기의 VCOM0 포트 목록 (한 줄에 하나)
DKS="$("$NRFUTIL" device list --json 2>/dev/null | python3 "$SRC/tools/find_dk.py")"
CANDIDATES=()
while IFS=$'\t' read -r serial vcom0 all; do
	[[ -z "${serial:-}" ]] && continue
	if [[ "$vcom0" != "-" ]]; then
		CANDIDATES+=("$vcom0")
	else
		# vcom 번호를 모르면 그 기기의 첫 포트 (보통 VCOM0)
		CANDIDATES+=("${all%%,*}")
	fi
done <<< "$DKS"

if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
	echo "❌ J-Link 가 달린 DK 를 못 찾았습니다. USB(디버거 쪽)와 POWER 스위치를 확인하세요."
	echo "   지금 보이는 시리얼 포트 (이 중 J-Link 가 아닌 건 쓰지 않습니다):"
	ls /dev/cu.* 2>/dev/null | sed 's/^/     /'
	exit 1
fi

PORT=""
if [[ -n "$WANT_PORT" ]]; then
	ALL_DK_PORTS="$(printf '%s\n' "$DKS" | cut -f3 | tr ',' '\n')"
	if ! grep -qxF "$WANT_PORT" <<< "$ALL_DK_PORTS"; then
		echo "❌ $WANT_PORT 는 J-Link(DK) 포트가 아닙니다. 다른 보드일 수 있어 열지 않습니다."
		echo "   DK 포트: $(printf '%s ' $ALL_DK_PORTS)"
		exit 1
	fi
	PORT="$WANT_PORT"
elif [[ ${#CANDIDATES[@]} -eq 1 ]]; then
	PORT="${CANDIDATES[0]}"
else
	echo "DK 가 여러 대입니다. 번호를 고르세요:"
	select p in "${CANDIDATES[@]}"; do
		[[ -n "${p:-}" ]] && PORT="$p" && break
	done
fi

echo "DK 로그 포트: $PORT ($BAUD bps)"

if [[ "$SHELL_MODE" -eq 1 ]]; then
	exec screen "$PORT" "$BAUD"
fi

mkdir -p "$SRC/logs"
LOG_FILE="$SRC/logs/dk-$(date +%Y%m%d-%H%M%S).log"
echo "읽기 전용. 파일로도 저장: $LOG_FILE  (끝: Ctrl+C)"
echo "보드 RESET 버튼을 누르면 부팅 로그(QR 주소 포함)부터 다시 볼 수 있습니다."
echo "────────────────────────────────────────"
exec "$PY" - "$PORT" "$BAUD" "$LOG_FILE" <<'EOF'
import sys
import serial

port, baud, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
# exclusive(flock): 레이더_지도.py --source nrf 와 동시에 열리면 바이트를 나눠 먹어 둘 다 깨진다
with serial.Serial(port, baud, timeout=0.5, exclusive=True) as ser, open(path, "ab") as log:
    try:
        while True:
            data = ser.read(512)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                log.write(data)
                log.flush()
    except KeyboardInterrupt:
        pass
EOF
