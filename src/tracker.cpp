/*
 * 구역 필터 + 트래커 구현. Zephyr 헤더를 쓰지 않는다 — 호스트(clang++)와 타깃 양쪽에서 컴파일된다.
 */

#include "tracker.h"

#include <cstdio>

namespace Tracker {
namespace {

constexpr char kNames[] = "ABCDEFGHJKLMNPQRSTUVWXYZ"; /* 트래커.py NAMES */
constexpr uint32_t kNameCount = sizeof(kNames) - 1;

struct Pair {
	int64_t dist2;
	uint8_t ti;
	uint8_t pi;
};

int64_t Dist2(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
	int64_t dx = ax - bx;
	int64_t dy = ay - by;
	return dx * dx + dy * dy;
}

} /* namespace */

bool InZone(const Zone &zone, int32_t x, int32_t y)
{
	return zone.xmin <= x && x <= zone.xmax && zone.ymin <= y && y <= zone.ymax;
}

size_t Update(State &state, const Ld2450::Target (&targets)[Ld2450::kTargetSlots], const Zone &zone,
	      int64_t nowMs)
{
	/* 레이더_지도.py: inside = [p if p and in_zone(p) else None ...]; 트래커.py: points = [p for p if p] */
	const Ld2450::Target *points[Ld2450::kTargetSlots];
	size_t pointCount = 0;
	for (const Ld2450::Target &t : targets) {
		if (t.present && InZone(zone, t.x, t.y)) {
			points[pointCount++] = &t;
		}
	}

	/*
	 * (거리, 트랙 순서, 점 순서) 로 정렬한 짝 목록. (ti, pi) 순서로 만들고 거리만으로
	 * 안정 정렬(삽입 정렬)하면 파이썬 sorted(tuple) 과 같은 순서가 된다.
	 */
	Pair pairs[kMaxTracks * Ld2450::kTargetSlots];
	size_t pairCount = 0;
	for (size_t ti = 0; ti < state.count; ti++) {
		for (size_t pi = 0; pi < pointCount; pi++) {
			Pair p = { Dist2(state.tracks[ti].x, state.tracks[ti].y, points[pi]->x, points[pi]->y),
				   static_cast<uint8_t>(ti), static_cast<uint8_t>(pi) };
			size_t j = pairCount++;
			while (j > 0 && pairs[j - 1].dist2 > p.dist2) {
				pairs[j] = pairs[j - 1];
				j--;
			}
			pairs[j] = p;
		}
	}

	constexpr int64_t kGate2 = static_cast<int64_t>(kGateMm) * kGateMm;
	bool usedTrack[kMaxTracks] = {};
	bool usedPoint[Ld2450::kTargetSlots] = {};
	for (size_t i = 0; i < pairCount; i++) {
		const Pair &p = pairs[i];
		if (p.dist2 > kGate2 || usedTrack[p.ti] || usedPoint[p.pi]) {
			continue;
		}
		usedTrack[p.ti] = true;
		usedPoint[p.pi] = true;
		Track &tr = state.tracks[p.ti];
		tr.x = points[p.pi]->x;
		tr.y = points[p.pi]->y;
		tr.speed = points[p.pi]->speed;
		tr.lastMs = nowMs;
		tr.seen = true;
	}

	for (size_t ti = 0; ti < state.count; ti++) {
		if (!usedTrack[ti]) {
			state.tracks[ti].seen = false;
		}
	}
	for (size_t pi = 0; pi < pointCount; pi++) {
		if (usedPoint[pi]) {
			continue;
		}
		if (state.count >= kMaxTracks) {
			state.overflow++;
			continue;
		}
		state.tracks[state.count++] = { state.nextName++, points[pi]->x, points[pi]->y, points[pi]->speed,
						nowMs, nowMs, true };
	}

	/* 순서를 지키며 오래된 트랙 제거 (트래커.py: t - last <= keep 만 남김) */
	size_t kept = 0;
	for (size_t ti = 0; ti < state.count; ti++) {
		if (nowMs - state.tracks[ti].lastMs <= kKeepMs) {
			state.tracks[kept++] = state.tracks[ti];
		}
	}
	state.count = kept;
	return state.count;
}

void FormatName(uint32_t nameIndex, char (&buf)[kNameBufLen])
{
	char letter = kNames[nameIndex % kNameCount];
	if (nameIndex < kNameCount) {
		snprintf(buf, sizeof(buf), "%c", letter);
	} else {
		snprintf(buf, sizeof(buf), "%c%lu", letter, static_cast<unsigned long>(nameIndex / kNameCount));
	}
}

} /* namespace Tracker */
