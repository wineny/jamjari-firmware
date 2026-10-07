/*
 * 재생 빌드용 프레임 (10/5 N) — 맥 dummy/보드재생.py 가 VCOM0 으로 보내는 세 종류. 호스트·타깃 같이 컴파일.
 * 진짜 레이더(LD2450, 헤더 AA FF 03 00, 30바이트)는 ld2450_parser 가 그대로 맡는다. 재생 빌드만 이걸 쓴다.
 *
 *   레이더+시각  AA FF 07 00 | t_ms u32 | 타깃 3 × 8 (LD2450 과 같은 모양) | 55 CC    = 34바이트
 *   60GHz 좌·우  AA FF 06 00 | sec u32 | L: in_bed breath heart move | R: 같음 | 55 CC = 18바이트
 *                (각 1바이트, 숨·심장이 없으면 0xFF)
 *   장면 끝      AA FF 08 00 | 0 u32 | 55 CC                                         = 10바이트
 *   재생 시작    AA FF 09 00 | 0 u32 | 55 CC                                         = 10바이트 (보드 추정을 새로 시작)
 * 숫자는 모두 리틀엔디안. t_ms·sec 은 장면 시각이다(보드 추정은 이 시각으로 돈다, 설계-보드추정.md).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ReplayFrames {

/* kLd2450 = 시각 없는 옛 재생(보드재생.py --g60 없이). 보드 추정은 안 돌고 기존 판단만 돈다 */
enum class Type : uint8_t { kLd2450 = 0x03, kG60 = 0x06, kRadar = 0x07, kEnd = 0x08, kStart = 0x09 };

constexpr size_t kMaxLen = 34;

size_t LenOf(uint8_t type); /* 모르는 종류면 0 */

struct SyncState {
	uint8_t frame[kMaxLen] = {};
	size_t fill = 0;
	size_t want = 0; /* 헤더를 다 받으면 그 종류의 길이 */
	uint32_t dropped = 0;
};

/* 완성된 프레임 하나(길이 = LenOf(frame[2])). frame 은 다음 Feed 전까지만 유효 */
using Emit = void (*)(const uint8_t *frame, void *ctx);

/* 바이트 하나를 먹인다. 완성된 프레임마다 emit 을 부른다(한 바이트에 둘 이상일 수도 있다 —
 * 푸터가 어긋나면 첫 바이트만 버리고 남은 바이트 안에서 다시 찾기 때문). ISR 에서 불러도 된다(재귀·할당 없음). */
void Feed(SyncState &state, uint8_t byte, Emit emit, void *ctx);

inline uint32_t U32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} /* namespace ReplayFrames */
