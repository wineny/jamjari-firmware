#include "replay_frames.h"

#include <cstring>

namespace ReplayFrames {

size_t LenOf(uint8_t type)
{
	switch (static_cast<Type>(type)) {
	case Type::kRadar:
		return 34;
	case Type::kG60:
		return 18;
	case Type::kEnd:
	case Type::kStart:
		return 10;
	case Type::kLd2450:
		return 30;
	}
	return 0;
}

namespace {

/* 헤더 자리 i 에 이 바이트가 올 수 있나 */
bool HeaderOk(size_t i, uint8_t b)
{
	switch (i) {
	case 0:
		return b == 0xAA;
	case 1:
		return b == 0xFF;
	case 2:
		return LenOf(b) != 0;
	default:
		return b == 0x00;
	}
}

} /* namespace */

void Feed(SyncState &s, uint8_t byte, Emit emit, void *ctx)
{
	/* 먹일 바이트 줄. 푸터가 어긋나면 첫 바이트만 버리고 남은 바이트를 줄 앞에 되돌려 처음부터 다시 먹인다
	 * (그 안에 더 짧은 프레임 — 장면 끝 10바이트 등 — 이 통째로 들어 있을 수 있어 완성되면 그대로 내보낸다).
	 * ISR 에서 불리므로 재귀 없이 고정 크기(되돌린 바이트 ≤ 33 + 아직 안 먹인 ≤ 33). */
	uint8_t q[2 * kMaxLen];
	size_t head = 0, n = 0;
	q[n++] = byte;
	while (head < n) {
		uint8_t b = q[head++];
		if (s.fill < 4) {
			if (HeaderOk(s.fill, b)) {
				s.frame[s.fill++] = b;
				if (s.fill == 4) {
					s.want = LenOf(s.frame[2]);
				}
			} else {
				s.dropped += s.fill;
				s.fill = 0;
				if (b == 0xAA) {
					s.frame[s.fill++] = b;
				} else {
					s.dropped++;
				}
			}
			continue;
		}
		s.frame[s.fill++] = b;
		if (s.fill < s.want) {
			continue;
		}
		if (s.frame[s.want - 2] == 0x55 && s.frame[s.want - 1] == 0xCC) {
			s.fill = 0;
			s.want = 0;
			emit(s.frame, ctx);
			continue;
		}
		uint8_t back[2 * kMaxLen];
		size_t m = s.fill - 1;
		memcpy(back, s.frame + 1, m);
		memcpy(back + m, q + head, n - head);
		n = m + (n - head);
		head = 0;
		memcpy(q, back, n);
		s.dropped++;
		s.fill = 0;
		s.want = 0;
	}
}

} /* namespace ReplayFrames */
