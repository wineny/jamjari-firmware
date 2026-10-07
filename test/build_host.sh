#!/usr/bin/env bash
# 호스트(맥)에서 순수 C++ 파서·트래커·occupancy 로직만 컴파일한다. Zephyr 의존 없음, 외부 라이브러리 없음.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/../src"

clang++ -std=c++17 -O2 -Wall -Wextra -Wshadow \
	-I "$SRC" \
	"$DIR/run_cpp_parser.cpp" "$SRC/ld2450_parser.cpp" \
	-o "$DIR/run_cpp_parser"

clang++ -std=c++17 -O2 -Wall -Wextra -Wshadow \
	-I "$SRC" \
	"$DIR/run_tracker.cpp" "$SRC/ld2450_parser.cpp" "$SRC/tracker.cpp" "$SRC/occupancy.cpp" \
	-o "$DIR/run_tracker"

clang++ -std=c++17 -O2 -Wall -Wextra -Wshadow \
	-I "$SRC" \
	"$DIR/run_bed_rules.cpp" "$SRC/tracker.cpp" "$SRC/bed_rules.cpp" \
	-o "$DIR/run_bed_rules"

echo "빌드 완료: $DIR/run_cpp_parser, $DIR/run_tracker, $DIR/run_bed_rules"
