/*
 * ZCL 콜백. (예전 ep2 「감지 멈춤」 On/Off 는 뺐다 — SmartThings 가 기기를 플러그로 잡아 동작 감지를 가렸다.)
 */

/* 0x0431 Ambient Context Sensing: SDK 에 서버 코드가 없어 시작 함수도 없다. 값은 ZAP 속성 저장소에만 둔다. */
void MatterAmbientContextSensingPluginServerInitCallback() {}
