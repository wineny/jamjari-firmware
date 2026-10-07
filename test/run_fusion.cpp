/*
 * 보드 추정 호스트 시험 (10/5 N). 보드재생.py --g60 --dump 로 만든 바이트 파일을
 * 보드와 같은 길(ReplayFrames 동기화 → Ld2450::ParseFrame → Fusion)로 돌려 test/fusion_ref.py 와 같은 형식으로 찍는다.
 *
 *   ./run_fusion <바이트 파일>
 *   ./run_fusion --types <바이트 파일>   # 동기화 시험: 완성된 프레임 종류(16진)만
 */

#include <cstdio>
#include <string>

#include "fusion.h"
#include "ld2450_parser.h"
#include "replay_frames.h"

namespace {

const char kState[] = { 'E', 'H', 'P', 'U' };

const char *Clues(uint8_t c)
{
	static const char *k[] = { "-", "A", "B", "AB" };
	return k[c & 3];
}

Fusion::G60 Side(const uint8_t *p)
{
	return { p[0], static_cast<int16_t>(p[1] == 0xFF ? -1 : p[1]), static_cast<int16_t>(p[2] == 0xFF ? -1 : p[2]), p[3] };
}

/* --types: 동기화 시험용 — 완성된 프레임 종류만 찍는다 */
void OnType(const uint8_t *fr, void *)
{
	printf("%02x\n", fr[2]);
}

void OnFrame(const uint8_t *fr, void *ctx)
{
	Fusion::Engine &e = *static_cast<Fusion::Engine *>(ctx);
	switch (static_cast<ReplayFrames::Type>(fr[2])) {
	case ReplayFrames::Type::kG60: {
		auto o = Fusion::Judge(e, static_cast<int32_t>(ReplayFrames::U32(fr + 4)), Side(fr + 8), Side(fr + 12));
		printf("S %d %c %s %c %s %s %d\n", o.sec, kState[static_cast<int>(o.side[0].state)], Clues(o.side[0].clues),
		       kState[static_cast<int>(o.side[1].state)], Clues(o.side[1].clues), Fusion::RegionName(o.catAt),
		       o.light ? 1 : 0);
		break;
	}
	case ReplayFrames::Type::kRadar: {
		Ld2450::Target targets[Ld2450::kTargetSlots];
		Ld2450::ParseFrame(fr + 4, targets); /* 타깃이 8바이트째부터라 4칸 당겨 넘긴다 */
		Fusion::Point pts[Ld2450::kTargetSlots];
		size_t n = 0;
		for (auto &t : targets) {
			if (t.present) {
				pts[n++] = { t.x, t.y };
			}
		}
		Fusion::Radar(e, ReplayFrames::U32(fr + 4) / 1000.0, pts, n);
		break;
	}
	case ReplayFrames::Type::kEnd: {
		Fusion::Spot spots[Fusion::kMaxSpots];
		size_t n = Fusion::End(e, spots);
		for (size_t i = 0; i < n; i++) {
			printf("P %zu %d %d %d %d %d\n", i + 1, spots[i].from, spots[i].to, spots[i].x, spots[i].y, spots[i].still);
		}
		break;
	}
	case ReplayFrames::Type::kStart:
		Fusion::Init(e);
		break;
	default:
		break; /* 03 옛 레이더 프레임은 보드 추정에 안 쓴다 */
	}
}

} /* namespace */

int main(int argc, char **argv)
{
	bool types = argc == 3 && std::string(argv[1]) == "--types";
	if (argc != 2 && !types) {
		fprintf(stderr, "사용법: %s [--types] <바이트 파일>\n", argv[0]);
		return 2;
	}
	const char *path = argv[argc - 1];
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}
	static Fusion::Engine e;
	Fusion::Init(e);
	ReplayFrames::SyncState sync;
	int c;
	while ((c = fgetc(f)) != EOF) {
		ReplayFrames::Feed(sync, static_cast<uint8_t>(c), types ? OnType : OnFrame, &e);
	}
	fclose(f);
	fprintf(stderr, "dropped %u · 트랙 넘침 merger %u namer %u · 자리 넘침 %u\n", sync.dropped, e.m.overflow, e.n.overflow,
		e.s.overflow);
	return 0;
}
