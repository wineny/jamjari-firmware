// 구역 필터 + 트래커(tracker.h) + 점유 판정(occupancy.h)을 녹화에 대고 돌려 프레임별 CSV 를 뽑는다.
// 펌웨어 app_task.cpp 의 OnRadarFrame 과 같은 순서: 파싱 → 구역 필터·트래커 → 살아 있는 트랙 수로 점유 판정.
//
// 프레임 시각: 녹화 파일에 도착 시각이 없어서 「균일 간격」을 가정한다.
//   now_ms = (int64) (frame_index × interval_ms)   — reference_tracker.py 와 같은 식
//
// 사용: run_tracker <입력.bin> <interval-ms> <구역 "xmin,xmax,ymin,ymax" | none> <출력.csv>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../src/ld2450.h"
#include "../src/ld2450_parser.h"
#include "../src/occupancy.h"
#include "../src/tracker.h"

int main(int argc, char **argv)
{
	if (argc != 5) {
		std::cerr << "사용법: " << argv[0] << " <입력.bin> <interval-ms> <xmin,xmax,ymin,ymax|none> <출력.csv>\n";
		return 2;
	}

	std::ifstream in(argv[1], std::ios::binary);
	if (!in) {
		std::cerr << "입력 파일을 열 수 없습니다: " << argv[1] << "\n";
		return 2;
	}
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	double intervalMs = std::atof(argv[2]);

	Tracker::Zone zone = { INT32_MIN, INT32_MAX, INT32_MIN, INT32_MAX };
	if (std::string(argv[3]) != "none" &&
	    std::sscanf(argv[3], "%d,%d,%d,%d", &zone.xmin, &zone.xmax, &zone.ymin, &zone.ymax) != 4) {
		std::cerr << "구역 형식 오류: " << argv[3] << "\n";
		return 2;
	}

	std::ofstream out(argv[4]);
	if (!out) {
		std::cerr << "출력 파일을 만들 수 없습니다: " << argv[4] << "\n";
		return 2;
	}
	out << "frame_index,now_ms,raw_live,alive,overflow,tracks,occupied\n";

	Ld2450::FrameSyncState sync;
	Tracker::State tracker;
	Occupancy::Debouncer debouncer;
	uint32_t frameIndex = 0;
	size_t maxAlive = 0;
	uint32_t transitions = 0;
	bool prevOccupied = false;

	for (uint8_t byte : data) {
		if (!Ld2450::FeedByte(sync, byte)) {
			continue;
		}
		Ld2450::Target targets[Ld2450::kTargetSlots];
		uint8_t live = Ld2450::ParseFrame(sync.frame, targets);

		int64_t nowMs = static_cast<int64_t>(frameIndex * intervalMs);
		size_t alive = Tracker::Update(tracker, targets, zone, nowMs);
		bool occupied = Occupancy::Update(debouncer, static_cast<uint8_t>(alive), nowMs);
		if (alive > maxAlive) {
			maxAlive = alive;
		}

		out << frameIndex << ',' << nowMs << ',' << static_cast<int>(live) << ',' << alive << ','
		    << tracker.overflow << ',';
		for (size_t i = 0; i < tracker.count; i++) {
			const Tracker::Track &t = tracker.tracks[i];
			char name[Tracker::kNameBufLen];
			Tracker::FormatName(t.nameIndex, name);
			out << (i ? ";" : "") << name << ':' << t.x << ':' << t.y << ':' << t.speed << ':'
			    << (t.seen ? 1 : 0);
		}
		out << ',' << (occupied ? 1 : 0) << '\n';

		if (frameIndex == 0 || occupied != prevOccupied) {
			transitions++;
			std::cerr << "t=" << nowMs << "ms frame#" << frameIndex << " occupied -> " << occupied << "\n";
		}
		prevOccupied = occupied;
		frameIndex++;
	}

	std::cerr << "프레임 " << frameIndex << "개, 최대 동시 트랙 " << maxAlive << "개 (칸 " << Tracker::kMaxTracks
		  << "), overflow " << tracker.overflow << ", 점유 전이 " << transitions << "회, 최종 occupied="
		  << prevOccupied << "\n";
	return 0;
}
