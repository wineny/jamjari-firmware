/*
 * 침대 칸 규칙 — 「있음 유지」·자리 뜸·왼쪽/오른쪽 칸 움직임 사건 줄.
 * 호스트(clang++)와 타깃 양쪽에서 컴파일되는 순수 C++ (Zephyr 의존 없음). 설계: 설계-레이더2차.md
 *
 * 입력은 트래커(tracker.h)가 그 프레임을 반영한 뒤의 상태다. 이 모듈은 트래커를 바꾸지 않는다.
 *   - 칸마다 「있음 표시」(hold) 를 둔다. occ = 둘 중 하나라도 켜짐.
 *   - 침대 안에 점이 보이면 그 칸 표시를 켠다. 점이 그냥 사라지면 아무것도 안 한다(= 있음 유지).
 *   - 그 칸에서 온 점이 침대 밖에서 보이거나 가장자리 띠(좌우·발치, 머리맡 없음)에서 사라지면 그 칸 표시를
 *     끈다. 같은 칸을 켜 둔 다른 살아 있는 점이 있으면 끄지 않는다.
 *   - 같은 점이 빈 반대 칸으로 넘어가면 표시를 넘긴다(원래 칸 끔). 반대 칸이 이미 켜져 있으면 안 넘긴다.
 *   - 두 칸 다 꺼지고 leaveDelayMs 가 지나면 occ 0. 침대 안 점이 holdMaxMs 동안 없으면 둘 다 끈다.
 *   - 움직임은 칸별로 센다(gap 넘게 끊기면 다른 건, min 못 채우면 버림). 시간이 겹치면 LR 한 줄,
 *     한 칸이 끝나고 followMs 안에 다른 칸이 시작하면 뒤쪽 줄에 L>R / R>L.
 *
 * 침대 좌표가 모두 0 이면(미설정) 예전 규칙: 구역 안 트랙이 1개 이상이면 occ 1, 마지막으로 1개 이상이던
 * 때부터 leaveDelayMs 지나면 0 (예전 k_timer 와 같다 — 레이더가 끊겨 프레임이 안 와도 Tick 이 내린다).
 *
 * 「그 칸에서 온 점」 = 트래커 이름(nameIndex)이 같은 트랙이 마지막으로 침대 안에 있던 칸.
 * seen=false(이번 프레임에 안 잡혀 이름만 살려 둔) 트랙의 좌표는 쓰지 않는다.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "tracker.h"

namespace BedRules {

/* 침대 사각형(mm). 좌표계는 트래커와 같다(레이더 정면 +y). 경계값 포함. */
struct Bed {
	int32_t xmin;
	int32_t xmax;
	int32_t ymin;
	int32_t ymax;
	int32_t splitX; /* x < splitX 면 왼쪽(L), 아니면 오른쪽(R). 레이더에서 본 기준 */
	int32_t edgeMm; /* 가장자리 띠 폭 (좌우 두 변 + 발치 ymin, 머리맡 ymax 는 띠 없음) */
};

struct Config {
	Bed bed;
	int64_t leaveDelayMs; /* 두 칸 다 꺼진 뒤 occ 0 까지 */
	int64_t holdMaxMs; /* 안전장치: 침대 안 점이 이만큼 없으면 둘 다 끔 */
	int64_t moveGapMs; /* 이만큼 넘게 끊기면 다른 움직임 */
	int64_t moveMinMs; /* 이보다 짧은 움직임은 버림 */
	int64_t followMs; /* 이어짐 기준 */
};

/* 침대 좌표가 모두 0 이면 false → 예전 규칙 */
bool BedEnabled(const Bed &bed);

enum class Side : uint8_t { kNone, kL, kR };
/* kSeat·kSpot 은 보드 추정(fusion) 사건이다 — 이 모듈은 안 낸다(웹 전송 줄을 같이 쓰려고 여기 둠) */
enum class Kind : uint8_t { kOn, kOff, kMove, kAway, kSeat, kSpot };
/* 사건 줄 맨 끝 side 칸: 빈칸 / L / R / LR / L>R / R>L */
enum class SideTag : uint8_t { kNone, kL, kR, kLR, kLthenR, kRthenL };

struct Event {
	uint32_t seq;
	Kind kind;
	int64_t startMs;
	int64_t endMs;
	uint8_t size; /* move 만: 움직인 거리 구간 1~3, 나머지 0 */
	int32_t x;
	int32_t y;
	uint8_t count;
	SideTag side;
};

/* 한 번의 Update/Tick 이 낸 사건들 */
struct EventBuf {
	static constexpr size_t kMax = 8;
	Event ev[kMax];
	size_t n = 0;
	uint32_t overflow = 0;
};

/* 칸 하나의 진행 중 움직임 */
struct Episode {
	bool active;
	int64_t startMs;
	int64_t lastMs;
	int32_t x0;
	int32_t y0;
	int64_t maxDist2;
	uint8_t maxCount;
};

/* 닫혔지만 아직 안 낸 움직임 묶음(겹치는 칸 움직임을 모으는 중) */
struct Group {
	bool pending;
	int64_t startMs;
	int64_t endMs;
	uint8_t sides; /* bit0 = L, bit1 = R */
	int32_t x0;
	int32_t y0;
	int64_t maxDist2;
	uint8_t countL;
	uint8_t countR;
};

/* 트래커 이름별로 기억하는 것 */
struct TrackMemo {
	uint32_t nameIndex;
	Side owner; /* 이 점이 켜 둔 칸 */
	int32_t x; /* 마지막으로 본(seen) 자리 */
	int32_t y;
	int64_t lastSeenMs;
};

struct State {
	bool occupied = false;
	uint32_t seq = 0;

	bool hold[2] = {}; /* [0]=L, [1]=R */
	/* 칸이 꺼진 뒤 다시 켜지면 away 한 줄 */
	bool awayPending[2] = {};
	int64_t awayStartMs[2] = {};
	int32_t awayX[2] = {};
	int32_t awayY[2] = {};

	/* 마지막으로 칸 표시를 켠 점(on 줄의 자리) */
	Side lastSetSide = Side::kNone;
	int32_t lastSetX = 0;
	int32_t lastSetY = 0;

	int64_t allClearSinceMs = -1; /* 두 칸 다 꺼진 시각, 아니면 -1 */
	int64_t lastLiveMs = 0; /* 예전 규칙: 구역 안 트랙이 마지막으로 1개 이상이던 시각 */
	SideTag lastClearTag = SideTag::kNone; /* off 줄의 칸: 나간 칸, 안전장치로 둘 다 끄면 LR */
	int32_t lastClearX = 0;
	int32_t lastClearY = 0;
	int64_t lastInBedMs = 0; /* 침대 안에 점이 마지막으로 보인 시각 */
	int32_t lastInBedX = 0; /* 그 자리 (안전장치 off 줄의 x,y) */
	int32_t lastInBedY = 0;

	TrackMemo memo[Tracker::kMaxTracks] = {};
	size_t memoCount = 0;

	Episode ep[2] = {};
	Group group = {};
	bool prevEmitted = false; /* 이어짐 판정용: 마지막으로 낸 move */
	uint8_t prevSides = 0;
	int64_t prevEndMs = 0;
};

/* 프레임 하나를 반영한다(트래커 Update 직후). 반환값 = occupied. */
bool Update(State &state, const Config &cfg, const Tracker::State &tracker, int64_t nowMs, EventBuf &out);

/* 프레임이 안 와도 시간만 흐르게 한다(1초 틱). 반환값 = occupied. */
bool Tick(State &state, const Config &cfg, int64_t nowMs, EventBuf &out);

/* 사건 하나를 "JJ1,EV,..." 한 줄(줄바꿈 없음)로 쓴다. 반환값 = 쓴 글자 수(snprintf 와 같음). */
int FormatEvent(const Event &ev, char *buf, size_t len);

} /* namespace BedRules */
