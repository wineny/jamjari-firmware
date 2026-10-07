/*
 * 보드 안 추정(fusion) — 레이더 + 60GHz 좌·우 + 자리 지도 → 칸 상태 + 반려동물 머문 자리 (10/5 N).
 * 호스트(clang++)와 타깃 양쪽에서 컴파일되는 순수 C++ (Zephyr 의존 없음, 동적 할당 없음). 설계: 설계-보드추정.md
 *
 * 맥 기준 구현을 그대로 옮겼다. 같은 입력이면 출력이 같아야 한다(test/run_fusion.sh 가 대조).
 *   - 칸 상태: f-60ghz dummy/자연/합치기.py (Merger) — 이름표 따라가기, 단서 A·B, 반려동물로 추정, 모름.
 *   - 점 이름: 이름붙이기-시험.py (멈춘 자리 기억) — 머문 자리 계산에만 쓴다.
 *   - 머문 자리: 화면데이터.py 초당 점(한 초 끝쪽 4개) + web/scripts/push-pet-spots.ts stays() — 10분 이상·최대 3개.
 * 시각은 장면 시각(초, double). 파이썬 float 계산과 같은 결과가 나오게 double 로 둔다.
 *
 * 부르는 순서(파이썬 run() 과 같다): 장면 초 s 의 60GHz 두 줄이 오면 Judge(s) → 그 뒤 t ≥ s 인 레이더 프레임들 Radar(t).
 * 끝나면 End() → 머문 자리 확정.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Fusion {

/* 칸 상태 (합치기.py 의 네 상태. 화면 글자는 「모름」 대신 「구분 안 됨」) */
enum class State : uint8_t { kEmpty = 0, kHuman = 1, kPet = 2, kUnknown = 3 };
/* 단서 비트: A = 레이더(고양이 이름표가 칸에 들어옴), B = 60GHz(숨 패턴이 혼자일 때와 다름) */
constexpr uint8_t kClueA = 1;
constexpr uint8_t kClueB = 2;

/* 자리 지도 region (합치기.py region()) */
enum class Region : uint8_t { kNone, kSeatL, kSeatR, kPetHead, kPetFoot, kBed, kOut };

struct Box {
	int32_t x0, x1, y0, y1; /* 경계 포함 */
};

/* 자리지도.json 과 같은 값 (mm). 지금은 고정값, 나중에 웹에서 받기 */
struct Map {
	Box bed;
	Box seat[2]; /* [0] L, [1] R */
	Box petHead; /* 머리맡 */
	Box petFoot; /* 발치 */
};
const Map &DefaultMap();

/* 60GHz 한 칸 한 줄. breath/heart 가 없으면(null) -1 */
struct G60 {
	uint8_t inBed;
	int16_t breath;
	int16_t heart;
	uint8_t move;
};

struct Point {
	int32_t x, y;
};

/* 1초 판단 결과 */
struct SideOut {
	State state;
	uint8_t clues;
};
struct SecondOut {
	int32_t sec;
	SideOut side[2];
	Region catAt; /* 고양이가 마지막으로 있던 곳(kNone = 안 보임) */
	bool light; /* 발밑 조명 */
};

/* 반려동물이 한 자리에 10분 이상 머문 곳 */
struct Spot {
	int32_t from; /* 장면 초 */
	int32_t to;
	int32_t x, y;
	int32_t still; /* 실제로 멈춰 있던 초 합 */
};

constexpr size_t kMaxTracks = 16;
constexpr size_t kBWin = 20;
constexpr size_t kMaxGroups = 32;
constexpr size_t kMaxSpots = 3;

/* ── 합치기.py Merger ── */
struct MTrack {
	int32_t x, y;
	double seen;
	int8_t tag; /* -1 = 아직 침대에 안 닿음, 0 고양이, 1 사람, 2 ? */
	Region region;
};

struct Merger {
	MTrack tracks[kMaxTracks];
	size_t nTracks;
	Region catAt;
	bool person[2];
	double emptySince[2];
	bool crossed[2];
	G60 hist[2][kBWin];
	size_t histN[2];
	double lastMove[2];
	double onset[2];
	bool b[2];
	int32_t clean[2];
	bool light;
	uint32_t overflow;
};

/* ── 이름붙이기-시험.py ── */
struct NTrack {
	int32_t x, y;
	double seen;
	int8_t name; /* -1 아직 없음(침대 밖에서 오는 중), 0 남편, 1 누리, 2 로나, 3 ? */
};

struct Namer {
	NTrack active[kMaxTracks];
	size_t nActive;
	bool hasStop[3];
	int32_t stopX[3], stopY[3];
	uint32_t overflow;
};

/* ── 머문 자리 (push-pet-spots.ts stays) ── */
struct Group {
	int32_t from, to, x, y, still;
};

struct Stays {
	/* 지금 초에 들어온 점(이름 포함) 중 끝쪽 4개 — 화면데이터.py 의 radar[s][-4:] */
	int32_t curSec;
	int32_t ringX[4], ringY[4];
	int8_t ringName[4];
	size_t ringN; /* 지금까지 넣은 수(4 넘으면 앞에서 밀림) */
	bool hasLast;
	int32_t lastT, lastX, lastY;
	Group groups[kMaxGroups];
	size_t nGroups;
	uint32_t overflow;
};

struct Engine {
	Merger m;
	Namer n;
	Stays s;
	int32_t judged; /* Judge 한 초 수 (= 다음 Judge 할 초) */
};

void Init(Engine &e);

/* 장면 초 sec 의 60GHz 좌·우 → 1초 판단. 반환값 = 그 초 결과 */
SecondOut Judge(Engine &e, int32_t sec, const G60 &left, const G60 &right);

/* 레이더 프레임 하나. pts = 이번 프레임에 잡힌 점(슬롯 순서, 빈 슬롯 제외) */
void Radar(Engine &e, double t, const Point *pts, size_t n);

/* 장면 끝 → 머문 자리 확정. out 에 오래 머문 순 최대 3개, 반환값 = 개수 */
size_t End(Engine &e, Spot (&out)[kMaxSpots]);

const char *RegionName(Region r);

} /* namespace Fusion */
