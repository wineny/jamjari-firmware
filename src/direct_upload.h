/*
 * 보드가 맥 없이 웹 서버로 직접 올리기 (E 계획).
 * Thread → SmartThings Station 의 NAT64 → 인터넷 → https://<host> 로 HTTPS 요청을 보낸다.
 * 토큰·서버 이름은 셸(jj token / jj host)로 settings 에 넣는다. 소스에 넣지 않는다.
 */

#pragma once

#include "bed_rules.h"
#include "ld2450.h"

namespace DirectUpload
{
/* 일꾼 스레드와 셸 명령 준비. settings 에서 토큰·서버 이름을 읽고 부팅번호를 1 올린다. */
void Init();

/* 사건 하나를 보낼 줄(고리 버퍼 64개)에 넣는다. LD2450 리더 스레드에서 불린다. 막히지 않는다. */
void QueueEvent(const BedRules::Event &ev);

/* 1초마다 레이더 누적 수치를 넘긴다(jj status 의 프레임 끊김 확인용). */
void NoteRadarStats(const Ld2450::Stats &stats);
} // namespace DirectUpload
