/*
 * 보드 안 추정(fusion). 규칙 설명은 fusion.h 와 f-60ghz dummy/자연/합치기-규칙.md.
 * 파이썬 줄과 짝을 맞추려고 함수 이름·순서를 합치기.py · 이름붙이기-시험.py · push-pet-spots.ts 와 같게 뒀다.
 */

#include "fusion.h"

#include <cmath>
#include <cstring>

namespace Fusion {
namespace {

constexpr int64_t kGateD2 = 700LL * 700; /* GATE_MM 700 (거리 비교는 제곱 정수로) */
constexpr double kLostS = 1.5;
constexpr double kEmptyS = 3;
constexpr double kBAfterMove = 10;
constexpr double kBAfterOnset = 35;
constexpr int32_t kBClear = 20;

constexpr int8_t kTagNone = -1, kTagCat = 0, kTagHuman = 1, kTagUnknown = 2;

/* 이름붙이기-시험.py */
constexpr double kNearMm = 350;
constexpr double kMarginMm = 120;
constexpr int8_t kNameNone = -1, kHusband = 0, kNuri = 1, kPet = 2, kQ = 3;

/* push-pet-spots.ts */
constexpr int32_t kMinStayS = 600;
constexpr int32_t kBriefS = 60;
constexpr int64_t kSameSpotD2 = 300LL * 300;

const Map kMap = {
	{ -800, 800, 300, 2300 }, /* bed */
	{ { -800, 0, 600, 2000 }, { 0, 800, 600, 2000 } }, /* seat L 남편 · R 누리 */
	{ 0, 800, 2000, 2300 }, /* 머리맡 */
	{ -800, 800, 300, 600 }, /* 발치 */
};

bool Inside(const Box &b, int32_t x, int32_t y)
{
	return b.x0 <= x && x <= b.x1 && b.y0 <= y && y <= b.y1;
}

/* 합치기.py region(): 칸 L·R → 머리맡·발치 → 침대 → 밖 (dict 순서) */
Region RegionOf(int32_t x, int32_t y)
{
	if (Inside(kMap.seat[0], x, y)) {
		return Region::kSeatL;
	}
	if (Inside(kMap.seat[1], x, y)) {
		return Region::kSeatR;
	}
	if (Inside(kMap.petHead, x, y)) {
		return Region::kPetHead;
	}
	if (Inside(kMap.petFoot, x, y)) {
		return Region::kPetFoot;
	}
	return Inside(kMap.bed, x, y) ? Region::kBed : Region::kOut;
}

bool IsSeat(Region r)
{
	return r == Region::kSeatL || r == Region::kSeatR;
}
bool IsPet(Region r)
{
	return r == Region::kPetHead || r == Region::kPetFoot;
}
int SideOf(Region r)
{
	return r == Region::kSeatL ? 0 : 1;
}

int64_t D2(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
	int64_t dx = ax - bx, dy = ay - by;
	return dx * dx + dy * dy;
}

/* ───────── 합치기.py Merger ───────── */

void ResetB(Merger &m, int side)
{
	m.histN[side] = 0;
	m.b[side] = false;
	m.clean[side] = 0;
}

void Arrive(Merger &m, MTrack &tr, Region r, Region prev)
{
	if (tr.tag == kTagCat || (tr.tag == kTagUnknown && IsPet(r))) {
		m.catAt = r != Region::kOut ? r : Region::kNone;
	} else if (tr.tag == kTagHuman && IsSeat(r)) {
		int side = SideOf(r);
		m.light = false;
		if (prev != Region::kNone && IsSeat(prev) && prev != r && m.person[side]) {
			m.crossed[side] = true; /* 옆 칸 사람이 넘어옴 → 이 칸 흐트러짐은 사람 둘 */
			ResetB(m, side);
		}
		m.person[side] = true;
	}
}

void Leave(Merger &m, MTrack &tr, Region r)
{
	if (tr.tag == kTagHuman && IsSeat(r)) {
		int side = SideOf(r);
		if (RegionOf(tr.x, tr.y) == Region::kOut) {
			m.light = true;
		}
		if (m.crossed[side]) {
			m.crossed[side] = false;
		} else {
			m.person[side] = false;
		}
		ResetB(m, side);
	}
}

void Born(Merger &m, int32_t x, int32_t y, double t)
{
	Region r = RegionOf(x, y);
	int8_t tag;
	if (r == Region::kOut) {
		tag = kTagNone;
	} else if (m.catAt == r && IsSeat(r)) {
		tag = kTagUnknown;
	} else if (m.catAt == r || IsPet(r)) {
		tag = kTagCat;
	} else {
		tag = kTagHuman;
	}
	MTrack tmp = { x, y, t, tag, r };
	MTrack *tr = &tmp;
	if (m.nTracks < kMaxTracks) {
		m.tracks[m.nTracks] = tmp;
		tr = &m.tracks[m.nTracks++];
	} else {
		m.overflow++; /* 칸이 꽉 참: 이 점은 따라가지 못하지만 도착 판단은 한다 */
	}
	Arrive(m, *tr, r, Region::kNone);
}

void Move(Merger &m, MTrack &tr, int32_t x, int32_t y, double t)
{
	Region r = RegionOf(x, y);
	tr.x = x;
	tr.y = y;
	tr.seen = t;
	if (tr.tag == kTagNone && r != Region::kOut) {
		tr.tag = IsPet(r) ? kTagCat : kTagHuman;
	}
	if (r != tr.region) {
		Region prev = tr.region;
		if (tr.tag == kTagUnknown && IsSeat(prev) && r == Region::kOut) {
			tr.tag = kTagHuman; /* 고양이 있는 칸에서 침대 밖으로 나감 → 사람 (ny 10/5) */
		}
		Leave(m, tr, prev);
		tr.region = r;
		Arrive(m, tr, r, prev);
	}
}

void MergerRadar(Merger &m, double t, const Point *pts, size_t n)
{
	size_t k = 0;
	for (size_t i = 0; i < m.nTracks; i++) {
		if (t - m.tracks[i].seen <= kLostS) {
			m.tracks[k++] = m.tracks[i];
		}
	}
	m.nTracks = k;
	bool used[kMaxTracks] = {};
	size_t freeCount = m.nTracks; /* free = 이 프레임 시작 때의 트랙들(새로 생긴 건 안 들어감) */
	for (size_t p = 0; p < n; p++) {
		int best = -1;
		int64_t bestD2 = 0;
		for (size_t i = 0; i < freeCount; i++) {
			if (used[i]) {
				continue;
			}
			int64_t d2 = D2(m.tracks[i].x, m.tracks[i].y, pts[p].x, pts[p].y);
			if (best < 0 || d2 < bestD2) {
				best = static_cast<int>(i);
				bestD2 = d2;
			}
		}
		if (best >= 0 && bestD2 <= kGateD2) {
			used[best] = true;
			Move(m, m.tracks[best], pts[p].x, pts[p].y, t);
		} else {
			Born(m, pts[p].x, pts[p].y, t);
		}
	}
}

bool ClueB(Merger &m, double t, int side, const G60 &row)
{
	if (row.inBed != 1) {
		m.onset[side] = t + 1;
		ResetB(m, side);
		return false;
	}
	if (row.move) {
		m.lastMove[side] = t;
	}
	G60 *h = m.hist[side];
	if (m.histN[side] == kBWin) {
		memmove(h, h + 1, sizeof(G60) * (kBWin - 1));
		h[kBWin - 1] = row;
	} else {
		h[m.histN[side]++] = row;
	}
	bool quiet = m.histN[side] == kBWin;
	for (size_t i = 0; quiet && i < kBWin; i++) {
		quiet = h[i].move == 0 && h[i].inBed == 1;
	}
	quiet = quiet && t - m.lastMove[side] >= kBWin + kBAfterMove && t - m.onset[side] >= kBWin + kBAfterOnset;
	if (!quiet || m.crossed[side]) {
		return m.b[side] && !m.crossed[side]; /* 움직이는 동안은 직전 판단 유지 */
	}
	int gaps = 0, nbr = 0, bmin = 0, bmax = 0;
	for (size_t i = 0; i < kBWin; i++) {
		if (h[i].breath < 0 || h[i].heart < 0) {
			gaps++;
		}
		if (h[i].breath >= 0) {
			bmin = nbr ? (h[i].breath < bmin ? h[i].breath : bmin) : h[i].breath;
			bmax = nbr ? (h[i].breath > bmax ? h[i].breath : bmax) : h[i].breath;
			nbr++;
		}
	}
	bool noisy = gaps >= 4 || (nbr >= 2 && bmax - bmin >= 6);
	if (noisy) {
		m.b[side] = true;
		m.clean[side] = 0;
	} else if (++m.clean[side] >= kBClear) {
		m.b[side] = false;
	}
	return m.b[side];
}

/* ───────── 이름붙이기-시험.py ───────── */

int8_t FirstName(Region r)
{
	if (r == Region::kSeatL) {
		return kHusband;
	}
	if (r == Region::kSeatR) {
		return kNuri;
	}
	return IsPet(r) ? kPet : kNameNone;
}

void Retire(Namer &n, const NTrack &tr)
{
	if (tr.name >= 0 && tr.name < 3) {
		if (RegionOf(tr.x, tr.y) == Region::kOut) {
			n.hasStop[tr.name] = false;
		} else {
			n.hasStop[tr.name] = true;
			n.stopX[tr.name] = tr.x;
			n.stopY[tr.name] = tr.y;
		}
	}
}

/* 점마다 그때 이름(-1 은 「?」로 친다)을 names 에 돌려준다 */
void NamerRadar(Namer &n, double t, const Point *pts, size_t np, int8_t *names)
{
	size_t k = 0;
	for (size_t i = 0; i < n.nActive; i++) {
		if (t - n.active[i].seen > kLostS) {
			Retire(n, n.active[i]);
		} else {
			n.active[k++] = n.active[i];
		}
	}
	n.nActive = k;
	bool used[kMaxTracks] = {};
	size_t freeCount = n.nActive;
	for (size_t p = 0; p < np; p++) {
		int32_t x = pts[p].x, y = pts[p].y;
		int best = -1;
		int64_t bestD2 = 0;
		for (size_t i = 0; i < freeCount; i++) {
			if (used[i]) {
				continue;
			}
			int64_t d2 = D2(n.active[i].x, n.active[i].y, x, y);
			if (best < 0 || d2 < bestD2) {
				best = static_cast<int>(i);
				bestD2 = d2;
			}
		}
		if (best >= 0 && bestD2 <= kGateD2) {
			used[best] = true;
			NTrack &tr = n.active[best];
			tr.x = x;
			tr.y = y;
			tr.seen = t;
			Region r = RegionOf(x, y);
			if (tr.name == kNameNone && r != Region::kOut) {
				int8_t f = FirstName(r);
				tr.name = f >= 0 ? f : kQ;
			}
			names[p] = tr.name;
			continue;
		}
		Region r = RegionOf(x, y);
		int8_t name;
		if (r == Region::kOut) {
			name = kNameNone;
		} else {
			bool busy[3] = {};
			for (size_t i = 0; i < n.nActive; i++) {
				if (n.active[i].name >= 0 && n.active[i].name < 3) {
					busy[n.active[i].name] = true;
				}
			}
			/* 후보 (거리, 이름) 정렬 — 이름 순서 남편 < 누리 < 로나 (한글 글자 순서와 같다) */
			double cd[3];
			int8_t cn[3];
			int nc = 0;
			for (int8_t who = 0; who < 3; who++) {
				if (!n.hasStop[who] || busy[who]) {
					continue;
				}
				double d = std::sqrt(static_cast<double>(D2(n.stopX[who], n.stopY[who], x, y)));
				int j = nc++;
				while (j > 0 && (cd[j - 1] > d || (cd[j - 1] == d && cn[j - 1] > who))) {
					cd[j] = cd[j - 1];
					cn[j] = cn[j - 1];
					j--;
				}
				cd[j] = d;
				cn[j] = who;
			}
			if (nc == 0 || cd[0] > kNearMm) {
				name = kQ;
			} else if (nc > 1 && cd[1] - cd[0] < kMarginMm) {
				name = kQ;
			} else {
				name = cn[0];
			}
		}
		NTrack tr = { x, y, t, name };
		if (n.nActive < kMaxTracks) {
			n.active[n.nActive++] = tr;
		} else {
			n.overflow++;
		}
		names[p] = name;
	}
}

/* ───────── 머문 자리 (화면데이터.py 초당 점 + push-pet-spots.ts stays) ───────── */

void AddStop(Stays &s, int32_t at, int32_t x, int32_t y, int32_t until)
{
	if (until - at < kBriefS) {
		return;
	}
	for (size_t i = 0; i < s.nGroups; i++) {
		Group &g = s.groups[i];
		if (D2(x, y, g.x, g.y) < kSameSpotD2 && at - g.to <= kMinStayS) {
			g.to = until;
			g.still += until - at;
			return;
		}
	}
	/* 칸 비우기: 더는 합쳐질 수 없고(이제 오는 멈춤은 at 이 더 크다) 10분도 못 채운 묶음은 결과에 안 나온다 → 지운다.
	 * 순서는 그대로 둔다(같은 머문 시간이면 먼저 생긴 것 먼저). 결과는 파이썬과 같다 — 8시간 밤에도 32칸이 안 차게. */
	size_t k = 0;
	for (size_t i = 0; i < s.nGroups; i++) {
		const Group &g = s.groups[i];
		if (!(at - g.to > kMinStayS && g.still < kMinStayS)) {
			s.groups[k++] = g;
		}
	}
	s.nGroups = k;
	if (s.nGroups < kMaxGroups) {
		s.groups[s.nGroups++] = { at, until, x, y, until - at };
	} else {
		s.overflow++;
	}
}

/* 한 초가 끝났다: 그 초의 끝쪽 4개 점 중 첫 반려동물 점 → stays() 한 걸음 */
void CloseSecond(Stays &s, int32_t judged)
{
	if (s.curSec < 0 || s.curSec >= judged || s.ringN == 0) {
		return; /* 화면데이터는 판단한 초(n)까지만 쓴다 */
	}
	size_t cnt = s.ringN < 4 ? s.ringN : 4;
	size_t start = s.ringN < 4 ? 0 : s.ringN % 4; /* 가장 오래된 것부터 */
	for (size_t i = 0; i < cnt; i++) {
		size_t j = (start + i) % 4;
		if (s.ringName[j] != kPet) {
			continue;
		}
		int32_t t = s.curSec;
		if (s.hasLast && t - s.lastT > 1) {
			AddStop(s, s.lastT, s.lastX, s.lastY, t);
		}
		s.hasLast = true;
		s.lastT = t;
		s.lastX = s.ringX[j];
		s.lastY = s.ringY[j];
		return;
	}
}

void StaysRadar(Stays &s, int32_t judged, double t, const Point *pts, size_t n, const int8_t *names)
{
	int32_t sec = static_cast<int32_t>(std::floor(t));
	if (sec != s.curSec) {
		CloseSecond(s, judged);
		s.curSec = sec;
		s.ringN = 0;
	}
	for (size_t i = 0; i < n; i++) {
		size_t j = s.ringN % 4;
		s.ringX[j] = pts[i].x;
		s.ringY[j] = pts[i].y;
		s.ringName[j] = names[i];
		s.ringN++;
	}
}

} /* namespace */

const Map &DefaultMap()
{
	return kMap;
}

void Init(Engine &e)
{
	memset(&e, 0, sizeof(e));
	e.m.catAt = Region::kNone;
	for (int side = 0; side < 2; side++) {
		e.m.lastMove[side] = -999;
		e.m.onset[side] = -999;
	}
	e.s.curSec = -1;
}

SecondOut Judge(Engine &e, int32_t sec, const G60 &left, const G60 &right)
{
	Merger &m = e.m;
	double t = sec;
	const G60 *g[2] = { &left, &right };
	SecondOut out = {};
	out.sec = sec;
	for (int side = 0; side < 2; side++) {
		bool here = g[side]->inBed == 1;
		bool catHere = m.catAt == (side == 0 ? Region::kSeatL : Region::kSeatR);
		if (here) {
			m.emptySince[side] = t;
			if (!catHere) {
				m.person[side] = true;
			}
		} else if (t - m.emptySince[side] >= kEmptyS && !catHere) {
			m.person[side] = false;
		}
		bool b = ClueB(m, t, side, *g[side]);
		uint8_t clues = (catHere ? kClueA : 0) | (b && m.person[side] ? kClueB : 0);
		SideOut &o = out.side[side];
		if (m.person[side] && clues) {
			o = { State::kUnknown, clues };
		} else if (catHere) {
			o = { State::kPet, kClueA };
		} else if (m.person[side]) {
			o = { State::kHuman, 0 };
		} else {
			o = { State::kEmpty, 0 };
		}
	}
	out.catAt = m.catAt;
	out.light = m.light;
	e.judged = sec + 1;
	return out;
}

void Radar(Engine &e, double t, const Point *pts, size_t n)
{
	int8_t names[8];
	if (n > 8) {
		n = 8;
	}
	NamerRadar(e.n, t, pts, n, names);
	StaysRadar(e.s, e.judged, t, pts, n, names);
	MergerRadar(e.m, t, pts, n);
}

size_t End(Engine &e, Spot (&out)[kMaxSpots])
{
	Stays &s = e.s;
	CloseSecond(s, e.judged);
	s.curSec = -1;
	s.ringN = 0;
	if (s.hasLast) {
		AddStop(s, s.lastT, s.lastX, s.lastY, e.judged - 1);
		s.hasLast = false;
	}
	/* still 이 큰 순, 같으면 먼저 생긴 것 먼저(안정 정렬) → 최대 3개 */
	size_t n = 0;
	for (size_t i = 0; i < s.nGroups; i++) {
		const Group &g = s.groups[i];
		if (g.still < kMinStayS) {
			continue;
		}
		Spot sp = { g.from, g.to, g.x, g.y, g.still };
		size_t j = n < kMaxSpots ? n++ : kMaxSpots;
		while (j > 0 && out[j - 1].still < sp.still) {
			if (j < kMaxSpots) {
				out[j] = out[j - 1];
			}
			j--;
		}
		if (j < kMaxSpots) {
			out[j] = sp;
		}
	}
	return n;
}

const char *RegionName(Region r)
{
	switch (r) {
	case Region::kSeatL:
		return "L";
	case Region::kSeatR:
		return "R";
	case Region::kPetHead:
		return "head";
	case Region::kPetFoot:
		return "foot";
	case Region::kBed:
		return "bed";
	default:
		return "-";
	}
}

} /* namespace Fusion */
