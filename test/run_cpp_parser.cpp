// 호스트(clang++, C++17)에서 돌아가는 LD2450 파서 테스트 러너.
// ld2450_parser.h/.cpp (Zephyr 의존 없음)를 그대로 링크해서, 실제 타깃 코드와
// 동일한 로직으로 .bin 을 바이트 단위로 흘려 넣고 프레임을 CSV로 뽑는다.
//
// 사용: run_cpp_parser <입력.bin> <출력.csv>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#include "../src/ld2450.h"
#include "../src/ld2450_parser.h"

int main(int argc, char **argv)
{
	if (argc != 3) {
		std::cerr << "사용법: " << argv[0] << " <입력.bin> <출력.csv>\n";
		return 2;
	}

	std::ifstream in(argv[1], std::ios::binary);
	if (!in) {
		std::cerr << "입력 파일을 열 수 없습니다: " << argv[1] << "\n";
		return 2;
	}
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	std::ofstream out(argv[2]);
	if (!out) {
		std::cerr << "출력 파일을 만들 수 없습니다: " << argv[2] << "\n";
		return 2;
	}
	out << "frame_index,live_count,"
	       "t1_present,t1_x,t1_y,t1_speed,"
	       "t2_present,t2_x,t2_y,t2_speed,"
	       "t3_present,t3_x,t3_y,t3_speed,dropped\n";

	Ld2450::FrameSyncState sync;
	uint32_t frameIndex = 0;

	for (uint8_t byte : data) {
		if (!Ld2450::FeedByte(sync, byte)) {
			continue;
		}
		Ld2450::Target targets[Ld2450::kTargetSlots];
		uint8_t live = Ld2450::ParseFrame(sync.frame, targets);

		out << frameIndex << ',' << static_cast<int>(live);
		for (const Ld2450::Target &t : targets) {
			out << ',' << (t.present ? 1 : 0) << ',' << t.x << ',' << t.y << ',' << t.speed;
		}
		out << ',' << sync.dropped << '\n';
		frameIndex++;
	}

	std::cerr << "프레임 " << frameIndex << "개, 입력 바이트 " << data.size() << "개, 버린 바이트 " << sync.dropped
		  << "개\n";
	/* 버린 바이트 세기 검산: 입력 = 30 × 프레임 + 버림 + 아직 조립 중 */
	if (data.size() != frameIndex * Ld2450::kFrameLen + sync.dropped + sync.fill) {
		std::cerr << "❌ 바이트 수 검산 실패\n";
		return 1;
	}
	return 0;
}
