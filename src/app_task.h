/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "board/board.h"

#include <platform/CHIPDeviceLayer.h>

struct Identify;

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();

	/* 「감지 잠시 멈춤」 스위치(엔드포인트 2 On/Off). on=false 면 멈춤. Matter(CHIP) 스레드에서만 부른다. */
	static void SetSensing(bool on);

	/* 시연용 가상 온도(엔드포인트 3). 값은 모두 클러스터 단위(0.01 C). Matter 스레드 밖에서 불러도 된다(작업을 넘긴다). */
	void SetTemperature(int16_t centi);
	void StepTemperature(int16_t deltaCenti);

	int16_t GetCurrentTemperature() const { return mCurrentTemperature; }
	int16_t GetMinTemperature() const { return mTemperatureSensorMinValue; }
	int16_t GetMaxTemperature() const { return mTemperatureSensorMaxValue; }

private:
	CHIP_ERROR Init();

	/* Matter 스레드에서만 부른다. */
	void ApplyTemperature(int32_t centi);

	static void ButtonEventHandler(Nrf::ButtonState state, Nrf::ButtonMask hasChanged);

	/* ZAP 의 Min/MaxMeasuredValue 와 같은 값. StartApp 에서 다시 읽기 전(서버가 막 뜬 사이)에
	 * 버튼·셸이 들어와도 0..0 으로 잘리지 않게 미리 채워 둔다. */
	int16_t mTemperatureSensorMaxValue = 4000;
	int16_t mTemperatureSensorMinValue = -1000;
	int16_t mCurrentTemperature = 0;
};
