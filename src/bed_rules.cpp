/*
 * 침대 칸 규칙 구현. Zephyr 헤더를 쓰지 않는다 — 호스트(clang++)와 타깃 양쪽에서 컴파일된다.
 */

#include "bed_rules.h"

#include <cstdio>

namespace BedRules {
namespace {

constexpr int64_t kSize1Dist2 = 100 * 100; /* 움직인 거리 <10cm → size 1 (웹 기획 §10 임시값) */
constexpr int64_t kSize2Dist2 = 300 * 300; /* <30cm → 2, 그 이상 → 3 */
constexpr uint8_t kBitL = 1;
constexpr uint8_t kBitR = 2;

size_t Idx(Side s)
{
	return s == Side::kL ? 0 : 1;
}

SideTag TagOf(Side s)
{
	switch (s) {
	case Side::kL:
		return SideTag::kL;
	case Side::kR:
		return SideTag::kR;
	default:
		return SideTag::kNone;
	}
}

bool InBed(const Bed &bed, int32_t x, int32_t y)
{
	return bed.xmin <= x && x <= bed.xmax && bed.ymin <= y && y <= bed.ymax;
}

/* 가장자리 띠 = 좌우 두 변 + 발치(ymin, 레이더 쪽). 머리맡(ymax)은 띠 없음 — 베개 위 머리 움직임이
 * 사라져도 자리 뜸이 아니다(설계서 §6-2 H1). */
bool InEdge(const Bed &bed, int32_t x, int32_t y)
{
	return InBed(bed, x, y) &&
	       (x < bed.xmin + bed.edgeMm || x > bed.xmax - bed.edgeMm || y < bed.ymin + bed.edgeMm);
}

/* 이 점 말고 같은 칸을 켜 둔 다른 살아 있는 점이 있나 (한 사람이 점 2개로 보일 때, §6-2 H3) */
bool OtherOwner(const State &state, Side s, uint32_t nameIndex, const bool *alive)
{
	for (size_t m = 0; m < state.memoCount; m++) {
		const TrackMemo &other = state.memo[m];
		if (other.nameIndex != nameIndex && other.owner == s && (alive == nullptr || alive[m])) {
			return true;
		}
	}
	return false;
}

Side SideOf(const Bed &bed, int32_t x)
{
	return x < bed.splitX ? Side::kL : Side::kR;
}

int64_t Dist2(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
	int64_t dx = ax - bx;
	int64_t dy = ay - by;
	return dx * dx + dy * dy;
}

void Emit(State &state, EventBuf &out, Kind kind, int64_t startMs, int64_t endMs, uint8_t size, int32_t x,
	  int32_t y, uint8_t count, SideTag side)
{
	if (out.n >= EventBuf::kMax) {
		out.overflow++;
		return;
	}
	out.ev[out.n++] = { state.seq++, kind, startMs, endMs, size, x, y, count, side };
}

/* 칸 표시 켬. 꺼진 뒤 다시 켜지는 거면 away 한 줄. */
void SetSide(State &state, Side s, int64_t nowMs, int32_t x, int32_t y, EventBuf &out)
{
	size_t i = Idx(s);
	if (state.hold[i]) {
		return;
	}
	state.hold[i] = true;
	state.lastSetSide = s;
	state.lastSetX = x;
	state.lastSetY = y;
	if (state.awayPending[i]) {
		state.awayPending[i] = false;
		Emit(state, out, Kind::kAway, state.awayStartMs[i], nowMs, 0, state.awayX[i], state.awayY[i], 1,
		     TagOf(s));
	}
}

/* 그 칸 표시만 끈다(away 없음). 그 칸을 켜 둔 점들의 주인 표시도 지운다. */
void DropSide(State &state, Side s)
{
	state.hold[Idx(s)] = false;
	for (size_t m = 0; m < state.memoCount; m++) {
		if (state.memo[m].owner == s) {
			state.memo[m].owner = Side::kNone;
		}
	}
}

/* 자리 뜸: 칸 표시 끄고, 돌아오면 away 를 낼 수 있게 나간 시각·자리를 기억한다. */
void LeaveSide(State &state, Side s, int64_t atMs, int32_t x, int32_t y)
{
	size_t i = Idx(s);
	if (!state.hold[i]) {
		return;
	}
	DropSide(state, s);
	state.awayPending[i] = true;
	state.awayStartMs[i] = atMs;
	state.awayX[i] = x;
	state.awayY[i] = y;
	state.lastClearTag = TagOf(s);
	state.lastClearX = x;
	state.lastClearY = y;
}

uint8_t SizeOf(int64_t dist2)
{
	if (dist2 < kSize1Dist2) {
		return 1;
	}
	return dist2 < kSize2Dist2 ? 2 : 3;
}

void EmitGroup(State &state, const Config &cfg, EventBuf &out)
{
	Group &g = state.group;
	SideTag tag;
	if (g.sides == (kBitL | kBitR)) {
		tag = SideTag::kLR;
	} else {
		tag = g.sides == kBitL ? SideTag::kL : SideTag::kR;
		/* LR 앞뒤로는 이어짐을 붙이지 않는다. 겹치지 않으므로 간격은 항상 0보다 크다. */
		bool prevSingleOther = state.prevEmitted && state.prevSides != (kBitL | kBitR) &&
				       state.prevSides != g.sides;
		if (prevSingleOther && g.startMs - state.prevEndMs <= cfg.followMs) {
			tag = g.sides == kBitR ? SideTag::kLthenR : SideTag::kRthenL;
		}
	}
	Emit(state, out, Kind::kMove, g.startMs, g.endMs, SizeOf(g.maxDist2), g.x0, g.y0,
	     static_cast<uint8_t>(g.countL + g.countR), tag);
	state.prevEmitted = true;
	state.prevSides = g.sides;
	state.prevEndMs = g.endMs;
	g.pending = false;
}

/* 닫힌 칸 움직임을 묶음에 넣는다. 시간이 겹치면 합치고, 아니면 앞 묶음을 먼저 낸다. */
void AddToGroup(State &state, const Config &cfg, const Episode &ep, size_t i, EventBuf &out)
{
	Group &g = state.group;
	uint8_t bit = i == 0 ? kBitL : kBitR;
	/* 묶음이 반대 칸의 진행 중 움직임을 기다리는 중이면, 그 움직임도 묶음에 들어갈 것이므로 겹침 비교 구간을
	 * 그 움직임의 지금까지 끝으로 넓힌다(긴 L 도중 R 이 두 번 → 한 줄 LR, §6-2 M1). */
	int64_t groupEnd = g.endMs;
	const Episode &other = state.ep[1 - i];
	if (g.pending && other.active && other.startMs <= g.endMs && other.lastMs > groupEnd) {
		groupEnd = other.lastMs;
	}
	if (g.pending && ep.startMs <= groupEnd && g.startMs <= ep.lastMs) {
		if (ep.startMs < g.startMs) {
			g.startMs = ep.startMs;
			g.x0 = ep.x0;
			g.y0 = ep.y0;
		}
		if (ep.lastMs > g.endMs) {
			g.endMs = ep.lastMs;
		}
		g.sides |= bit;
		if (ep.maxDist2 > g.maxDist2) {
			g.maxDist2 = ep.maxDist2;
		}
	} else {
		if (g.pending) {
			EmitGroup(state, cfg, out);
		}
		g = { true, ep.startMs, ep.lastMs, bit, ep.x0, ep.y0, ep.maxDist2, 0, 0 };
	}
	uint8_t &count = i == 0 ? g.countL : g.countR;
	if (ep.maxCount > count) {
		count = ep.maxCount;
	}
}

/* 끊긴 지 gap 넘은 칸 움직임을 닫고, 더 겹칠 게 없는 묶음을 낸다. */
void CloseAndFlush(State &state, const Config &cfg, int64_t nowMs, EventBuf &out)
{
	for (size_t i = 0; i < 2; i++) {
		Episode &ep = state.ep[i];
		if (ep.active && nowMs - ep.lastMs > cfg.moveGapMs) {
			ep.active = false;
			if (ep.lastMs - ep.startMs >= cfg.moveMinMs) {
				AddToGroup(state, cfg, ep, i, out);
			}
		}
	}
	if (!state.group.pending) {
		return;
	}
	for (const Episode &ep : state.ep) {
		/* 진행 중인 움직임이 묶음과 겹치면 끝날 때까지 기다린다(짧아서 버려지면 그때 낸다) */
		if (ep.active && ep.startMs <= state.group.endMs) {
			return;
		}
	}
	EmitGroup(state, cfg, out);
}

void SafetyAndOcc(State &state, const Config &cfg, int64_t nowMs, EventBuf &out)
{
	bool any = state.hold[0] || state.hold[1];
	if (any && nowMs - state.lastInBedMs >= cfg.holdMaxMs) {
		/* 안전장치: 나가는 걸 놓쳤다고 본다. away 는 내지 않는다. off 줄은 켜져 있던 칸(둘 다면 LR)과
		 * 침대 안 마지막 자리. */
		state.lastClearTag = state.hold[0] && state.hold[1] ? SideTag::kLR
				     : state.hold[0]		    ? SideTag::kL
								    : SideTag::kR;
		state.lastClearX = state.lastInBedX;
		state.lastClearY = state.lastInBedY;
		DropSide(state, Side::kL);
		DropSide(state, Side::kR);
		any = false;
	}

	if (any) {
		state.allClearSinceMs = -1;
		if (!state.occupied) {
			state.occupied = true;
			Emit(state, out, Kind::kOn, nowMs, nowMs, 0, state.lastSetX, state.lastSetY, 1,
			     TagOf(state.lastSetSide));
		}
		return;
	}
	if (state.allClearSinceMs < 0) {
		state.allClearSinceMs = nowMs;
	}
	if (state.occupied && nowMs - state.allClearSinceMs >= cfg.leaveDelayMs) {
		state.occupied = false;
		Emit(state, out, Kind::kOff, nowMs, nowMs, 0, state.lastClearX, state.lastClearY, 0,
		     state.lastClearTag);
	}
}

/* 침대 좌표 미설정: 예전 규칙. 트랙이 있으면 시계를 다시 재고, 마지막으로 있던 때부터 leaveDelayMs 지나면 0 */
void LegacyOcc(State &state, const Config &cfg, size_t trackCount, int32_t x, int32_t y, int64_t nowMs,
	       EventBuf &out)
{
	if (trackCount > 0) {
		state.lastLiveMs = nowMs;
		state.lastClearX = x;
		state.lastClearY = y;
		if (!state.occupied) {
			state.occupied = true;
			Emit(state, out, Kind::kOn, nowMs, nowMs, 0, x, y, static_cast<uint8_t>(trackCount),
			     SideTag::kNone);
		}
		return;
	}
	if (state.occupied && nowMs - state.lastLiveMs >= cfg.leaveDelayMs) {
		state.occupied = false;
		Emit(state, out, Kind::kOff, nowMs, nowMs, 0, state.lastClearX, state.lastClearY, 0, SideTag::kNone);
	}
}

} /* namespace */

bool BedEnabled(const Bed &bed)
{
	return bed.xmin != 0 || bed.xmax != 0 || bed.ymin != 0 || bed.ymax != 0;
}

bool Update(State &state, const Config &cfg, const Tracker::State &tracker, int64_t nowMs, EventBuf &out)
{
	const Bed &bed = cfg.bed;
	if (!BedEnabled(bed)) {
		int32_t x = tracker.count ? tracker.tracks[0].x : 0;
		int32_t y = tracker.count ? tracker.tracks[0].y : 0;
		LegacyOcc(state, cfg, tracker.count, x, y, nowMs, out);
		return state.occupied;
	}

	/* 1. 시간으로 닫히는 움직임 먼저 (새 점이 gap 뒤에 오면 다른 건) */
	CloseAndFlush(state, cfg, nowMs, out);

	/* 2. 트래커에서 지워진 점: 가장자리 띠에서 사라졌으면 자리 뜸
	 *    (단 같은 칸을 켜 둔 다른 살아 있는 점이 있으면 안 끔) */
	bool alive[Tracker::kMaxTracks] = {};
	for (size_t m = 0; m < state.memoCount; m++) {
		for (size_t t = 0; t < tracker.count; t++) {
			if (tracker.tracks[t].nameIndex == state.memo[m].nameIndex) {
				alive[m] = true;
				break;
			}
		}
	}
	for (size_t m = 0; m < state.memoCount; m++) {
		const TrackMemo &memo = state.memo[m];
		if (!alive[m] && memo.owner != Side::kNone && InEdge(bed, memo.x, memo.y) &&
		    !OtherOwner(state, memo.owner, memo.nameIndex, alive)) {
			LeaveSide(state, memo.owner, memo.lastSeenMs, memo.x, memo.y);
		}
	}
	size_t kept = 0;
	for (size_t m = 0; m < state.memoCount; m++) {
		if (alive[m]) {
			state.memo[kept++] = state.memo[m];
		}
	}
	state.memoCount = kept;

	/* 3. 이번 프레임에 잡힌 점 */
	uint8_t sideCount[2] = {};
	int32_t sideX[2][Tracker::kMaxTracks];
	int32_t sideY[2][Tracker::kMaxTracks];
	for (size_t t = 0; t < tracker.count; t++) {
		const Tracker::Track &tr = tracker.tracks[t];
		TrackMemo *memo = nullptr;
		for (size_t m = 0; m < state.memoCount; m++) {
			if (state.memo[m].nameIndex == tr.nameIndex) {
				memo = &state.memo[m];
				break;
			}
		}
		if (memo == nullptr) {
			if (state.memoCount >= Tracker::kMaxTracks) {
				continue;
			}
			memo = &state.memo[state.memoCount++];
			*memo = { tr.nameIndex, Side::kNone, tr.x, tr.y, nowMs };
		}
		if (!tr.seen) {
			continue;
		}
		memo->x = tr.x;
		memo->y = tr.y;
		memo->lastSeenMs = nowMs;

		if (InBed(bed, tr.x, tr.y)) {
			state.lastInBedMs = nowMs;
			state.lastInBedX = tr.x;
			state.lastInBedY = tr.y;
			Side s = SideOf(bed, tr.x);
			if (memo->owner == Side::kNone) {
				memo->owner = s;
			} else if (memo->owner != s && !state.hold[Idx(s)]) {
				/* 같은 점이 빈 반대 칸으로 넘어감 → 표시를 넘긴다. 넘어간 칸이 이미 켜져 있으면(옆사람)
				 * 넘기지 않고 이 점을 원래 칸 사람으로 계속 본다(§6-2 H2). */
				DropSide(state, memo->owner);
				memo->owner = s;
			}
			SetSide(state, s, nowMs, tr.x, tr.y, out);
			size_t i = Idx(s);
			sideX[i][sideCount[i]] = tr.x;
			sideY[i][sideCount[i]] = tr.y;
			sideCount[i]++;
		} else if (memo->owner != Side::kNone) {
			/* 그 칸에서 온 점이 침대 밖에서 보임 → 자리 뜸 (같은 칸에 다른 점이 남아 있으면 이 점만 놓음) */
			if (OtherOwner(state, memo->owner, memo->nameIndex, nullptr)) {
				memo->owner = Side::kNone;
			} else {
				LeaveSide(state, memo->owner, nowMs, tr.x, tr.y);
			}
		}
	}

	/* 4. 칸별 움직임 */
	for (size_t i = 0; i < 2; i++) {
		if (sideCount[i] == 0) {
			continue;
		}
		Episode &ep = state.ep[i];
		if (!ep.active) {
			ep = { true, nowMs, nowMs, sideX[i][0], sideY[i][0], 0, 0 };
		}
		ep.lastMs = nowMs;
		if (sideCount[i] > ep.maxCount) {
			ep.maxCount = sideCount[i];
		}
		for (size_t k = 0; k < sideCount[i]; k++) {
			int64_t d2 = Dist2(sideX[i][k], sideY[i][k], ep.x0, ep.y0);
			if (d2 > ep.maxDist2) {
				ep.maxDist2 = d2;
			}
		}
	}

	/* 5. 안전장치·occ (묶음 내보내기는 다음 프레임·틱의 1단계에서) */
	SafetyAndOcc(state, cfg, nowMs, out);
	return state.occupied;
}

bool Tick(State &state, const Config &cfg, int64_t nowMs, EventBuf &out)
{
	if (!BedEnabled(cfg.bed)) {
		LegacyOcc(state, cfg, 0, 0, 0, nowMs, out);
		return state.occupied;
	}
	CloseAndFlush(state, cfg, nowMs, out);
	SafetyAndOcc(state, cfg, nowMs, out);
	return state.occupied;
}

int FormatEvent(const Event &ev, char *buf, size_t len)
{
	static const char *const kKinds[] = { "on", "off", "move", "away", "seat", "spot" };
	static const char *const kSides[] = { "", "L", "R", "LR", "L>R", "R>L" };
	return snprintf(buf, len, "JJ1,EV,%lu,%s,%lld,%lld,%u,%ld,%ld,%u,,%s", static_cast<unsigned long>(ev.seq),
			kKinds[static_cast<size_t>(ev.kind)], static_cast<long long>(ev.startMs),
			static_cast<long long>(ev.endMs), static_cast<unsigned>(ev.size), static_cast<long>(ev.x),
			static_cast<long>(ev.y), static_cast<unsigned>(ev.count),
			kSides[static_cast<size_t>(ev.side)]);
}

} /* namespace BedRules */
