/*
 * LD2450 프레임 동기화·파싱 — 호스트에서도 컴파일되는 순수 C++ (Zephyr 의존 없음).
 * 프레임 규격과 파싱 규칙은 ld2450_점검.py 의 parse_frame / decode_signed / read_frames 그대로다.
 * nrf_jamjari/test/ 의 호스트 테스트가 이 헤더를 그대로 include 해서 쓴다.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "ld2450.h"

namespace Ld2450 {

constexpr size_t kFrameLen = 30; /* 헤더 4 + 타깃 3 × 8 + 푸터 2 */

/* ld2450_점검.py decode_signed() 그대로 */
int32_t DecodeSigned(uint8_t low, uint8_t high);

/* ld2450_점검.py parse_frame() 그대로. 반환값은 present 슬롯 수(liveCount). */
uint8_t ParseFrame(const uint8_t *frame, Target (&targets)[kTargetSlots]);

/*
 * 바이트 스트림 → 30바이트 프레임 동기화 상태. 인스턴스마다 독립적이라
 * 호스트 테스트에서 여러 스트림(정상/오염)을 동시에 검증할 수 있다.
 */
struct FrameSyncState {
	uint8_t frame[kFrameLen] = {};
	size_t fill = 0;
	/* 프레임에 못 들어가고 버려진 바이트 누적 수 (ld2450_점검.py read_frames() 의 junk 와 같은 뜻).
	 * 항상 성립: 넣은 바이트 수 = 30 × 완성 프레임 수 + dropped + fill */
	uint32_t dropped = 0;
};

/*
 * 바이트 하나를 밀어 넣는다. state.frame 이 헤더 AA FF 03 00 로 시작해 30바이트를
 * 채우고 푸터 55 CC 로 끝나면 true 를 반환한다(state.frame 에 그 프레임이 담겨 있다).
 * 푸터가 어긋나면 ld2450_점검.py read_frames() 처럼 헤더 4바이트만 버리고 남은
 * 바이트 안에서 다음 헤더를 다시 찾는다(통째로 버리면 그 안에 숨은 진짜 헤더를 놓친다).
 */
bool FeedByte(FrameSyncState &state, uint8_t byte);

} /* namespace Ld2450 */
