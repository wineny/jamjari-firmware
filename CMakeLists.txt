#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

project(matter-jamjari-occupancy)

# Enable GNU STD support.
include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)

# NORDIC SDK APP START

# Include all source files that are located in the Matter common directory.
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/source_common.cmake)

# Include Data Model utils
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/data_model.cmake)

include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/zap_helpers.cmake)
ncs_get_zap_parent_dir(ZAP_PARENT_DIR)

target_include_directories(app PRIVATE
  src
  ${ZAP_PARENT_DIR}
)

target_sources(app PRIVATE
  src/app_task.cpp
  src/bed_rules.cpp
  src/main.cpp
  src/ld2450.cpp
  src/ld2450_parser.cpp
  src/tracker.cpp
  src/zcl_callbacks.cpp
)
target_sources_ifdef(CONFIG_JAMJARI_DIRECT_UPLOAD app PRIVATE src/direct_upload.cpp)
# 재생 빌드: 재생 프레임(장면 시각·60GHz) 동기화 + 보드 추정
target_sources_ifdef(CONFIG_JAMJARI_RADAR_REPLAY app PRIVATE src/replay_frames.cpp)
target_sources_ifdef(CONFIG_JAMJARI_FUSION app PRIVATE src/fusion.cpp)

# 0x0431 은 SDK 에 서버 코드가 없다(초안 클러스터) — 속성 저장소만 쓴다
ncs_configure_data_model(EXTERNAL_CLUSTERS AMBIENT_CONTEXT_SENSING_CLUSTER)
# NORDIC SDK APP END
