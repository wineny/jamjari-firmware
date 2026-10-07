/*
 * 구역 필터 + 사람 이어 붙이기(트래커) — 호스트에서도 컴파일되는 순수 C++ (Zephyr 의존 없음).
 *
 * 알고리즘은 실기/트래커.py 와 실기/레이더_지도.py 의 in_zone 을 그대로 옮겼다.
 *   - 구역 밖 점은 버린다(경계 포함 = 안). LD2450 은 아무것도 없는 약 4m 거리에
 *     speed=480 인 「유령 점」을 계속 내는데, 기본 구역(ymax 3000)이 이걸 거른다.
 *   - 매 프레임 기존 트랙과 새 점을 가까운 짝부터 묶는다(탐욕 매칭, 동점이면 트랙 순서 → 점 순서).
 *   - 직전 위치에서 kGateMm 넘게 튄 점은 같은 사람으로 보지 않는다.
 *   - kKeepMs 넘게 안 잡힌 트랙은 지운다. 그 사이엔 seen=false 로 이름을 살려 둔다.
 *   - 이름은 A, B, … (I·O 제외 24자), 한 바퀴 돌면 A1, B1, …
 *
 * 파이썬과 다른 점: 트랙 수를 kMaxTracks 로 고정했다(동적 할당 없음). 꽉 찼을 때
 * 새 점은 트랙을 만들지 않고 overflow 만 센다. 호스트 테스트가 두 녹화에서
 * overflow==0 인지(=파이썬과 동작이 같은 범위인지) 확인한다.
 * 거리 비교는 제곱 거리 정수로 한다(hypot 과 순서·게이트 판정이 같고 오차가 없다).
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "ld2450.h"

namespace Tracker {

constexpr int32_t kGateMm = 700; /* 트래커.py GATE_MM */
constexpr int64_t kKeepMs = 1500; /* 트래커.py KEEP_S × 1000 */
constexpr size_t kMaxTracks = 16;
constexpr size_t kNameBufLen = 12; /* "Z" + uint32 10자리 + NUL */

/* 지켜볼 사각형(mm). 레이더 정면이 +y, 레이더 기준 좌우가 x. 경계값 포함. */
struct Zone {
	int32_t xmin;
	int32_t xmax;
	int32_t ymin;
	int32_t ymax;
};

bool InZone(const Zone &zone, int32_t x, int32_t y);

struct Track {
	uint32_t nameIndex; /* 0 → "A" */
	int16_t x;
	int16_t y;
	int32_t speed;
	int64_t firstMs;
	int64_t lastMs;
	bool seen; /* 이번 프레임에 점이 붙었나 */
};

struct State {
	Track tracks[kMaxTracks] = {};
	size_t count = 0; /* 살아 있는 트랙 수 (seen=false 로 버티는 중인 것 포함) */
	uint32_t nextName = 0;
	uint32_t overflow = 0; /* 트랙 칸이 꽉 차서 못 만든 새 점 수(누적) */
};

/*
 * 프레임 하나를 반영한다. 구역 밖 슬롯은 무시한다. 반환값 = 살아 있는 트랙 수
 * (= state.count). 점유 판정은 이 값이 1 이상인지로 한다.
 */
size_t Update(State &state, const Ld2450::Target (&targets)[Ld2450::kTargetSlots], const Zone &zone,
	      int64_t nowMs);

/* 트래커.py _new_name() 과 같은 이름 문자열. */
void FormatName(uint32_t nameIndex, char (&buf)[kNameBufLen]);

} /* namespace Tracker */
