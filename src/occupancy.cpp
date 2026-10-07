#include "occupancy.h"

namespace Occupancy {

bool Update(Debouncer &state, uint8_t liveCount, int64_t nowMs)
{
	if (liveCount > 0) {
		state.occupied = true;
		state.everLive = true;
		state.lastLiveMs = nowMs;
	} else if (state.occupied && state.everLive && (nowMs - state.lastLiveMs) >= kUnoccupiedMs) {
		state.occupied = false;
	}
	return state.occupied;
}

} /* namespace Occupancy */
