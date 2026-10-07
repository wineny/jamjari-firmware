/*
 * HLK-LD2450 레이더 UART 리더.
 *
 * 프레임 (30바이트, ld2450_점검.py 와 같음)
 *   헤더 AA FF 03 00 | 타깃 3개 × 8바이트 | 푸터 55 CC
 *   타깃 8바이트 = X(2) Y(2) Speed(2) Resolution(2), 리틀엔디안
 *   X/Y/Speed 부호: 상위 바이트 최상위 비트가 1 이면 양수, 0 이면 음수. 값은 하위 15비트.
 *   Speed 는 ×10 해서 mm/s. 미검출 슬롯은 8바이트 전부 0.
 */

#include "ld2450.h"
#include "ld2450_parser.h"
#include "replay_frames.h"

#include <hal/nrf_uarte.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

LOG_MODULE_REGISTER(ld2450, CONFIG_CHIP_APP_LOG_LEVEL);

namespace Ld2450 {
namespace {

#define LD2450_UART_NODE DT_ALIAS(ld2450_uart)

/*
 * nRF UARTE 는 16MHz 를 정수로 나눠 보드레이트를 만든다. Zephyr 드라이버는 표준값 표에
 * 없는 256000 을 거부하므로(250000 까지만 허용) 레지스터에 직접 쓴다.
 * 값은 드라이버의 UARTE_GET_CUSTOM_BAUDRATE 공식과 같다:
 *   (2^20 / round(16e6 / 256000)) << 12 = (1048576 / 63) << 12 = 0x04104000
 *   실제 속도 16e6 / 63 = 253968bps (오차 -0.8%)
 */
constexpr uint32_t kBaudrate256000Reg = (BIT(20) / 63) << 12;
static_assert(kBaudrate256000Reg == 0x04104000, "LD2450 baudrate register");

#ifdef CONFIG_JAMJARI_RADAR_REPLAY
/* 재생: 맥이 몇 배속으로 넣을 수 있어 큐를 늘리고, 프레임 칸은 가장 긴 재생 프레임(34바이트) */
constexpr size_t kQueueDepth = 32;
constexpr size_t kMsgLen = ReplayFrames::kMaxLen;
#else
constexpr size_t kQueueDepth = 8;
constexpr size_t kMsgLen = kFrameLen;
#endif
K_MSGQ_DEFINE(sFrameQueue, kMsgLen, kQueueDepth, 1);

/* 프레임 핸들러가 침대 칸 규칙(bed_rules: 사건 버퍼·칸별 좌표 표)까지 돌아서 2048 → 3072, 재생 빌드는 보드 추정까지 돌아서 4096 */
#ifdef CONFIG_JAMJARI_RADAR_REPLAY
constexpr size_t kStackSize = 4096;
#else
constexpr size_t kStackSize = 3072;
#endif
K_THREAD_STACK_DEFINE(sStack, kStackSize);
k_thread sThread;

const device *const sUart = DEVICE_DT_GET(LD2450_UART_NODE);
FrameHandler sHandler;
TickHandler sTick;
ReplayHandler sReplay;

constexpr int64_t kTickMs = 1000;
atomic_t sDroppedBytes = ATOMIC_INIT(0); /* ISR 가 sSync.dropped 를 옮겨 둔다 */
atomic_t sQueueDrops = ATOMIC_INIT(0);

/* ISR 에서만 쓰는 프레임 조립 상태 (실제 동기화·파싱 로직은 ld2450_parser.cpp / replay_frames.cpp, 호스트 테스트와 공유) */
#ifdef CONFIG_JAMJARI_RADAR_REPLAY
ReplayFrames::SyncState sSync;
atomic_t sCtlDrops = ATOMIC_INIT(0); /* 버린 60GHz·장면 끝·시작 프레임 — 보드 추정이 파이썬과 달라진다 */

void EmitReplay(const uint8_t *frame, void *)
{
	/* 큐가 차 있으면 이번 프레임은 버린다 */
	if (k_msgq_put(&sFrameQueue, frame, K_NO_WAIT) != 0) {
		atomic_inc(&sQueueDrops);
		if (frame[2] != 0x03 && frame[2] != 0x07) {
			atomic_inc(&sCtlDrops);
		}
	}
}
#else
FrameSyncState sSync;
#endif

void UartIsr(const device *dev, void *)
{
	if (!uart_irq_update(dev)) {
		return;
	}
	while (uart_irq_rx_ready(dev)) {
		uint8_t buf[32];
		int len = uart_fifo_read(dev, buf, sizeof(buf));
		if (len <= 0) {
			break;
		}
		for (int i = 0; i < len; i++) {
#ifdef CONFIG_JAMJARI_RADAR_REPLAY
			ReplayFrames::Feed(sSync, buf[i], EmitReplay, nullptr);
#else
			if (FeedByte(sSync, buf[i])) {
				/* 큐가 차 있으면 이번 프레임은 버린다 (다음 프레임이 곧 온다) */
				if (k_msgq_put(&sFrameQueue, sSync.frame, K_NO_WAIT) != 0) {
					atomic_inc(&sQueueDrops);
				}
			}
#endif
		}
	}
	atomic_set(&sDroppedBytes, static_cast<atomic_val_t>(sSync.dropped));
}

void ReaderThread(void *, void *, void *)
{
	uint8_t frame[kMsgLen];
	Target targets[kTargetSlots];
	Stats stats = { 0, 0, 0, -1 };
	int64_t nextTick = k_uptime_get() + kTickMs;

	while (true) {
		int64_t now = k_uptime_get();
		k_timeout_t wait = nextTick > now ? K_MSEC(nextTick - now) : K_NO_WAIT;

		if (k_msgq_get(&sFrameQueue, frame, wait) == 0) {
			stats.frames++;
			stats.lastFrameMs = k_uptime_get();
#ifdef CONFIG_JAMJARI_RADAR_REPLAY
			/* 재생 프레임 종류: 03 옛 레이더 · 07 레이더+장면 시각 → 기존 판단, 06·07·08·09 → 보드 추정 */
			uint8_t type = frame[2];
			if (type == 0x03 || type == 0x07) {
				uint8_t live = ParseFrame(type == 0x07 ? frame + 4 : frame, targets); /* 07 은 타깃이 4칸 뒤 */
				if (sHandler) {
					sHandler(targets, live);
				}
			}
			if (type != 0x03 && sReplay) {
				sReplay(frame);
			}
#else
			uint8_t live = ParseFrame(frame, targets);
			if (sHandler) {
				sHandler(targets, live);
			}
#endif
		}

		now = k_uptime_get();
		if (now >= nextTick) {
			nextTick = now + kTickMs;
			stats.droppedBytes = static_cast<uint32_t>(atomic_get(&sDroppedBytes));
			stats.queueDrops = static_cast<uint32_t>(atomic_get(&sQueueDrops));
#ifdef CONFIG_JAMJARI_RADAR_REPLAY
			static atomic_val_t sCtlSeen;
			atomic_val_t ctl = atomic_get(&sCtlDrops);
			if (ctl != sCtlSeen) {
				LOG_WRN("재생: 60GHz·장면 끝 프레임 %d개 버림(큐 참) — 배속을 낮출 것", static_cast<int>(ctl - sCtlSeen));
				sCtlSeen = ctl;
			}
#endif
			if (sTick) {
				sTick(stats, now);
			}
		}
	}
}

int SetBaudrate256000()
{
	NRF_UARTE_Type *uarte = reinterpret_cast<NRF_UARTE_Type *>(DT_REG_ADDR(LD2450_UART_NODE));

	/* 꺼 둔 상태에서 레지스터를 바꾸고 다시 켠다. nRF5340 은 끄고 켜도 BAUDRATE 가 유지된다. */
	int err = pm_device_action_run(sUart, PM_DEVICE_ACTION_SUSPEND);
	if (err && err != -EALREADY) {
		return err;
	}
	nrf_uarte_baudrate_set(uarte, static_cast<nrf_uarte_baudrate_t>(kBaudrate256000Reg));
	err = pm_device_action_run(sUart, PM_DEVICE_ACTION_RESUME);
	if (err && err != -EALREADY) {
		return err;
	}
	return 0;
}

} /* namespace */

int Init(FrameHandler handler, TickHandler tick, ReplayHandler replay)
{
	if (!device_is_ready(sUart)) {
		LOG_ERR("LD2450 UART not ready");
		return -ENODEV;
	}

	int err = 0;
	if (!IS_ENABLED(CONFIG_JAMJARI_RADAR_REPLAY)) {
		/* 시연용 재생은 맥이 DT 의 표준 속도(115200)로 보내므로 레이더 속도로 바꾸지 않는다. */
		err = SetBaudrate256000();
		if (err) {
			LOG_ERR("LD2450 baudrate set failed: %d", err);
			return err;
		}
	}

	sHandler = handler;
	sTick = tick;
	sReplay = replay;
	k_thread_create(&sThread, sStack, K_THREAD_STACK_SIZEOF(sStack), ReaderThread, nullptr, nullptr,
			nullptr, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&sThread, "ld2450");

	err = uart_irq_callback_user_data_set(sUart, UartIsr, nullptr);
	if (err) {
		LOG_ERR("LD2450 UART irq callback failed: %d", err);
		return err;
	}
	uart_irq_rx_enable(sUart);

	if (IS_ENABLED(CONFIG_JAMJARI_RADAR_REPLAY)) {
		LOG_INF("LD2450 reader started — 시연 재생 모드 (맥 VCOM0 에서 더미 프레임)");
	} else {
		LOG_INF("LD2450 reader started (256000bps 8N1)");
	}
	return 0;
}

} /* namespace Ld2450 */
