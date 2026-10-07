#!/usr/bin/env python3
"""west zap-generate 뒤에 한 번 돌린다.

0x0431 Ambient Context Sensing 은 SDK(v3.4.1) 생성 코드에 없어서
callback-stub.cpp 의 `app::Clusters::AmbientContextSensing::Id` 가 컴파일되지 않는다.
숫자 0x00000431 로 바꾸고, SDK callback.h 에 없는 init 함수 선언을 넣는다.
여러 번 돌려도 같다.
"""
import pathlib
import sys

p = pathlib.Path(__file__).parent / "zap-generated" / "callback-stub.cpp"
s = p.read_text()
s2 = s.replace("app::Clusters::AmbientContextSensing::Id", "0x00000431 /* AmbientContextSensing */")
DECL = "void emberAfAmbientContextSensingClusterInitCallback(chip::EndpointId endpoint);\n"
if DECL not in s2:
    s2 = s2.replace("using namespace chip;\n", "using namespace chip;\n\n// 0x0431 (patch_0431.py)\n" + DECL, 1)
if "AmbientContextSensing::Id" in s2 or DECL not in s2:
    sys.exit("patch_0431: 생성 형식이 바뀌어 패치를 못 넣었다 — callback-stub.cpp 를 직접 확인할 것")
p.write_text(s2)
print("patched" if s2 != s else "already patched")
