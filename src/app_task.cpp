/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"
#include "bed_rules.h"
#ifdef CONFIG_JAMJARI_DIRECT_UPLOAD
#include "direct_upload.h"
#endif
#ifdef CONFIG_JAMJARI_FUSION
#include "fusion.h"
#include "ld2450_parser.h"
#include "replay_frames.h"
#endif
#include "ld2450.h"
#include "tracker.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "board/board.h"
#include "clusters/identify.h"

#include "lib/core/CHIPError.h"
#include <app-common/zap-generated/attribute-type.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/clusters/occupancy-sensor-server/occupancy-sensor-server.h>
#include <app/util/attribute-table.h>

#include <dk_buttons_and_leds.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace
{
constexpr chip::EndpointId kOccupancySensorEndpointId = 1;
/* 0x0431 Ambient Context Sensing (Matter 다음 규격 초안, ZAP 정의는 src/default_zap/zcl/) */
constexpr chip::ClusterId kAmbientContextSensingClusterId = 0x0431;
constexpr chip::AttributeId kHumanActivityDetectedAttributeId = 0x0000;
/*
 * 시연용 가상 조도 (Light Sensor). 5 lux 고정(ZAP 기본값 6991 = 10000*log10(5)+1).
 * SmartThings Matter Sensor 드라이버에 「동작+온도」 모양이 없고 「동작+조도+온도」는 있어서 넣었다.
 * 예전 ep2 「감지 멈춤」 On/Off 스위치는 SmartThings 가 기기 전체를 플러그로 잡아 동작 감지를 가려서 뺐다 → BTN2 만 남음.
 */
constexpr chip::EndpointId kLightSensorEndpointId = 2;
/* 시연용 가상 온도 (Temperature Sensor). 값은 버튼·셸(temp)로만 바뀐다. */
constexpr chip::EndpointId kTemperatureSensorEndpointId = 3;

atomic_t sOccupied = ATOMIC_INIT(0);
/* 멈춤 요청(1=멈춤). Matter 스레드가 쓰고, 리더 스레드가 ApplyPauseState 로 반영한다. */
atomic_t sPaused = ATOMIC_INIT(0);

/* Identify 가 끝나면 LED2 를 끄지 말고 지금 Occupancy 로 되돌린다 */
void RestoreOccupancyLed()
{
	Nrf::PostTask([] { Nrf::GetBoard().GetLED(Nrf::DeviceLeds::LED2).Set(atomic_get(&sOccupied) != 0); });
}

Nrf::Matter::IdentifyCluster sIdentifyCluster(kOccupancySensorEndpointId, true, RestoreOccupancyLed);
Nrf::Matter::IdentifyCluster sLightIdentifyCluster(kLightSensorEndpointId, true, RestoreOccupancyLed);
Nrf::Matter::IdentifyCluster sTempIdentifyCluster(kTemperatureSensorEndpointId, true, RestoreOccupancyLed);

/* FeatureMap = Radar. 이 클러스터의 FeatureMap/ClusterRevision 은 이 인스턴스가 응답한다. */
Clusters::OccupancySensing::Instance
	sOccupancyInstance(BitMask<Clusters::OccupancySensing::Feature>(Clusters::OccupancySensing::Feature::kRadar));

/* 지켜볼 구역(mm). Kconfig CONFIG_JAMJARI_ZONE_* 로 바꾼다(README 「구역 조정」). */
constexpr Tracker::Zone kZone = { CONFIG_JAMJARI_ZONE_XMIN, CONFIG_JAMJARI_ZONE_XMAX, CONFIG_JAMJARI_ZONE_YMIN,
				  CONFIG_JAMJARI_ZONE_YMAX };
constexpr int64_t kRadarSilentMs = 5000;

/* 침대 칸 규칙(bed_rules.h). 침대 좌표가 모두 0 이면 예전 규칙. Kconfig CONFIG_JAMJARI_BED_* 등 */
constexpr BedRules::Config kBedConfig = {
	{ CONFIG_JAMJARI_BED_XMIN, CONFIG_JAMJARI_BED_XMAX, CONFIG_JAMJARI_BED_YMIN, CONFIG_JAMJARI_BED_YMAX,
	  CONFIG_JAMJARI_BED_SPLIT_X, CONFIG_JAMJARI_BED_EDGE_MM },
	CONFIG_JAMJARI_LEAVE_DELAY_MS,
	static_cast<int64_t>(CONFIG_JAMJARI_HOLD_MAX_MIN) * 60 * 1000,
	CONFIG_JAMJARI_MOVE_GAP_MS,
	CONFIG_JAMJARI_MOVE_MIN_MS,
	CONFIG_JAMJARI_FOLLOW_MS,
};

/* 침대 좌표 오설정은 빌드에서 막는다(미설정 = 모두 0 이면 검사 안 함) */
constexpr bool kBedSet = CONFIG_JAMJARI_BED_XMIN != 0 || CONFIG_JAMJARI_BED_XMAX != 0 ||
			 CONFIG_JAMJARI_BED_YMIN != 0 || CONFIG_JAMJARI_BED_YMAX != 0;
static_assert(!kBedSet || (CONFIG_JAMJARI_BED_XMIN < CONFIG_JAMJARI_BED_XMAX &&
			   CONFIG_JAMJARI_BED_YMIN < CONFIG_JAMJARI_BED_YMAX),
	      "침대 좌표: XMIN<XMAX, YMIN<YMAX 이어야 한다 (네 값을 다 넣었나?)");
static_assert(!kBedSet || (CONFIG_JAMJARI_BED_XMIN <= CONFIG_JAMJARI_BED_SPLIT_X &&
			   CONFIG_JAMJARI_BED_SPLIT_X <= CONFIG_JAMJARI_BED_XMAX),
	      "침대 좌표: SPLIT_X 는 XMIN..XMAX 안이어야 한다");
static_assert(!kBedSet || (2 * CONFIG_JAMJARI_BED_EDGE_MM < CONFIG_JAMJARI_BED_XMAX - CONFIG_JAMJARI_BED_XMIN &&
			   CONFIG_JAMJARI_BED_EDGE_MM < CONFIG_JAMJARI_BED_YMAX - CONFIG_JAMJARI_BED_YMIN),
	      "침대 좌표: 가장자리 띠가 침대보다 넓다 (EDGE_MM 줄이기)");
static_assert(!kBedSet || (CONFIG_JAMJARI_ZONE_XMIN <= CONFIG_JAMJARI_BED_XMIN &&
			   CONFIG_JAMJARI_BED_XMAX <= CONFIG_JAMJARI_ZONE_XMAX &&
			   CONFIG_JAMJARI_ZONE_YMIN <= CONFIG_JAMJARI_BED_YMIN &&
			   CONFIG_JAMJARI_BED_YMAX <= CONFIG_JAMJARI_ZONE_YMAX),
	      "침대 좌표: 침대는 구역(JAMJARI_ZONE_*) 안에 있어야 한다 (밖은 트래커가 버린다)");

/* 아래 셋은 LD2450 리더 스레드에서만 만진다(프레임 핸들러와 1초 틱이 같은 스레드) */
Tracker::State sTracker;
BedRules::State sBed;
uint8_t sLastRawCount;
int64_t sLastSilentWarnMs;
bool sAppliedPaused; /* 리더 스레드가 마지막으로 반영한 멈춤 상태 */

void ReportOccupancy(bool occupied)
{
	/* 상태가 바뀔 때만 Matter 스레드에 넘긴다 */
	if (atomic_set(&sOccupied, occupied ? 1 : 0) == (occupied ? 1 : 0)) {
		return;
	}
	LOG_INF("Occupancy -> %d", occupied);
	/*
	 * 레이더 리더 스레드와 시스템 워크큐(SetSensing — BTN2 콜백) 둘 다 부른다. 두 스레드가 거의 같이 부르면 일이 두 개
	 * 쌓이는데, 일은 넘길 때 값이 아니라 실행할 때 sOccupied 를 읽는다 — 마지막 일이 늘 최신 값을 쓰므로
	 * 속성과 LED2 가 sOccupied 와 어긋난 채 남지 않는다. LED 는 앱 스레드에서 바꾼다.
	 */
	DeviceLayer::PlatformMgr().ScheduleWork(
		[](intptr_t) {
			bool now = atomic_get(&sOccupied) != 0;
			Nrf::PostTask([now] { Nrf::GetBoard().GetLED(Nrf::DeviceLeds::LED2).Set(now); });
			BitMask<Clusters::OccupancySensing::OccupancyBitmap> value;
			if (now) {
				value.Set(Clusters::OccupancySensing::OccupancyBitmap::kOccupied);
			}
			Protocols::InteractionModel::Status status =
				Clusters::OccupancySensing::Attributes::Occupancy::Set(kOccupancySensorEndpointId,
										       value);
			if (status != Protocols::InteractionModel::Status::Success) {
				LOG_ERR("Updating occupancy failed %x", to_underlying(status));
			}
			/*
			 * 0x0431 Ambient Context Sensing(초안) HumanActivityDetected 도 같은 값으로. SDK 에 이 클러스터의
			 * Accessors 가 없어서 속성 저장소에 바로 쓴다(Matter 스레드라 잠금 안). HumanActivityType 은 Other(0) 고정.
			 */
			uint8_t detected = now ? 1 : 0;
			status = emberAfWriteAttribute(kOccupancySensorEndpointId, kAmbientContextSensingClusterId,
						       kHumanActivityDetectedAttributeId, &detected,
						       ZCL_BOOLEAN_ATTRIBUTE_TYPE);
			if (status != Protocols::InteractionModel::Status::Success) {
				LOG_ERR("Updating 0x0431 HumanActivityDetected failed %x", to_underlying(status));
			}
		},
		0);
}

/*
 * 리더 스레드에서 프레임·틱마다 맨 먼저 부른다. 반환값 = 지금 멈춤인가.
 * 멈춤/재개가 바뀌는 순간 트래커와 침대 칸 상태를 새로 시작한다 — 열려 있던 사건(움직임 묶음·away)은
 * 버린다. 사건 번호(seq)만 이어 간다(맥 쪽이 번호로 줄을 맞춘다).
 */
bool ApplyPauseState()
{
	bool paused = atomic_get(&sPaused) != 0;
	if (paused == sAppliedPaused) {
		return paused;
	}
	sAppliedPaused = paused;
	uint32_t seq = sBed.seq;
	sTracker = {};
	sBed = {};
	sBed.seq = seq;
	if (paused) {
		/* 멈춤 중엔 비어 있음으로 둔다(SetSensing 도 내리지만, 그 사이 프레임이 올렸을 수 있다) */
		ReportOccupancy(false);
	}
	return paused;
}

/* 사건 줄 출력 (Kconfig JAMJARI_EVENT_LOG). 맥은 JJ1, 이후만 읽는다.
 * 직접 전송(JAMJARI_DIRECT_UPLOAD)이면 같은 사건을 보낼 줄에도 넣는다. */
void LogEvents(const BedRules::EventBuf &out)
{
#ifdef CONFIG_JAMJARI_DIRECT_UPLOAD
	for (size_t i = 0; i < out.n; i++) {
		DirectUpload::QueueEvent(out.ev[i]);
	}
#endif
#ifdef CONFIG_JAMJARI_EVENT_LOG
	for (size_t i = 0; i < out.n; i++) {
		char line[96];
		BedRules::FormatEvent(out.ev[i], line, sizeof(line));
		LOG_INF("%s", line);
	}
	if (out.overflow) {
		LOG_WRN("사건 %u개 못 냄 (EventBuf 가득)", static_cast<unsigned>(out.overflow));
	}
#else
	ARG_UNUSED(out);
#endif
}

/* 프레임 한 줄 출력: F <ms> <슬롯×3> <트랙 수> <occ> (Kconfig JAMJARI_FRAME_LOG, 맥의 레이더_지도.py --source nrf 가 읽는다) */
void LogFrame(const Ld2450::Target (&targets)[Ld2450::kTargetSlots])
{
#ifdef CONFIG_JAMJARI_FRAME_LOG
	/* 슬롯 하나 최대 " -32767,-32767,-327670" 22자 */
	char slots[Ld2450::kTargetSlots * 24] = "";
	size_t len = 0;
	for (size_t i = 0; i < Ld2450::kTargetSlots; i++) {
		const Ld2450::Target &t = targets[i];
		int n = t.present ? snprintf(slots + len, sizeof(slots) - len, " %d,%d,%d", t.x, t.y,
					     static_cast<int>(t.speed))
				  : snprintf(slots + len, sizeof(slots) - len, " -");
		if (n < 0 || static_cast<size_t>(n) >= sizeof(slots) - len) {
			break;
		}
		len += static_cast<size_t>(n);
	}
	LOG_INF("F %u%s %u %d", static_cast<unsigned>(k_uptime_get_32()), slots,
		static_cast<unsigned>(sTracker.count), static_cast<int>(atomic_get(&sOccupied)));
#else
	ARG_UNUSED(targets);
#endif
}

/*
 * LD2450 리더 스레드에서 프레임마다 불린다.
 * 구역 필터·트래커 → 침대 칸 규칙(bed_rules) 순서. 원시 타깃이 아니라 트래커 결과로 판정한다 —
 * 구역 밖 유령 점이 Occupancy 를 붙잡지 않게. 침대 좌표 미설정이면 예전 규칙(구역 안 사람 1명 이상 → 1,
 * 마지막으로 있던 때부터 10초 → 0). (호스트 테스트: test/run_tracker.cpp 는 예전 규칙을 occupancy.h 로 본다)
 */
void OnRadarFrame(const Ld2450::Target (&targets)[Ld2450::kTargetSlots], uint8_t liveCount)
{
	sLastRawCount = liveCount;
	if (ApplyPauseState()) {
		return;
	}
	int64_t nowMs = k_uptime_get();
	Tracker::Update(sTracker, targets, kZone, nowMs);
	BedRules::EventBuf out;
	bool occupied = BedRules::Update(sBed, kBedConfig, sTracker, nowMs, out);
	/* 판단 도중 멈춤이 들어왔으면 결과를 버린다(JJ1,PAUSE,1 뒤에 occ 1·JJ1,EV 가 나가지 않게) */
	if (atomic_get(&sPaused)) {
		return;
	}
	ReportOccupancy(occupied);
	LogEvents(out);
	LogFrame(targets);
}

#ifdef CONFIG_JAMJARI_FUSION
/*
 * 보드 추정(fusion, 설계-보드추정.md). 맥이 장면 시각을 붙여 보낸 프레임으로 칸 상태·머문 자리를 계산한다.
 * 리더 스레드에서만 불린다(sFusion 은 이 스레드 전용). 결과는 로그 한 줄 + 웹 전송 줄(seat·spot).
 *   JJ1,SEAT,<장면 초>,<L|R>,<0 비어 있음|1 사람|2 반려동물로 추정|3 구분 안 됨>,<단서 0~3: A=1 B=2>
 *   JJ1,SPOT,<순위>,<from 초>,<to 초>,<x>,<y>,<머문 초>
 */
Fusion::Engine sFusion;
Fusion::SideOut sSeatPrev[2];
bool sSeatSent;
uint32_t sFusionSeq; /* 웹 src_id 가 bed_rules 사건과 안 겹치게 위 비트를 켠 번호를 쓴다 */
int32_t sSec0 = -1, sSecLast; /* 장면 초 ↔ 보드 시계: 첫 초와 마지막 초에 받은 보드 시각으로 선형 대응 */
int64_t sUp0, sUpLast;

void FusionReset()
{
	Fusion::Init(sFusion);
	sSeatSent = false;
	sSec0 = -1;
}

int64_t SceneToUptime(int32_t sec)
{
	if (sSec0 < 0 || sSecLast <= sSec0) {
		return sUp0;
	}
	return sUp0 + (static_cast<int64_t>(sec) - sSec0) * (sUpLast - sUp0) / (sSecLast - sSec0);
}

void QueueFusionEvent(BedRules::Kind kind, int64_t startMs, int64_t endMs, uint8_t size, int32_t x, int32_t y,
		      uint8_t count)
{
#ifdef CONFIG_JAMJARI_DIRECT_UPLOAD
	BedRules::Event ev = {};
	ev.seq = 0x80000000u | ++sFusionSeq;
	ev.kind = kind;
	ev.startMs = startMs;
	ev.endMs = endMs;
	ev.size = size;
	ev.x = x;
	ev.y = y;
	ev.count = count;
	DirectUpload::QueueEvent(ev);
#endif
}

Fusion::G60 G60Side(const uint8_t *p)
{
	return { p[0], static_cast<int16_t>(p[1] == 0xFF ? -1 : p[1]), static_cast<int16_t>(p[2] == 0xFF ? -1 : p[2]), p[3] };
}

void OnReplayFrame(const uint8_t *frame)
{
	using ReplayFrames::Type;
	Type type = static_cast<Type>(frame[2]);
	if (type == Type::kStart) {
		LOG_INF("fusion: 재생 시작 → 새로 시작");
		FusionReset();
		return;
	}
	/* 감지 멈춤 중엔 추정도 멈춘다. 장면 끝은 리셋만 한다(멈춘 동안의 결과는 안 낸다) */
	if (atomic_get(&sPaused)) {
		if (type == Type::kEnd) {
			LOG_INF("fusion: 감지 멈춤 중 장면 끝 → 머문 자리 안 내고 새로 시작");
			FusionReset();
		}
		return;
	}
	switch (type) {
	case Type::kRadar: {
		Ld2450::Target targets[Ld2450::kTargetSlots];
		Ld2450::ParseFrame(frame + 4, targets);
		Fusion::Point pts[Ld2450::kTargetSlots];
		size_t n = 0;
		for (const auto &t : targets) {
			if (t.present) {
				pts[n++] = { t.x, t.y };
			}
		}
		Fusion::Radar(sFusion, ReplayFrames::U32(frame + 4) / 1000.0, pts, n);
		break;
	}
	case Type::kG60: {
		int32_t sec = static_cast<int32_t>(ReplayFrames::U32(frame + 4));
		/* 장면 초는 1씩 이어진다. 안 이어지면(시작 프레임을 놓친 다른 재생) 새로 시작 */
		if (sSec0 >= 0 && sec != sFusion.judged) {
			LOG_INF("fusion: 장면 초가 안 이어짐(%d초, 기대 %d초) → 새로 시작", sec, sFusion.judged);
			FusionReset();
		}
		int64_t now = k_uptime_get();
		if (sSec0 < 0) {
			sSec0 = sec;
			sUp0 = now;
		}
		sSecLast = sec;
		sUpLast = now;
		Fusion::SecondOut o = Fusion::Judge(sFusion, sec, G60Side(frame + 8), G60Side(frame + 12));
		for (int side = 0; side < 2; side++) {
			const Fusion::SideOut &s = o.side[side];
			if (sSeatSent && s.state == sSeatPrev[side].state && s.clues == sSeatPrev[side].clues) {
				continue;
			}
			sSeatPrev[side] = s;
			LOG_INF("JJ1,SEAT,%d,%c,%u,%u", sec, side ? 'R' : 'L', static_cast<unsigned>(s.state),
				static_cast<unsigned>(s.clues));
			/* 칸은 x 부호로(L 음수·R 양수, 칸 가운데), 상태는 size, 단서는 count */
			QueueFusionEvent(BedRules::Kind::kSeat, now, now, static_cast<uint8_t>(s.state), side ? 400 : -400,
					 1300, s.clues);
		}
		sSeatSent = true;
		break;
	}
	case Type::kEnd: {
		Fusion::Spot spots[Fusion::kMaxSpots];
		size_t n = Fusion::End(sFusion, spots);
		LOG_INF("fusion: 장면 끝 %d초, 머문 자리 %u개 (넘침 트랙 %u/%u 자리 %u)", sFusion.judged,
			static_cast<unsigned>(n), static_cast<unsigned>(sFusion.m.overflow),
			static_cast<unsigned>(sFusion.n.overflow), static_cast<unsigned>(sFusion.s.overflow));
		for (size_t i = 0; i < n; i++) {
			const Fusion::Spot &sp = spots[i];
			LOG_INF("JJ1,SPOT,%u,%d,%d,%d,%d,%d", static_cast<unsigned>(i + 1), sp.from, sp.to, sp.x, sp.y,
				sp.still);
			int32_t minutes = sp.still / 60;
			QueueFusionEvent(BedRules::Kind::kSpot, SceneToUptime(sp.from), SceneToUptime(sp.to),
					 static_cast<uint8_t>(i + 1), sp.x, sp.y, static_cast<uint8_t>(minutes > 255 ? 255 : minutes));
		}
		FusionReset();
		break;
	}
	default:
		break;
	}
}
#endif

/* LD2450 리더 스레드에서 1초마다 불린다: 레이더 상태 요약 + 신호 끊김 경고 */
void OnRadarTick(const Ld2450::Stats &stats, int64_t nowMs)
{
	bool paused = ApplyPauseState();
	if (!paused) {
		/* 프레임이 끊겨도 시간으로 내리는 판단(occ 0·움직임 닫기)은 여기서 */
		BedRules::EventBuf out;
		bool occupied = BedRules::Tick(sBed, kBedConfig, nowMs, out);
		/* 판단 도중 멈춤이 들어왔으면 결과를 버린다(OnRadarFrame 과 같은 이유) */
		if (!atomic_get(&sPaused)) {
			ReportOccupancy(occupied);
			LogEvents(out);
		}
	}
#ifdef CONFIG_JAMJARI_DIRECT_UPLOAD
	/* 레이더 상태 숫자는 멈춤 중에도 갱신(보낼 묶음의 상태 칸) */
	DirectUpload::NoteRadarStats(stats);
#endif

	/* 레이더 끊김 경고는 멈춤 중에도 낸다 */
	int64_t silentMs = nowMs - (stats.lastFrameMs < 0 ? 0 : stats.lastFrameMs);
	if (silentMs > kRadarSilentMs && nowMs - sLastSilentWarnMs >= kRadarSilentMs) {
		sLastSilentWarnMs = nowMs;
		LOG_WRN("레이더 신호 없음 — 배선/5V 확인 (%lld초째 프레임 없음, 누적 %u프레임)",
			static_cast<long long>(silentMs / 1000), stats.frames);
	}

#ifdef CONFIG_JAMJARI_RADAR_SUMMARY_LOG
	static Ld2450::Stats sPrev = { 0, 0, 0, -1 };
	if (paused) {
		sPrev = stats; /* 재개 첫 줄의 fps 가 멈춤 동안 누적값이 되지 않게 */
		return;
	}
	char people[Tracker::kMaxTracks * 24] = "";
	size_t len = 0;
	for (size_t i = 0; i < sTracker.count && len < sizeof(people); i++) {
		const Tracker::Track &t = sTracker.tracks[i];
		char name[Tracker::kNameBufLen];
		Tracker::FormatName(t.nameIndex, name);
		/* 이번 프레임에 안 잡혀 이름만 살려 둔 트랙은 뒤에 ? */
		int n = snprintf(people + len, sizeof(people) - len, " %s%s(%d,%d)", name, t.seen ? "" : "?",
				 t.x, t.y);
		if (n < 0) {
			break;
		}
		len += static_cast<size_t>(n);
	}
	LOG_INF("radar %u fps, drop %uB, qdrop %u, raw %u, zone %u:%s, occ %d", stats.frames - sPrev.frames,
		stats.droppedBytes - sPrev.droppedBytes, stats.queueDrops - sPrev.queueDrops, sLastRawCount,
		static_cast<unsigned>(sTracker.count), sTracker.count ? people : " -",
		static_cast<int>(atomic_get(&sOccupied)));
	sPrev = stats;
#endif
}

#ifdef CONFIG_CHIP_ICD_UAT_SUPPORT
#define UAT_BUTTON_MASK DK_BTN3_MSK
#endif
/* BTN1 은 보드 공용 기능 버튼. BTN2 = 감지 멈춤 뒤집기(보드에서만. 저장 안 함 — 재부팅하면 감지) */
#define PAUSE_BUTTON_MASK DK_BTN2_MSK
/* BTN3 = 가상 온도 올리기, BTN4 = 내리기. BTN3 은 ICD UAT 가 켜지면 겹친다(지금은 ICD 끔). */
#ifdef CONFIG_CHIP_ICD_UAT_SUPPORT
#error "BTN3 을 ICD UAT 와 가상 온도 올리기가 같이 쓴다 — 버튼을 다시 나눌 것"
#endif
#define TEMP_UP_BUTTON_MASK DK_BTN3_MSK
#define TEMP_DOWN_BUTTON_MASK DK_BTN4_MSK

/* "28", "27.5", "-3.25" 를 0.01 C 단위로 바꾼다. 소수 셋째 자리부터는 버린다. */
bool ParseCentiDegrees(const char *text, int32_t &out)
{
	const char *p = text;
	bool negative = false;
	if (*p == '-' || *p == '+') {
		negative = (*p == '-');
		p++;
	}
	if (*p < '0' || *p > '9') {
		return false;
	}

	char *end;
	long whole = strtol(p, &end, 10);
	int32_t frac = 0;
	if (*end == '.') {
		end++;
		int digits = 0;
		while (*end >= '0' && *end <= '9') {
			if (digits < 2) {
				frac = frac * 10 + (*end - '0');
			}
			digits++;
			end++;
		}
		if (digits == 0) {
			return false;
		}
		if (digits == 1) {
			frac *= 10;
		}
	}
	if (*end != '\0' || whole > 1000) {
		return false;
	}

	int32_t value = static_cast<int32_t>(whole) * 100 + frac;
	out = negative ? -value : value;
	return true;
}

void PrintTemperature(const struct shell *sh, int16_t centi)
{
	int32_t absolute = centi < 0 ? -centi : centi;
	shell_print(sh, "temp %s%d.%02d C (virtual)", centi < 0 ? "-" : "", absolute / 100, absolute % 100);
}

int CmdTemp(const struct shell *sh, size_t argc, char **argv)
{
	AppTask &app = AppTask::Instance();

	if (argc == 1) {
		PrintTemperature(sh, app.GetCurrentTemperature());
		return 0;
	}

	if (strcmp(argv[1], "up") == 0) {
		app.StepTemperature(CONFIG_JAMJARI_TEMP_STEP_CENTI);
	} else if (strcmp(argv[1], "down") == 0) {
		app.StepTemperature(-CONFIG_JAMJARI_TEMP_STEP_CENTI);
	} else {
		int32_t centi;
		if (!ParseCentiDegrees(argv[1], centi)) {
			shell_error(sh, "usage: temp [<C> | up | down]   e.g. temp 28, temp 27.5");
			return -EINVAL;
		}
		if (centi < app.GetMinTemperature() || centi > app.GetMaxTemperature()) {
			shell_error(sh, "out of range (%d..%d, 0.01 C)", app.GetMinTemperature(), app.GetMaxTemperature());
			return -EINVAL;
		}
		app.SetTemperature(static_cast<int16_t>(centi));
	}

	shell_print(sh, "ok");
	return 0;
}

SHELL_CMD_ARG_REGISTER(temp, NULL, "Virtual temperature: temp [<C> | up | down]", CmdTemp, 1, 1);
} /* namespace */

void AppTask::SetSensing(bool on)
{
	/* 부팅 때(Init) 첫 호출은 바뀌지 않아도 한 줄 낸다 */
	static bool sLogged;
	bool paused = !on;
	bool was = atomic_set(&sPaused, paused ? 1 : 0) != 0;
	if (sLogged && was == paused) {
		return;
	}
	sLogged = true;
	LOG_INF("JJ1,PAUSE,%d,%u", paused ? 1 : 0, static_cast<unsigned>(k_uptime_get_32()));
	Nrf::PostTask([paused] { Nrf::GetBoard().GetLED(Nrf::DeviceLeds::LED3).Set(paused); });
	if (paused) {
		/* 레이더 스레드가 안 돌아도 1초 안에 비어 있음이 되게 바로 내린다(같은 값이면 아무것도 안 함) */
		ReportOccupancy(false);
	}
}

void AppTask::ButtonEventHandler(Nrf::ButtonState state, Nrf::ButtonMask hasChanged)
{
#ifdef CONFIG_CHIP_ICD_UAT_SUPPORT
	if ((UAT_BUTTON_MASK & state & hasChanged)) {
		LOG_INF("ICD UserActiveMode has been triggered.");
		Server::GetInstance().GetICDManager().OnNetworkActivity();
	}
#endif
	if (PAUSE_BUTTON_MASK & state & hasChanged) {
		SetSensing(atomic_get(&sPaused) != 0);
	}
	/* 가상 온도: 누를 때만 반응한다(뗄 때는 무시). */
	if (TEMP_UP_BUTTON_MASK & state & hasChanged) {
		Instance().StepTemperature(CONFIG_JAMJARI_TEMP_STEP_CENTI);
	}
	if (TEMP_DOWN_BUTTON_MASK & state & hasChanged) {
		Instance().StepTemperature(-CONFIG_JAMJARI_TEMP_STEP_CENTI);
	}
}

void AppTask::SetTemperature(int16_t centi)
{
	PlatformMgr().ScheduleWork([](intptr_t arg) { Instance().ApplyTemperature(static_cast<int32_t>(arg)); },
				   static_cast<intptr_t>(centi));
}

void AppTask::StepTemperature(int16_t deltaCenti)
{
	PlatformMgr().ScheduleWork(
		[](intptr_t arg) {
			AppTask &app = Instance();
			app.ApplyTemperature(static_cast<int32_t>(app.mCurrentTemperature) + static_cast<int32_t>(arg));
		},
		static_cast<intptr_t>(deltaCenti));
}

void AppTask::ApplyTemperature(int32_t centi)
{
	/* 클러스터가 정한 범위(ZAP 의 Min/MaxMeasuredValue) 안으로 자른다. */
	if (centi < mTemperatureSensorMinValue) {
		centi = mTemperatureSensorMinValue;
	} else if (centi > mTemperatureSensorMaxValue) {
		centi = mTemperatureSensorMaxValue;
	}
	mCurrentTemperature = static_cast<int16_t>(centi);

	Protocols::InteractionModel::Status status =
		Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(kTemperatureSensorEndpointId,
										  mCurrentTemperature);
	if (status != Protocols::InteractionModel::Status::Success) {
		LOG_ERR("Updating temperature measurement failed %x", to_underlying(status));
		return;
	}

	int32_t absolute = centi < 0 ? -centi : centi;
	LOG_INF("Virtual temperature %s%d.%02d C", centi < 0 ? "-" : "", absolute / 100, absolute % 100);
}

CHIP_ERROR AppTask::Init()
{
	/* Initialize Matter stack */
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer());

	if (!Nrf::GetBoard().Init(ButtonEventHandler)) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	/* Register Matter event handler that controls the connectivity status LED based on the captured Matter network
	 * state. */
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));

	ReturnErrorOnFailure(sIdentifyCluster.Init());
	ReturnErrorOnFailure(sLightIdentifyCluster.Init());
	ReturnErrorOnFailure(sTempIdentifyCluster.Init());
	ReturnErrorOnFailure(sOccupancyInstance.Init());
	SetSensing(true);

	ReturnErrorOnFailure(Nrf::Matter::StartServer());

	LOG_INF("Zone x[%d..%d] y[%d..%d] mm", kZone.xmin, kZone.xmax, kZone.ymin, kZone.ymax);
	if (BedRules::BedEnabled(kBedConfig.bed)) {
		LOG_INF("Bed x[%d..%d] y[%d..%d] split %d edge %d, follow %d ms", kBedConfig.bed.xmin,
			kBedConfig.bed.xmax, kBedConfig.bed.ymin, kBedConfig.bed.ymax, kBedConfig.bed.splitX,
			kBedConfig.bed.edgeMm, static_cast<int>(kBedConfig.followMs));
	} else {
		LOG_INF("Bed 미설정 → 예전 규칙 (구역 안 사람 있으면 occ 1)");
	}
#ifdef CONFIG_JAMJARI_DIRECT_UPLOAD
	/* 레이더보다 먼저: 사건이 들어오기 전에 보드 무작위 값·부팅번호를 정해 둔다 */
	DirectUpload::Init();
#endif
#ifdef CONFIG_JAMJARI_FUSION
	FusionReset();
	LOG_INF("fusion: 보드 추정 켜짐 (재생 빌드, 장면 시각)");
	if (Ld2450::Init(OnRadarFrame, OnRadarTick, OnReplayFrame) != 0) {
#else
	if (Ld2450::Init(OnRadarFrame, OnRadarTick) != 0) {
#endif
		/* 레이더가 없어도 Matter 기기로는 계속 동작한다 (Occupancy=0 유지) */
		LOG_ERR("LD2450 init failed");
	}

	return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	/* 가상 온도 범위를 ZAP 값에서 다시 읽는다. 못 읽어도 레이더는 계속 돌아야 하니 기본값(-10~40도)으로 간다. */
	DataModel::Nullable<int16_t> val;
	PlatformMgr().LockChipStack();
	if (Clusters::TemperatureMeasurement::Attributes::MinMeasuredValue::Get(kTemperatureSensorEndpointId, val) ==
		    Protocols::InteractionModel::Status::Success &&
	    !val.IsNull()) {
		mTemperatureSensorMinValue = val.Value();
	} else {
		LOG_ERR("Failed to get temperature measurement min value");
	}
	if (Clusters::TemperatureMeasurement::Attributes::MaxMeasuredValue::Get(kTemperatureSensorEndpointId, val) ==
		    Protocols::InteractionModel::Status::Success &&
	    !val.IsNull()) {
		mTemperatureSensorMaxValue = val.Value();
	} else {
		LOG_ERR("Failed to get temperature measurement max value");
	}
	PlatformMgr().UnlockChipStack();

	/* 시작 값. 이후로는 버튼·셸 명령으로만 바뀐다. */
	SetTemperature(CONFIG_JAMJARI_TEMP_START_CENTI);

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}
