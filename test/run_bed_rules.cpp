// 침대 칸 규칙(bed_rules.h)을 프레임 목록에 대고 돌려 occ 변화와 사건 줄을 뽑는다.
// 펌웨어 app_task.cpp 와 같은 순서: 프레임마다 트래커 Update → BedRules::Update, 1초마다 BedRules::Tick.
// 보드에선 1초 틱과 프레임이 같은 스레드에서 번갈아 불린다 → 프레임 ms 이하의 1000 배수 틱을 그 프레임 앞에 돌린다.
//
// 입력(표준입력, 공백 구분 — JSON 변환은 replay_scenes.py 가 한다):
//   Z <xmin> <xmax> <ymin> <ymax>                                   구역
//   C <xmin> <xmax> <ymin> <ymax> <split> <edge> <leave_ms> <hold_max_ms> <gap_ms> <min_ms> <follow_ms>
//   F <ms> <n> [<x> <y>] × n                                          프레임 (n ≤ 3)
// 출력(표준출력):
//   OCC <ms> <0|1>          occ 가 바뀐 순간
//   EV <JJ1,EV,... 한 줄>   보드가 낼 사건 줄 그대로

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "../src/bed_rules.h"
#include "../src/tracker.h"

namespace {

void Print(const BedRules::EventBuf &out)
{
	for (size_t i = 0; i < out.n; i++) {
		char line[128];
		BedRules::FormatEvent(out.ev[i], line, sizeof(line));
		std::printf("EV %s\n", line);
	}
	if (out.overflow) {
		std::fprintf(stderr, "경고: 사건 %u개 버림(EventBuf 가득)\n", out.overflow);
	}
}

void Occ(bool &prev, bool now, int64_t ms)
{
	if (now != prev) {
		std::printf("OCC %lld %d\n", static_cast<long long>(ms), now ? 1 : 0);
		prev = now;
	}
}

} // namespace

int main()
{
	Tracker::Zone zone = { -1500, 1500, 0, 3000 };
	BedRules::Config cfg = {};
	bool haveCfg = false;
	Tracker::State tracker;
	BedRules::State bed;
	bool occ = false;
	int64_t nextTickMs = 1000;

	std::string line;
	while (std::getline(std::cin, line)) {
		std::istringstream in(line);
		std::string tag;
		if (!(in >> tag)) {
			continue;
		}
		if (tag == "Z") {
			in >> zone.xmin >> zone.xmax >> zone.ymin >> zone.ymax;
		} else if (tag == "C") {
			BedRules::Bed &b = cfg.bed;
			long long leave, hold, gap, min, follow;
			in >> b.xmin >> b.xmax >> b.ymin >> b.ymax >> b.splitX >> b.edgeMm >> leave >> hold >> gap >> min >>
				follow;
			cfg.leaveDelayMs = leave;
			cfg.holdMaxMs = hold;
			cfg.moveGapMs = gap;
			cfg.moveMinMs = min;
			cfg.followMs = follow;
			haveCfg = static_cast<bool>(in);
		} else if (tag == "F") {
			if (!haveCfg) {
				std::cerr << "C 줄이 F 보다 먼저 와야 합니다\n";
				return 2;
			}
			long long ms;
			int n;
			in >> ms >> n;
			Ld2450::Target targets[Ld2450::kTargetSlots] = {};
			for (int i = 0; i < n && i < Ld2450::kTargetSlots; i++) {
				int x, y;
				in >> x >> y;
				targets[i].present = true;
				targets[i].x = static_cast<int16_t>(x);
				targets[i].y = static_cast<int16_t>(y);
			}
			if (!in) {
				std::cerr << "프레임 형식 오류: " << line << "\n";
				return 2;
			}
			for (; nextTickMs <= ms; nextTickMs += 1000) {
				BedRules::EventBuf out;
				bool v = BedRules::Tick(bed, cfg, nextTickMs, out);
				Print(out);
				Occ(occ, v, nextTickMs);
			}
			Tracker::Update(tracker, targets, zone, ms);
			BedRules::EventBuf out;
			bool v = BedRules::Update(bed, cfg, tracker, ms, out);
			Print(out);
			Occ(occ, v, ms);
		}
	}
	return 0;
}
