/*
 * HLK-LD2450 레이더 UART 리더 (256000bps 8N1).
 * 프레임 규격과 파싱 규칙은 실기/ld2450_점검.py 를 그대로 옮겼다.
 */

#pragma once

#include <cstdint>

namespace Ld2450 {

constexpr uint8_t kTargetSlots = 3;

struct Target {
	bool present; /* 슬롯 8바이트가 전부 0 이면 false (파이썬의 None) */
	int16_t x; /* mm */
	int16_t y; /* mm */
	int32_t speed; /* mm/s (원시값 × 10) */
	uint16_t resolution; /* mm */
	uint32_t distance; /* mm, sqrt(x^2 + y^2) */
};

/* 부팅 후 누적 수신 통계 */
struct Stats {
	uint32_t frames; /* 정상 프레임 수 */
	uint32_t droppedBytes; /* 프레임에 못 들어가고 버린 바이트 수 (잡음·깨진 프레임) */
	uint32_t queueDrops; /* 처리가 밀려 버린 정상 프레임 수 */
	int64_t lastFrameMs; /* 마지막 정상 프레임 시각(k_uptime_get), 아직 없으면 -1 */
};

/* 정상 프레임 하나마다 리더 스레드에서 불린다. liveCount = present 슬롯 수. */
using FrameHandler = void (*)(const Target (&targets)[kTargetSlots], uint8_t liveCount);

/* 프레임이 오든 안 오든 1초마다 같은 리더 스레드에서 불린다(FrameHandler 와 동시에 불리지 않는다). */
using TickHandler = void (*)(const Stats &stats, int64_t nowMs);

/* 재생 빌드만: 맥이 보낸 보드 추정용 프레임(replay_frames.h 의 06·07·08) 하나마다 리더 스레드에서 불린다.
 * 07(레이더+시각)은 FrameHandler 를 먼저 부른 뒤 이것도 부른다. */
using ReplayHandler = void (*)(const uint8_t *frame);

/* UART 를 256000bps 로 맞추고 수신을 시작한다. 0 이면 성공. */
int Init(FrameHandler handler, TickHandler tick, ReplayHandler replay = nullptr);

} /* namespace Ld2450 */
