#!/usr/bin/env python3
"""`nrfutil device list --json` 출력(stdin)에서 J-Link(SEGGER) 디버거가 달린 기기만 골라 낸다.

flash.sh / log.sh 가 같이 쓴다. 다른 ESP32(/dev/cu.usbmodem1101)나 CP2102 같은
다른 USB 시리얼은 J-Link 가 아니라서 여기서 절대 안 나온다.

출력: 기기 하나당 한 줄, 탭 구분
    <J-Link 시리얼번호>\t<VCOM0 포트(/dev/cu.*) 또는 ->\t<그 기기의 모든 포트, 쉼표>
nRF54LM20 DK 는 로그 콘솔(uart20)이 VCOM1 로 나온다(9/30 실측, VCOM0 은 조용함).
두 번째 칸 이름은 옛 보드 호환을 위해 vcom0 이지만 실제로는 로그 포트다.
"""

import json
import sys


def cu(path: str) -> str:
    return path.replace("/dev/tty.", "/dev/cu.", 1)


def main() -> int:
    devices = []
    for line in sys.stdin:
        line = line.strip()
        if not line.startswith("{"):
            continue
        msg = json.loads(line)
        data = msg.get("data") or {}
        if msg.get("type") == "task_end":
            data = data.get("data") or {}
        if "devices" in data:
            devices = data["devices"]
    for dev in devices:
        traits = dev.get("traits") or {}
        if not (traits.get("jlink") or traits.get("seggerUsb")):
            continue
        ports = sorted(dev.get("serialPorts") or [], key=lambda p: (p.get("vcom") is None, p.get("vcom")))
        vcom0 = next((cu(p["comName"]) for p in ports if p.get("vcom") == 1), "-")
        print(f"{dev.get('serialNumber', '?')}\t{vcom0}\t{','.join(cu(p['comName']) for p in ports)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
