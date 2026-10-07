/*
 * 보드가 맥 없이 웹 서버로 직접 올리기 (E 계획, 2단계③ ping · 3단계 사건 묶음).
 *
 * 길: OpenThread DNS(NAT64 주소로 바꿔 줌) → OpenThread TCP → mbedTLS(TLS1.2) → HTTP/1.1.
 * 10/4 1단계에서 SmartThings Station 이 NAT64 접두사 fd5f:6ac7:cfbe:2::/96 을 주는 것을 확인했다.
 *
 * 스레드·잠금 규칙 (교착 방지):
 *  - TCP 콜백은 OpenThread 스레드에서 OT 잠금을 쥔 채 불린다 → 콜백에선 플래그만 바꾸고 세마포어만 올린다.
 *  - 이 파일의 일꾼 스레드가 OT API 를 부를 때만 openthread_mutex_lock 을 짧게 잡는다.
 *    TLS 핸드셰이크·대기 중에는 잠금을 쥐고 있지 않는다.
 *
 * 인증서: 서명 체인은 검증한다(VERIFY_REQUIRED). 보드엔 시계가 없어 유효기간은 못 본다
 * (MBEDTLS_HAVE_TIME_DATE 꺼짐 → mbedTLS 가 기간 검사를 건너뜀).
 *
 * 사건 묶음 (3단계):
 *  - 리더 스레드가 QueueEvent 로 고리 버퍼(64개)에 넣는다. 가득 차면 가장 오래된 것을 버리고 센다.
 *  - 일꾼이 1분마다 또는 16개 쌓이면 최대 16개씩 /api/ingest/board 로 보낸다. 0개여도 1분마다 보낸다
 *    (서버가 「보드 연결됨」을 갱신하는 살아 있음 신호를 겸함).
 *  - HTTP 2xx 를 받아야 버퍼에서 지운다. 실패하면 30초 → 2분 → 5분(이후 5분) 뒤 다시.
 *    400(형식 오류)은 다시 보내도 같으니 그 묶음은 버리고 센다.
 *  - 보드엔 시계가 없다. 본문의 now_ms 는 보낼 때마다 새로 재고, 서버가 「받은 시각 − (now_ms − start_ms)」로 시각을 낸다.
 *  - src_id = <보드 무작위 8자리>-<부팅번호>-<seq>. 무작위 값은 처음 한 번 만들어 settings 에, 부팅번호는 부팅마다 +1.
 */

#include "direct_upload.h"

#include <openthread.h>
#include <openthread/dns_client.h>
#include <openthread/tcp.h>
#include <openthread/tcp_ext.h>

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

LOG_MODULE_REGISTER(upload, LOG_LEVEL_INF);

namespace
{
#include "root_certs.inc"

constexpr size_t kTokenMax = 96;
constexpr size_t kHostMax = 64;
constexpr k_timeout_t kDnsTimeout = K_SECONDS(15);
constexpr k_timeout_t kConnectTimeout = K_SECONDS(15);
constexpr k_timeout_t kIoTimeout = K_SECONDS(20);

char sToken[kTokenMax];
char sHost[kHostMax] = "jamjari.vercel.app";
char sBypass[48]; /* preview 배포 보호를 넘는 값(x-vercel-protection-bypass). 비어 있으면 헤더 안 붙임 */
uint32_t sRid; /* 보드 무작위 값 (settings jj/rid, 처음 한 번 만듦) */
uint32_t sBoot; /* 부팅번호 (settings jj/boot, 부팅마다 +1) */

/* ---- 사건 고리 버퍼 (리더 스레드 → 일꾼 스레드) ---- */
constexpr size_t kRingSize = 64;
constexpr size_t kBatchMax = 16;
constexpr int64_t kSendEveryMs = 60 * 1000;
constexpr int64_t kRetryMs[] = { 30 * 1000, 2 * 60 * 1000, 5 * 60 * 1000 };

struct k_spinlock sRingLock;
BedRules::Event sRing[kRingSize];
uint32_t sHead; /* 지금까지 넣은 개수 */
uint32_t sTail; /* 지금까지 지운(보냈거나 버린) 개수. sHead - sTail = 쌓인 개수 */
uint32_t sDropped; /* 가득 차서 버린 사건 */

/* 묶음 통계 (jj status) */
struct BatchStats {
	uint32_t posts; /* 보낸 묶음 수 (성공·실패 모두) */
	uint32_t fails;
	uint32_t rejected; /* 400 으로 버린 사건 */
	uint32_t sent; /* 서버가 받은 사건 (received 합) */
	uint32_t saved; /* 서버가 새로 저장한 사건 (saved 합) */
	int lastStatus;
	int64_t lastOkMs;
	uint8_t retryStep; /* 0 = 정상, 1~3 = kRetryMs 단계 */
} sBatch;

/* 레이더 누적 (1초마다 갱신, jj status 용) */
Ld2450::Stats sRadar = { 0, 0, 0, -1 };

/* ---- OpenThread TCP 상태 (콜백 ↔ 일꾼 스레드) ---- */
otTcpEndpoint sEndpoint;
otTcpCircularSendBuffer sSendBuf;
uint8_t sSendData[2048];
uint8_t sRecvBuf[OT_TCP_RECEIVE_BUFFER_SIZE_MANY_HOPS];

K_SEM_DEFINE(sTcpEvent, 0, 1);
volatile bool sEstablished;
volatile bool sClosed;

K_SEM_DEFINE(sDnsDone, 0, 1);
volatile uint32_t sDnsGen; /* 요청마다 +1. 시간 초과 뒤 늦게 온 응답이 다음 요청을 덮지 않게 */
volatile otError sDnsError;
otIp6Address sDnsAddr;

K_SEM_DEFINE(sJob, 0, 1); /* 깨우기: jj ping · jj send · 사건 16개 */
volatile bool sPingWanted;

/* 마지막 결과 (jj status) */
struct Result {
	int httpStatus;
	int tlsError;
	int64_t handshakeMs;
	int64_t totalMs;
	size_t stackFree;
	char step[16];
} sLast;

void Signal() { k_sem_give(&sTcpEvent); }

void OnEstablished(otTcpEndpoint *) { sEstablished = true; Signal(); }

void OnForwardProgress(otTcpEndpoint *, size_t inSendBuffer, size_t)
{
	otTcpCircularSendBufferHandleForwardProgress(&sSendBuf, inSendBuffer);
	Signal();
}

void OnReceiveAvailable(otTcpEndpoint *, size_t, bool endOfStream, size_t)
{
	if (endOfStream) {
		sClosed = true;
	}
	Signal();
}

void OnDisconnected(otTcpEndpoint *, otTcpDisconnectedReason) { sClosed = true; Signal(); }

void OnDnsResult(otError error, const otDnsAddressResponse *response, void *context)
{
	if (reinterpret_cast<uintptr_t>(context) != sDnsGen) {
		return; /* 이미 포기한 옛 요청 */
	}
	sDnsError = error;
	if (error == OT_ERROR_NONE) {
		sDnsError = otDnsAddressResponseGetAddress(response, 0, &sDnsAddr, nullptr);
	}
	k_sem_give(&sDnsDone);
}

/* ---- mbedTLS 입출력 (일꾼 스레드에서만 불림) ---- */
int TlsSend(void *, const unsigned char *buf, size_t len)
{
	for (;;) {
		if (sClosed) {
			return MBEDTLS_ERR_SSL_CONN_EOF;
		}
		size_t written = 0;
		openthread_mutex_lock();
		otError err = otTcpCircularSendBufferWrite(&sEndpoint, &sSendBuf, buf, len, &written, 0);
		openthread_mutex_unlock();
		if (err != OT_ERROR_NONE) {
			return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
		}
		if (written > 0) {
			return static_cast<int>(written);
		}
		/* 보낼 칸이 꽉 참 → 상대가 받았다는 소식(forward progress)을 기다린다 */
		if (k_sem_take(&sTcpEvent, kIoTimeout) != 0) {
			return MBEDTLS_ERR_SSL_TIMEOUT;
		}
	}
}

int TlsRecv(void *, unsigned char *buf, size_t len)
{
	for (;;) {
		size_t copied = 0;
		openthread_mutex_lock();
		const otLinkedBuffer *data = nullptr;
		if (otTcpReceiveByReference(&sEndpoint, &data) == OT_ERROR_NONE) {
			for (; data != nullptr && copied < len; data = data->mNext) {
				size_t n = data->mLength < len - copied ? data->mLength : len - copied;
				memcpy(buf + copied, data->mData, n);
				copied += n;
			}
			if (copied > 0) {
				otTcpCommitReceive(&sEndpoint, copied, 0);
			}
		}
		openthread_mutex_unlock();
		if (copied > 0) {
			return static_cast<int>(copied);
		}
		if (sClosed) {
			return 0; /* 상대가 닫음 */
		}
		if (k_sem_take(&sTcpEvent, kIoTimeout) != 0) {
			return MBEDTLS_ERR_SSL_TIMEOUT;
		}
	}
}

void SetStep(const char *step)
{
	snprintf(sLast.step, sizeof(sLast.step), "%s", step);
}

/* TCP 를 닫고 끝점을 정리한다 */
void TcpClose()
{
	openthread_mutex_lock();
	otTcpAbort(&sEndpoint);
	otTcpEndpointDeinitialize(&sEndpoint);
	openthread_mutex_unlock();
}

/* HTTPS 요청 한 번. 성공하면 HTTP 상태 코드, 실패하면 음수. 응답 본문 앞부분을 respBody 에 담는다. */
int HttpsPost(const char *path, const char *body, char *respBody, size_t respCap)
{
	respBody[0] = '\0';
	otInstance *ot = openthread_get_default_instance();
	int64_t t0 = k_uptime_get();
	memset(&sLast, 0, sizeof(sLast));

	/* 1) 서버 이름 → NAT64 IPv6 주소 */
	SetStep("dns");
	k_sem_reset(&sDnsDone);
	openthread_mutex_lock();
	otError err = otDnsClientResolveIp4Address(ot, sHost, OnDnsResult,
						   reinterpret_cast<void *>(static_cast<uintptr_t>(++sDnsGen)), nullptr);
	openthread_mutex_unlock();
	if (err != OT_ERROR_NONE || k_sem_take(&sDnsDone, kDnsTimeout) != 0 || sDnsError != OT_ERROR_NONE) {
		LOG_WRN("upload: DNS 실패 (%d/%d)", err, sDnsError);
		return -1;
	}

	/* 2) TCP 연결 */
	SetStep("tcp");
	sEstablished = false;
	sClosed = false;
	k_sem_reset(&sTcpEvent);
	otTcpEndpointInitializeArgs args = {};
	args.mEstablishedCallback = OnEstablished;
	args.mForwardProgressCallback = OnForwardProgress;
	args.mReceiveAvailableCallback = OnReceiveAvailable;
	args.mDisconnectedCallback = OnDisconnected;
	args.mReceiveBuffer = sRecvBuf;
	args.mReceiveBufferSize = sizeof(sRecvBuf);
	otSockAddr peer = {};
	peer.mAddress = sDnsAddr;
	peer.mPort = 443;

	openthread_mutex_lock();
	err = otTcpEndpointInitialize(ot, &sEndpoint, &args);
	if (err == OT_ERROR_NONE) {
		otTcpCircularSendBufferInitialize(&sSendBuf, sSendData, sizeof(sSendData));
		err = otTcpConnect(&sEndpoint, &peer, OT_TCP_CONNECT_NO_FAST_OPEN);
	}
	openthread_mutex_unlock();
	if (err != OT_ERROR_NONE) {
		LOG_WRN("upload: TCP 시작 실패 %d", err);
		TcpClose();
		return -2;
	}
	int64_t deadline = k_uptime_get() + 15000;
	while (!sEstablished && !sClosed && k_uptime_get() < deadline) {
		k_sem_take(&sTcpEvent, kConnectTimeout);
	}
	if (!sEstablished) {
		LOG_WRN("upload: TCP 연결 안 됨");
		TcpClose();
		return -3;
	}

	/* 3) TLS */
	SetStep("tls");
	int status = -4;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt ca;
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_x509_crt_init(&ca);

	int ret = 0;
	int loaded = 0;
	for (size_t i = 0; i < ARRAY_SIZE(kRootCerts); i++) {
		int r = mbedtls_x509_crt_parse_der(&ca, kRootCerts[i].der, kRootCerts[i].len);
		if (r == 0) {
			loaded++;
		} else {
			LOG_WRN("upload: 뿌리 인증서 %u 읽기 실패 -0x%04x", static_cast<unsigned>(i), -r);
		}
	}
	if (loaded == 0) {
		ret = MBEDTLS_ERR_X509_UNKNOWN_OID;
	}
	if (ret == 0) {
		ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
						  MBEDTLS_SSL_PRESET_DEFAULT);
	}
	if (ret == 0) {
		mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
		mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
		ret = mbedtls_ssl_setup(&ssl, &conf);
	}
	if (ret == 0) {
		ret = mbedtls_ssl_set_hostname(&ssl, sHost); /* SNI + 인증서 이름 확인 */
	}
	if (ret == 0) {
		mbedtls_ssl_set_bio(&ssl, nullptr, TlsSend, TlsRecv, nullptr);
		int64_t th = k_uptime_get();
		do {
			ret = mbedtls_ssl_handshake(&ssl);
		} while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
		sLast.handshakeMs = k_uptime_get() - th;
		if (ret != 0) {
			uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
			LOG_WRN("upload: TLS 핸드셰이크 실패 -0x%04x (인증서 검사 0x%x)", -ret, flags);
		}
	}

	/* 4) HTTP 요청·응답 */
	if (ret == 0) {
		SetStep("http");
		static char req[512];
		int n = snprintf(req, sizeof(req),
				 "POST %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n%s%s%s"
				 "Content-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
				 path, sHost, sToken, sBypass[0] ? "x-vercel-protection-bypass: " : "", sBypass,
				 sBypass[0] ? "\r\n" : "", static_cast<unsigned>(strlen(body)));
		if (n <= 0 || n >= static_cast<int>(sizeof(req))) {
			ret = -1;
		}
		/* 머리 다음 본문. 둘 다 끝까지 쓴다 */
		const char *parts[2] = { req, body };
		int lens[2] = { n, static_cast<int>(strlen(body)) };
		for (int p = 0; ret == 0 && p < 2; p++) {
			for (int off = 0; ret == 0 && off < lens[p];) {
				int w = mbedtls_ssl_write(
					&ssl, reinterpret_cast<const unsigned char *>(parts[p]) + off, lens[p] - off);
				if (w > 0) {
					off += w;
				} else if (w != MBEDTLS_ERR_SSL_WANT_WRITE && w != MBEDTLS_ERR_SSL_WANT_READ) {
					ret = w;
				}
			}
		}
		/* 상태 줄과 본문 앞부분만 본다. Vercel 응답 머리가 길어(약 0.6~1KB) 넉넉히 받는다 */
		static char resp[1536];
		size_t got = 0;
		while (ret == 0 && got < sizeof(resp) - 1) {
			int r = mbedtls_ssl_read(&ssl, reinterpret_cast<unsigned char *>(resp) + got,
						 sizeof(resp) - 1 - got);
			if (r > 0) {
				got += r;
			} else if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
				continue;
			} else {
				break; /* 0 = 닫힘, PEER_CLOSE_NOTIFY 포함 */
			}
		}
		resp[got] = '\0';
		int code = 0;
		if (sscanf(resp, "HTTP/1.%*d %d", &code) == 1) {
			status = code;
		}
		const char *bodyStart = strstr(resp, "\r\n\r\n");
		if (bodyStart) {
			snprintf(respBody, respCap, "%s", bodyStart + 4);
		}
		LOG_INF("upload: %s HTTP %d %s", path, code, respBody);
		mbedtls_ssl_close_notify(&ssl);
	}
	sLast.tlsError = ret;

	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_x509_crt_free(&ca);
	TcpClose();

	sLast.httpStatus = status;
	sLast.totalMs = k_uptime_get() - t0;
	SetStep("done");
	return status;
}

/* ---- 사건 묶음 ---- */
uint32_t Pending()
{
	k_spinlock_key_t key = k_spin_lock(&sRingLock);
	uint32_t n = sHead - sTail;
	k_spin_unlock(&sRingLock, key);
	return n;
}

/* 쌓인 사건 앞에서 최대 kBatchMax 개를 JSON 으로 만든다. *first = 첫 사건의 번호(sTail 기준). 반환: 담은 개수 */
size_t BuildBatch(char *buf, size_t cap, uint32_t *first)
{
	BedRules::Event evs[kBatchMax];
	k_spinlock_key_t key = k_spin_lock(&sRingLock);
	*first = sTail;
	uint32_t avail = sHead - sTail;
	size_t n = avail < kBatchMax ? avail : kBatchMax;
	for (size_t i = 0; i < n; i++) {
		evs[i] = sRing[(sTail + i) % kRingSize];
	}
	k_spin_unlock(&sRingLock, key);

	static const char *const kKinds[] = { "on", "off", "move", "away", "seat", "spot" };
	constexpr size_t kTail = 3; /* "]}" + NUL */
	/* now_ms 는 보낼 때마다 새로 잰다(재시도 때 옛 값을 쓰면 서버가 시각을 틀리게 낸다) */
	size_t len = snprintf(buf, cap, "{\"v\":1,\"now_ms\":%lld,\"events\":[", static_cast<long long>(k_uptime_get()));
	for (size_t i = 0; i < n; i++) {
		const BedRules::Event &ev = evs[i];
		int w = snprintf(buf + len, cap - len,
				"%s{\"src_id\":\"%08x-%u-%u\",\"kind\":\"%s\",\"start_ms\":%lld,\"end_ms\":%lld,"
				"\"size\":%u,\"x_mm\":%ld,\"y_mm\":%ld,\"count\":%u}",
				i ? "," : "", static_cast<unsigned>(sRid), static_cast<unsigned>(sBoot),
				static_cast<unsigned>(ev.seq), kKinds[static_cast<size_t>(ev.kind)],
				static_cast<long long>(ev.startMs), static_cast<long long>(ev.endMs),
				static_cast<unsigned>(ev.size), static_cast<long>(ev.x), static_cast<long>(ev.y),
				static_cast<unsigned>(ev.count));
		if (w < 0 || len + static_cast<size_t>(w) + kTail > cap) {
			n = i; /* 칸이 모자라면 이 사건부터는 다음 묶음으로 (잘린 JSON 을 보내지 않게) */
			break;
		}
		len += static_cast<size_t>(w);
	}
	snprintf(buf + len, cap - len, "]}");
	return n;
}

/* 보낸 묶음을 버퍼에서 지운다. 보내는 사이 가득 차서 이미 버려졌으면 그만큼은 건너뛴다.
 * delivered = 서버가 받음 → 보내는 사이 「버림」으로 센 것은 실제론 도착했으니 되돌린다 */
void Commit(uint32_t first, size_t n, bool delivered)
{
	k_spinlock_key_t key = k_spin_lock(&sRingLock);
	uint32_t end = first + static_cast<uint32_t>(n);
	int32_t passed = static_cast<int32_t>(sTail - first); /* 보내는 사이 버려진 개수 */
	if (delivered && passed > 0) {
		sDropped -= static_cast<uint32_t>(passed < static_cast<int32_t>(n) ? passed : static_cast<int32_t>(n));
	}
	if (static_cast<int32_t>(end - sTail) > 0) {
		sTail = end;
	}
	k_spin_unlock(&sRingLock, key);
}

int JsonInt(const char *s, const char *key)
{
	const char *p = strstr(s, key);
	return p ? atoi(p + strlen(key)) : -1;
}

/* 묶음 하나 보내기. 반환: true = 버퍼에서 지움(2xx 또는 400) */
bool SendBatch()
{
	/* 16개 × 사건 하나 최대 약 181자 + 머리 */
	static char body[kBatchMax * 200 + 64];
	static char resp[512]; /* chunked 면 앞에 덩어리 크기 줄이 붙는다 */
	uint32_t first = 0;
	size_t n = BuildBatch(body, sizeof(body), &first);
	int status = HttpsPost("/api/ingest/board", body, resp, sizeof(resp));
	sBatch.posts++;
	sBatch.lastStatus = status;
	if (status >= 200 && status < 300) {
		int received = JsonInt(resp, "\"received\":");
		int saved = JsonInt(resp, "\"saved\":");
		Commit(first, n, true);
		sBatch.sent += received > 0 ? received : 0;
		sBatch.saved += saved > 0 ? saved : 0;
		sBatch.lastOkMs = k_uptime_get();
		LOG_INF("upload: 사건 %u개 → received %d saved %d (남음 %u, 버림 %u)", static_cast<unsigned>(n),
			received, saved, static_cast<unsigned>(Pending()), static_cast<unsigned>(sDropped));
		return true;
	}
	sBatch.fails++;
	if (status == 400) {
		/* 형식 오류는 다시 보내도 같다 → 버리고 센다 */
		Commit(first, n, false);
		sBatch.rejected += n;
		LOG_WRN("upload: 사건 %u개 서버가 거절(400) — 버림", static_cast<unsigned>(n));
		return true;
	}
	LOG_WRN("upload: 사건 묶음 실패 %d (남음 %u)", status, static_cast<unsigned>(Pending()));
	return false;
}

/* ---- 일꾼 스레드: 1분마다(또는 16개 쌓이면) 사건 묶음, jj ping 이 오면 ping ---- */
volatile bool sSendNow;

void Worker(void *, void *, void *)
{
	int64_t nextSend = k_uptime_get() + kSendEveryMs;
	for (;;) {
		int64_t wait = nextSend - k_uptime_get();
		k_sem_take(&sJob, wait > 0 ? K_MSEC(wait) : K_NO_WAIT);

		if (sToken[0] == '\0') {
			if (sPingWanted || sSendNow) {
				LOG_WRN("upload: 토큰 없음 — jj token <값>");
			}
			sPingWanted = false;
			sSendNow = false;
			nextSend = k_uptime_get() + kSendEveryMs;
			continue;
		}

		if (sPingWanted) {
			sPingWanted = false;
			static char resp[160];
			int status = HttpsPost("/api/ingest/ping", "{\"v\":1,\"radar\":true,\"occ\":null}", resp,
					       sizeof(resp));
			size_t unused = 0;
			k_thread_stack_space_get(k_current_get(), &unused);
			sLast.stackFree = unused;
			LOG_INF("upload: 결과 %d · 핸드셰이크 %lld ms · 전체 %lld ms · 스택 여유 %u B", status,
				sLast.handshakeMs, sLast.totalMs, static_cast<unsigned>(unused));
		}

		int64_t now = k_uptime_get();
		/* 재시도 대기 중엔 16개가 차도 기다린다(서버·길이 막혔을 때 두드리지 않게) */
		bool full = Pending() >= kBatchMax && sBatch.retryStep == 0;
		if (!sSendNow && !full && now < nextSend) {
			continue;
		}
		sSendNow = false;

		if (SendBatch()) {
			sBatch.retryStep = 0;
			/* 아직 16개 이상 남았으면 바로 한 번 더 */
			nextSend = k_uptime_get() + (Pending() >= kBatchMax ? 0 : kSendEveryMs);
		} else {
			size_t step = sBatch.retryStep < ARRAY_SIZE(kRetryMs) ? sBatch.retryStep : ARRAY_SIZE(kRetryMs) - 1;
			nextSend = k_uptime_get() + kRetryMs[step];
			if (sBatch.retryStep < ARRAY_SIZE(kRetryMs)) {
				sBatch.retryStep++;
			}
			LOG_INF("upload: %lld초 뒤 다시", static_cast<long long>(kRetryMs[step] / 1000));
		}
		size_t unused = 0;
		k_thread_stack_space_get(k_current_get(), &unused);
		sLast.stackFree = unused;
	}
}

K_THREAD_DEFINE(sUploadThread, 12288, Worker, nullptr, nullptr, nullptr, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
		0);

/* ---- settings: jj/token, jj/host, jj/bypass (글자), jj/rid, jj/boot (숫자) ---- */
int SettingsSet(const char *name, size_t len, settings_read_cb read, void *arg)
{
	if (strcmp(name, "rid") == 0 || strcmp(name, "boot") == 0) {
		uint32_t *dst = name[0] == 'r' ? &sRid : &sBoot;
		if (len != sizeof(*dst)) {
			return -EINVAL;
		}
		ssize_t n = read(arg, dst, sizeof(*dst));
		return n < 0 ? static_cast<int>(n) : 0;
	}
	char *dst = nullptr;
	size_t cap = 0;
	if (strcmp(name, "token") == 0) {
		dst = sToken;
		cap = sizeof(sToken);
	} else if (strcmp(name, "host") == 0) {
		dst = sHost;
		cap = sizeof(sHost);
	} else if (strcmp(name, "bypass") == 0) {
		dst = sBypass;
		cap = sizeof(sBypass);
	} else {
		return -ENOENT;
	}
	if (len >= cap) {
		return -EINVAL;
	}
	ssize_t n = read(arg, dst, len);
	if (n < 0) {
		return static_cast<int>(n);
	}
	dst[n] = '\0';
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(jj, "jj", nullptr, SettingsSet, nullptr, nullptr);

/* ---- 셸 ---- */
int CmdToken(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2 || strlen(argv[1]) >= sizeof(sToken)) {
		shell_error(sh, "사용법: jj token <토큰>");
		return -EINVAL;
	}
	snprintf(sToken, sizeof(sToken), "%s", argv[1]);
	int err = settings_save_one("jj/token", sToken, strlen(sToken));
	shell_print(sh, "토큰 저장 %s (%u자)", err ? "실패" : "끝", static_cast<unsigned>(strlen(sToken)));
	return err;
}

int CmdHost(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2 || strlen(argv[1]) >= sizeof(sHost)) {
		shell_error(sh, "사용법: jj host <서버이름>   예: jamjari.vercel.app");
		return -EINVAL;
	}
	snprintf(sHost, sizeof(sHost), "%s", argv[1]);
	int err = settings_save_one("jj/host", sHost, strlen(sHost));
	shell_print(sh, "서버 %s 저장 %s", sHost, err ? "실패" : "끝");
	return err;
}

int CmdBypass(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2 || strlen(argv[1]) >= sizeof(sBypass)) {
		shell_error(sh, "사용법: jj bypass <값>   (지우기: jj bypass -)");
		return -EINVAL;
	}
	snprintf(sBypass, sizeof(sBypass), "%s", strcmp(argv[1], "-") == 0 ? "" : argv[1]);
	int err = settings_save_one("jj/bypass", sBypass, strlen(sBypass));
	shell_print(sh, "배포 보호 값 %s %s", sBypass[0] ? "저장" : "지움", err ? "실패" : "끝");
	return err;
}

int CmdStatus(const struct shell *sh, size_t, char **)
{
	shell_print(sh, "서버 %s · 토큰 %s · 보호값 %s · 보드 %08x 부팅 %u", sHost, sToken[0] ? "있음" : "없음",
		    sBypass[0] ? "있음" : "없음", static_cast<unsigned>(sRid), static_cast<unsigned>(sBoot));
	shell_print(sh, "마지막: 단계 %s · HTTP %d · TLS %d · 핸드셰이크 %lld ms · 전체 %lld ms · 스택 여유 %u B",
		    sLast.step[0] ? sLast.step : "-", sLast.httpStatus, sLast.tlsError, sLast.handshakeMs,
		    sLast.totalMs, static_cast<unsigned>(sLast.stackFree));
	k_spinlock_key_t key = k_spin_lock(&sRingLock);
	uint32_t made = sHead;
	uint32_t pending = sHead - sTail;
	uint32_t dropped = sDropped;
	Ld2450::Stats radar = sRadar;
	k_spin_unlock(&sRingLock, key);
	int64_t okAgo = sBatch.lastOkMs ? (k_uptime_get() - sBatch.lastOkMs) / 1000 : -1;
	shell_print(sh, "사건: 생김 %u · 남음 %u · 버림 %u · 거절 %u · 서버 received %u saved %u",
		    static_cast<unsigned>(made), static_cast<unsigned>(pending), static_cast<unsigned>(dropped),
		    static_cast<unsigned>(sBatch.rejected), static_cast<unsigned>(sBatch.sent),
		    static_cast<unsigned>(sBatch.saved));
	shell_print(sh, "묶음: 보냄 %u · 실패 %u · 마지막 HTTP %d · 마지막 성공 %lld초 전 · 재시도 단계 %u",
		    static_cast<unsigned>(sBatch.posts), static_cast<unsigned>(sBatch.fails), sBatch.lastStatus,
		    static_cast<long long>(okAgo), static_cast<unsigned>(sBatch.retryStep));
	/* 재생 스크립트가 보낸 프레임 수와 「받음」이 같고 버림이 0 이면 끊김 없음 */
	shell_print(sh, "레이더 프레임: 받음 %u · 깨진 바이트 %u · 밀려 버림 %u", static_cast<unsigned>(radar.frames),
		    static_cast<unsigned>(radar.droppedBytes), static_cast<unsigned>(radar.queueDrops));
	return 0;
}

int CmdPing(const struct shell *sh, size_t, char **)
{
	sPingWanted = true;
	k_sem_give(&sJob);
	shell_print(sh, "ping 보내는 중 — 결과는 로그(upload:)로");
	return 0;
}

int CmdSend(const struct shell *sh, size_t, char **)
{
	sSendNow = true;
	k_sem_give(&sJob);
	shell_print(sh, "사건 묶음 지금 보내는 중 (남음 %u) — 결과는 로그(upload:)로", static_cast<unsigned>(Pending()));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sJjCmds, SHELL_CMD_ARG(token, NULL, "토큰 저장", CmdToken, 2, 0),
			       SHELL_CMD_ARG(host, NULL, "서버 이름 저장", CmdHost, 2, 0),
			       SHELL_CMD_ARG(bypass, NULL, "preview 배포 보호 값 저장(- 는 지움)", CmdBypass, 2, 0),
			       SHELL_CMD(status, NULL, "설정·마지막 결과·사건·프레임", CmdStatus),
			       SHELL_CMD(ping, NULL, "/api/ingest/ping 한 번", CmdPing),
			       SHELL_CMD(send, NULL, "사건 묶음 지금 보내기", CmdSend), SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(jj, &sJjCmds, "잠자리 직접 전송", NULL);
} // namespace

namespace DirectUpload
{
void Init()
{
	settings_load_subtree("jj");
	if (sRid == 0) {
		while (sRid == 0) {
			sys_csrand_get(&sRid, sizeof(sRid));
		}
		settings_save_one("jj/rid", &sRid, sizeof(sRid));
	}
	sBoot++;
	settings_save_one("jj/boot", &sBoot, sizeof(sBoot));
	LOG_INF("upload: 직접 전송 준비 (서버 %s, 토큰 %s, 보드 %08x 부팅 %u)", sHost, sToken[0] ? "있음" : "없음",
		static_cast<unsigned>(sRid), static_cast<unsigned>(sBoot));
}

void QueueEvent(const BedRules::Event &ev)
{
	bool wake = false;
	k_spinlock_key_t key = k_spin_lock(&sRingLock);
	if (sHead - sTail >= kRingSize) {
		sTail++; /* 가장 오래된 것 버림 */
		sDropped++;
	}
	sRing[sHead % kRingSize] = ev;
	sHead++;
	wake = sHead - sTail >= kBatchMax;
	k_spin_unlock(&sRingLock, key);
	if (wake) {
		k_sem_give(&sJob);
	}
}

void NoteRadarStats(const Ld2450::Stats &stats)
{
	k_spinlock_key_t key = k_spin_lock(&sRingLock);
	sRadar = stats;
	k_spin_unlock(&sRingLock, key);
}
} // namespace DirectUpload
