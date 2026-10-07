/*
 * LD2450 프레임 동기화·파싱 구현. Zephyr 헤더를 하나도 쓰지 않는다 — 호스트(clang++)와
 * 타깃(nRF Connect SDK) 양쪽에서 그대로 컴파일된다.
 */

#include "ld2450_parser.h"

#include <cmath>
#include <cstring>

namespace Ld2450 {
namespace {

constexpr uint8_t kHeader[4] = { 0xAA, 0xFF, 0x03, 0x00 };
constexpr uint8_t kFooter[2] = { 0x55, 0xCC };

} /* namespace */

/* ld2450_점검.py decode_signed() */
int32_t DecodeSigned(uint8_t low, uint8_t high)
{
	int32_t value = ((high & 0x7F) << 8) | low;
	return (high & 0x80) ? value : -value;
}

/* ld2450_점검.py parse_frame() */
uint8_t ParseFrame(const uint8_t *frame, Target (&targets)[kTargetSlots])
{
	static const uint8_t kEmpty[8] = {};
	uint8_t live = 0;

	for (uint8_t i = 0; i < kTargetSlots; i++) {
		const uint8_t *b = frame + 4 + i * 8;
		Target &t = targets[i];

		if (memcmp(b, kEmpty, sizeof(kEmpty)) == 0) {
			t = {};
			continue;
		}
		t.present = true;
		t.x = static_cast<int16_t>(DecodeSigned(b[0], b[1]));
		t.y = static_cast<int16_t>(DecodeSigned(b[2], b[3]));
		t.speed = DecodeSigned(b[4], b[5]) * 10;
		t.resolution = static_cast<uint16_t>((b[7] << 8) | b[6]);
		t.distance = static_cast<uint32_t>(
			sqrtf(static_cast<float>(t.x) * t.x + static_cast<float>(t.y) * t.y));
		live++;
	}
	return live;
}

/* 한 바이트씩 받아 헤더 동기화 → 30바이트 채움 → 푸터 확인 */
bool FeedByte(FrameSyncState &state, uint8_t byte)
{
	if (state.fill < sizeof(kHeader)) {
		if (byte == kHeader[state.fill]) {
			state.frame[state.fill++] = byte;
		} else {
			/* 헤더 도중 어긋남: 이 바이트가 새 헤더의 시작일 수 있다 */
			state.dropped += state.fill;
			state.fill = 0;
			if (byte == kHeader[0]) {
				state.frame[state.fill++] = byte;
			} else {
				state.dropped++;
			}
		}
		return false;
	}

	state.frame[state.fill++] = byte;
	if (state.fill < kFrameLen) {
		return false;
	}

	if (state.frame[kFrameLen - 2] == kFooter[0] && state.frame[kFrameLen - 1] == kFooter[1]) {
		state.fill = 0;
		return true;
	}

	/*
	 * 푸터가 어긋났다. ld2450_점검.py read_frames() 는 이때 헤더 4바이트만 버리고
	 * 남은 바이트 안에서 다음 헤더를 다시 찾는다 — 통째로 버리면 그 26바이트 안에
	 * 숨어 있던 진짜 헤더(쓰레기 바이트에 우연히, 또는 이전 헤더 매칭이 잘못
	 * 걸렸을 때)를 놓친다. 같은 동작을 재현: 이미 채워 둔 30바이트 버퍼 안에서
	 * (원래 헤더 자리인 앞 4바이트는 제외하고) 다음 헤더를 찾아, 찾으면 그 뒤
	 * 바이트를 버퍼 앞으로 당겨 이어서 채운다.
	 */
	for (size_t p = sizeof(kHeader); p + sizeof(kHeader) <= kFrameLen; p++) {
		if (memcmp(&state.frame[p], kHeader, sizeof(kHeader)) == 0) {
			size_t remain = kFrameLen - p;
			memmove(state.frame, &state.frame[p], remain);
			state.fill = remain;
			state.dropped += p;
			return false;
		}
	}
	state.dropped += kFrameLen;
	state.fill = 0;
	return false;
}

} /* namespace Ld2450 */
