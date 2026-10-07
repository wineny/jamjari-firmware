/*
 * 점유(Occupancy) 판정 — app_task.cpp 의 OnRadarFrame + k_timer(10초) 로직을
 * Zephyr 없이도 재생·검증할 수 있게 뽑은 순수 함수.
 *
 * 실기(app_task.cpp)는 "타깃이 하나라도 있으면 즉시 occupied, 10초 동안 타깃이
 * 하나도 없으면 unoccupied" 를 k_timer 로 구현한다. LD2450 은 타깃이 없어도 빈
 * 프레임(liveCount=0)을 계속 보내고 OnRadarFrame 은 그 프레임마다 불리므로,
 * "프레임이 도착할 때마다 이 함수를 부른다"는 방식이 k_timer 방식과 관측 가능한
 * 차이가 없다. 단, 레이더가 통째로 끊겨 프레임 자체가 안 오는 경우는 이 함수가
 * 아니라 여전히 app_task.cpp 의 실제 k_timer 가 담당한다 — 그 배선은 바꾸지
 * 않았다(동작 불변 원칙). 이 모듈은 녹화 데이터로 점유 타임라인을 검증하는
 * 용도로만 쓴다.
 */

#pragma once

#include <cstdint>

namespace Occupancy {

/* app_task.cpp kUnoccupiedDelay = K_SECONDS(10) 과 같은 값(ms) */
constexpr int64_t kUnoccupiedMs = 10000;

struct Debouncer {
	bool occupied = false;
	bool everLive = false;
	int64_t lastLiveMs = 0;
};

/*
 * 프레임 하나(또는 임의의 시점)마다 부른다. liveCount>=1 이면 즉시 occupied=true 로
 * 하고 시계를 다시 잰다. liveCount==0 이면 마지막 liveCount>=1 이후 kUnoccupiedMs 가
 * 지났을 때만 occupied=false 로 내린다. 반환값은 갱신된 occupied 상태.
 */
bool Update(Debouncer &state, uint8_t liveCount, int64_t nowMs);

} /* namespace Occupancy */
