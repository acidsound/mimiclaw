# Image Relay Spec (ESP32-S3 + Telegram)

## 1. Goal
- ESP32가 이미지 `b64_json` payload를 직접 처리하지 않고도 Telegram 이미지 응답을 제공한다.
- 이미지 생성/업로드/임시저장을 Relay 서버로 오프로딩한다.

## 2. Background
- 대상 디바이스: ESP32-S3, 가용 RAM 약 200KB 수준.
- 문제: 이미지 생성 모델 응답(`b64_json`, `inlineData`)은 JSON 파싱 + base64 디코드 메모리 비용이 커서 온디바이스 처리 리스크가 높다.
- 결론: 디바이스는 텍스트 제어 plane만 담당하고, 데이터 plane(이미지 바이트)은 Relay가 담당.

## 3. Scope
- 포함:
  - Relay API 정의
  - Telegram `sendPhoto` 업로드 플로우
  - `file_id` 중심 재사용 정책
  - 삭제/보안/timeout 정책
- 제외:
  - ESP32 내 이미지 디코딩/리사이즈
  - Relay의 복잡한 큐 시스템(초기 버전)

## 4. High-level Architecture
- `ESP32`:
  - Telegram 메시지 수신
  - Relay에 이미지 생성 요청
  - 성공/실패 텍스트 상태 응답
- `Relay (Vercel / Hugging Face)`:
  - 이미지 모델 API 호출
  - Telegram Bot API `sendPhoto` 호출
  - `file_id`, `message_id`를 ESP32에 반환
- `Model Provider`:
  - OpenAI/Gemini 등 이미지 생성 응답 제공 (base64/bytes)
- `Telegram Bot API`:
  - 업로드된 이미지 저장, `file_id` 반환

## 5. API Contract (Relay)

### 5.1 Request
- Endpoint: `POST /v1/image/send`
- Headers:
  - `Content-Type: application/json`
  - `X-Mimi-Signature: <hmac_sha256>`
  - `X-Mimi-Timestamp: <unix_seconds>`
- Body:
```json
{
  "request_id": "uuid-v4",
  "chat_id": "123456789",
  "prompt": "cinematic cat astronaut",
  "size": "1024x1024",
  "provider": "openai",
  "model": "gpt-image-1"
}
```

### 5.2 Response (success)
```json
{
  "ok": true,
  "request_id": "uuid-v4",
  "telegram": {
    "message_id": 321,
    "file_id": "AgACAgUAAxkBA..."
  },
  "usage": {
    "provider_latency_ms": 4200,
    "telegram_latency_ms": 900
  }
}
```

### 5.3 Response (error)
```json
{
  "ok": false,
  "request_id": "uuid-v4",
  "error_code": "UPSTREAM_TIMEOUT",
  "error_message": "image generation timeout"
}
```

## 6. End-to-End Flow
1. 사용자: Telegram에서 이미지 생성 의도 입력.
2. ESP32: 정책 검사(허용 chat, rate limit) 후 Relay 호출.
3. Relay: 모델 API 호출, 이미지 bytes 획득.
4. Relay: Telegram `sendPhoto` multipart 업로드.
5. Telegram: `file_id` 반환.
6. Relay: ESP32에 성공 JSON 반환.
7. ESP32: 필요 시 `file_id`를 로컬 메타데이터로 저장.

## 7. Data Retention Policy
- Relay 임시 이미지 저장은 업로드 완료 직후 삭제.
- 실패 시에도 즉시 삭제, 최대 보관 TTL `60s`.
- 영구 보관 대상:
  - `request_id`
  - `chat_id` (또는 해시)
  - `file_id`
  - 상태코드/지연시간
- 원본 base64/이미지 파일은 영구 저장 금지(기본 정책).

## 8. Security
- 요청 인증:
  - HMAC(shared secret) + timestamp window(예: ±60s)
- 재전송 방지:
  - `request_id` 기반 idempotency (동일 ID 중복 처리 방지)
- 최소 권한:
  - Relay가 아는 비밀값: 모델 키, Telegram bot token
  - ESP32에는 Relay 호출용 공유 시크릿만 보관
- 로깅:
  - API 키/토큰/prompt 원문 마스킹 또는 최소화

## 9. Timeout / Retry
- ESP32 -> Relay timeout: 예) `12s` (짧게)
- Relay -> Model timeout: 예) `30s`
- Relay -> Telegram timeout: 예) `10s`
- Retry:
  - ESP32는 네트워크 오류 시 1회 재시도
  - Relay는 모델 호출 무한 재시도 금지(폭주 방지)

## 10. Platform Notes
- Vercel:
  - 함수 실행 시간 제한, payload 제한 확인 필요
  - 제한이 빡빡하면 큐/worker 분리 고려
- Hugging Face Space:
  - 지속 실행 프로세스 구성에 유리
  - cold start와 리소스 한도 확인 필요

## 11. ESP32 Integration Notes
- 디바이스는 이미지 bytes를 절대 메모리에 적재하지 않는다.
- 기존 Telegram outbound 파이프라인은 텍스트/메타 중심 유지.
- 새 툴(예: `image_generate_relay`)은 단일 요청-단일 응답 모델로 구현.

## 12. Acceptance Criteria
- 이미지 생성 요청 1건이 Telegram 채팅에 정상 이미지로 도착.
- ESP32는 base64 디코딩 로직 없이 동작.
- Relay 성공 응답에 `file_id` 포함.
- Relay는 업로드 이후 원본 이미지 삭제.
- timeout/실패 시 ESP32가 사용자에게 실패 사유를 텍스트로 안내.

## 13. TODO (Implementation)
- [ ] Relay MVP 구현 (`/v1/image/send`)
- [ ] ESP32 relay client 추가 (HMAC + timeout + 1 retry)
- [ ] Telegram command 연결 (예: `/image <prompt>`)
- [ ] `file_id` 저장 정책 확정 (저장 위치/NVS 여부)
- [ ] 운영 대시보드 최소 로그 정의 (성공률/지연)
