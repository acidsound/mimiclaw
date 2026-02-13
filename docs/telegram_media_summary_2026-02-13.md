# Telegram Media & Agent Stability Summary

작성일: 2026-02-13
범위: Telegram 수신 확장, 이미지/음성 처리, STT, 툴 루프 안정성 보강, 텔레그램 출력 포맷 보정

## 핵심 기능 변경
- Telegram 메시지 파서(`main/telegram/telegram_bot.c`)가 텍스트/포토/보이스를 구분 처리하도록 확장됨.
- 음성 파일은 `telegram_get_file_info` → `telegram_stream_file` 기반 스트리밍 파이프라인으로 STT 전송.
- LLM Vision 입력은 사용자 메시지에 image 블록을 구성하도록 `agent_loop`에서 base64 인코딩 후 주입.
- STT는 OpenAI 호환 인터페이스 기반(현재 Groq 프로필로 테스트)로 분리되어 독립 설정 가능.
- `tool_web_search`의 `0건 결과`를 `"No web results found for \"<query>\"` 문구로 정규화.
- 웹 검색 0건 케이스에서 ReAct 루프를 조기 종료해 불필요한 tool iteration을 방지.
- Telegram 마크다운->HTML 변환에서 `[web_search]` 같은 대괄호/언더스코어 토큰이 이탤릭으로 깨지지 않도록 예외 처리.

## 안정성/버그 수정
- `agent_loop` 스택 오버플로우 원인 분석 결과: 반복 루프 내 대형 로컬 배열(`web_no_result_output`)이 스택 사용량을 과도하게 유발.
- 해당 버퍼를 스택에서 PSRAM 힙 할당으로 이동(`TOOL_OUTPUT_SIZE`, 8KB)해 해결.
- 음성 텍스트 결합 버퍼 길이 계산을 정확히 수정해 문자열 경계 오버런 가능성 제거.
- 기존 `main/CMakeLists.txt`/헤더/CLI 경로에 STT/미디어 확장 모듈 등록 반영.

## 테스트 및 관찰 포인트
- 텔레그램 보이스 메모 길이/용량 제한 경고 메시지 동작.
- 사진 첨부 quick mode 권장 문구와 크기 제한 정책 적용.
- STT 성공/실패 로그, LLM error fallback, tool iteration max 경고 동작 검증.
- 모니터에서 `stack overflow` 재발 여부 확인(특히 연속 질의/툴 호출 시나리오).

## 남은 검토 항목
- 실제 장비에서 build/flash 후 안정성 재검증(필수).
- STT provider별(content-type, 확장자) 동작 호환성은 장비 로그 기반으로 계속 확인.
- Telegram 마크다운 렌더링 변환 규칙이 다른 특수 패턴에 미치는 영향은 추가 회귀 테스트 필요.
