# MimiClaw: $5 칩으로 만드는 포켓 AI 어시스턴트

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![DeepWiki](https://img.shields.io/badge/DeepWiki-mimiclaw-blue.svg)](https://deepwiki.com/memovai/mimiclaw)
[![Discord](https://img.shields.io/badge/Discord-mimiclaw-5865F2?logo=discord&logoColor=white)](https://discord.gg/r8ZxSvB8Yr)
[![X](https://img.shields.io/badge/X-@ssslvky-black?logo=x)](https://x.com/ssslvky)

**[English](README_EN.md) | 한국어 | [中文](README_CN.md)**

> [!IMPORTANT]
> **v0.2.0 신규 기능:**
> - **Kimi (Moonshot AI)** 및 **OpenAI 호환** 서비스 지원!
> - **텔레그램 미디어 처리:** 음성 메시지 실시간 STT 처리 및 사진 메시지 vision 입력 지원
> - **LLM 기반 스케줄러 (NEW!):** 텔레그램으로 "3분 뒤에 알려줘"라고 말하면 자동으로 `/spiffs/public/schedule.md`를 관리하여 알림을 줍니다.
> - **동적 시간 주입:** LLM이 현재 시각(KST)을 항상 정확히 인지하도록 실시간 시간 주입 기능이 추가되었습니다.
> - **오류 복구 강화:** Gemini 등 특정 모델이 반환하는 비표준 도구 호출 에러(배열 형태 등)에 대한 예외 처리를 강화하여 안정적인 중단 및 복구가 가능합니다.

<p align="center">
  <img src="assets/banner.png" alt="MimiClaw" width="480" />
</p>

**$5 칩으로 구현된 세계 최초의 AI 어시스턴트(OpenClaw). Linux도, Node.js도 필요 없습니다. 오직 순수 C로만 작성되었습니다.**

MimiClaw는 작은 ESP32-S3 보드를 개인용 AI 어시스턴트로 바꿔줍니다. USB 전원을 연결하고 WiFi에 접속한 뒤 텔레그램으로 대화하세요. 텔레그램을 통해 어떤 작업이든 처리하고, 로컬 메모리를 통해 시간이 지날수록 사용자에게 맞춰 진화합니다. 이 모든 것이 엄지손가락만한 칩 하나에서 이루어집니다.

## MimiClaw의 특징

- **가벼움** — Linux도, Node.js도, 무거운 라이브러리도 없습니다. 오직 순수 C로만 작동합니다.
- **편리함** — 텔레그램 메시지 하나면 충분합니다. 나머지는 MimiClaw가 알아서 처리합니다.
- **충직함** — 메모리를 통해 학습하며, 재부팅 후에도 사용자를 기억합니다.
- **지치지 않는 열정** — USB 전원, 0.5W의 저전력으로 24시간 내내 대기합니다.
- **경제적** — 약 $5 정도의 ESP32-S3 보드 하나면 충분합니다.

## 작동 원리

![](assets/mimiclaw.png)

텔레그램으로 메시지를 보냅니다. ESP32-S3가 WiFi를 통해 메시지를 수신하고 에이전트 루프에 전달합니다. Claude(LLM)가 생각하고, 도구를 호출하고, 메모리를 읽어서 최적의 답변을 생성하여 텔레그램으로 다시 보냅니다. 모든 과정은 단돈 $5짜리 칩 하나에서 실행되며, 모든 데이터는 로컬 플래시에 안전하게 저장됩니다.

## 빠른 시작

### 준비물

- **ESP32-S3 개발 보드** (16MB Flash, 8MB PSRAM 권장. 예: Xiaozhi AI 보드 등, 약 $10 내외)
- **USB Type-C 케이블**
- **텔레그램 봇 토큰** — 텔레그램 [@BotFather](https://t.me/BotFather)를 통해 생성 가능
- **Anthropic API Key** (Claude) 또는 **Moonshot API Key** (Kimi)
- **(선택) STT API Key** — 음성 노트를 사용하는 경우 (예: Groq)

### 설치

```bash
# 먼저 ESP-IDF v5.5 이상이 설치되어 있어야 합니다:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

# 4MB Flash / 2MB PSRAM 기기에 최적화됨
# (sdkconfig.defaults.esp32s3 및 partitions.csv에 미리 설정됨)
idf.py set-target esp32s3
```

### 설정

MimiClaw는 **2계층 설정** 시스템을 사용합니다. `mimi_secrets.h`에 빌드 타임 기본값을 설정하고, 실행 중에 시리얼 CLI를 통해 설정을 변경할 수 있습니다. CLI에서 설정한 값은 NVS 플래시에 저장되어 빌드 타임 설정보다 우선 적용됩니다.

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

`main/mimi_secrets.h` 수정:

```c
#define MIMI_SECRET_WIFI_SSID       "WiFi이름"
#define MIMI_SECRET_WIFI_PASS       "WiFi비밀번호"
#define MIMI_SECRET_TG_TOKEN        "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11"

/* 서비스 선택: MIMI_LLM_PROVIDER_ANTHROPIC 또는 MIMI_LLM_PROVIDER_OPENAI */
#define MIMI_SECRET_PROVIDER        MIMI_LLM_PROVIDER_ANTHROPIC 
#define MIMI_SECRET_API_KEY         "sk-ant-api03-xxxxx"
#define MIMI_SECRET_BASE_URL        "https://api.anthropic.com/v1/messages" // 또는 https://api.moonshot.cn/v1/chat/completions

#define MIMI_SECRET_SEARCH_KEY      ""              // 선택사항: Brave Search API 키
#define MIMI_SECRET_PROXY_HOST      ""              // 선택사항: 예) "10.0.0.1"
#define MIMI_SECRET_PROXY_PORT      ""              // 선택사항: 예) "7897"

/* STT (Groq/Whisper) */
#define MIMI_SECRET_STT_PROVIDER    MIMI_STT_PROVIDER_GROQ
#define MIMI_SECRET_STT_KEY         ""
#define MIMI_SECRET_STT_BASE_URL    "https://api.groq.com/openai/v1"
#define MIMI_SECRET_STT_MODEL       "whisper-large-v3"
```

빌드 및 플래싱:

```bash
# 클린 빌드 (mimi_secrets.h 변경 시 필수)
idf.py fullclean && idf.py build

# 시리얼 포트 찾기
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# 플래싱 및 모니터링 (PORT를 실제 포트로 변경)
# USB 어댑터: 보통 /dev/cu.usbmodem11401 (macOS) 또는 /dev/ttyACM0 (Linux)
idf.py -p PORT flash monitor
```

### CLI 명령어

시리얼로 접속하여 설정을 변경하거나 디버깅할 수 있습니다. **설정 명령어**를 사용하면 펌웨어를 다시 컴파일하지 않고도 USB 케이블만 연결해서 설정을 바꿀 수 있습니다.

**런타임 설정** (NVS에 저장됨):

```
mimi> wifi_set MySSID MyPassword   # WiFi 네트워크 변경
mimi> set_tg_token 123456:ABC...   # 텔레그램 봇 토큰 변경

# LLM 설정
mimi> set_provider kimi            # Kimi (Moonshot AI)로 전환
mimi> set_base_url https://api.moonshot.cn/v1/chat/completions
mimi> set_api_key sk-xxxxxxxx...   # Kimi API 키 설정
mimi> set_model moonshot-v1-8k     # Kimi 모델 설정

mimi> set_proxy 127.0.0.1 7897     # HTTP 프록시 설정
mimi> clear_proxy                  # 프록시 제거
mimi> set_search_key BSA...        # Brave Search API 키 설정
mimi> set_stt_provider groq        # STT 제공자 설정 (현재 지원: groq)
mimi> set_stt_base_url https://api.groq.com/openai/v1
mimi> set_stt_key gsk_...         # STT API 키 설정
mimi> set_stt_model whisper-large-v3
mimi> set_media_limits 1024 400 10 # 기본 제약(사진KB/음성KB/초)
mimi> tg_auth_add 12345            # 텔레그램 사용자 권한 부여 (chat_id)
mimi> tg_auth_remove 12345         # 권한 제거
mimi> tg_auth_list                 # 권한 부여된 목록 확인
mimi> config_show                  # 모든 설정 확인 (마스킹 처리됨)
mimi> config_reset                 # NVS 초기화 및 빌드 타임 기본값으로 복구
```

**디버그 및 유지보수:**

```
mimi> ls [path]                # 파일 목록 조회 (예: ls public)
mimi> cat <path>               # 파일 내용 보기 (페이징 지원)
mimi> rm <path>                # 파일 삭제 (예: rm public/temp.md)
mimi> wifi_status              # WiFi 연결 상태 확인
mimi> memory_read              # MimiClaw가 기억하는 내용 보기
mimi> memory_write "내용"      # MEMORY.md에 직접 쓰기
mimi> heap_info                # 가용 RAM 확인
mimi> session_list             # 모든 채팅 세션 목록
mimi> session_clear 12345      # 특정 대화 내역 삭제
mimi> restart                  # 재부팅
```

## 보안 및 저장소

MimiClaw는 데이터 보호를 위해 엄격한 **디렉토리 기반 격리** 정책을 시행합니다.

- **Public (`/spiffs/public/`)**: LLM 도구가 접근 가능한 영역입니다. 기억(Memory) 및 성격(Soul) 파일이 저장됩니다.
- **Private (`/spiffs/private/`)**: LLM 도구로부터 숨겨진 영역입니다. 비밀 키, 세션 정보, 기기 탐색 데이터 등이 저장됩니다.
- **로그 마스킹**: 시스템 로그에서 민감한 토큰이나 키 정보는 자동으로 마스킹 처리됩니다.

### 파일 구조

| 파일 | 접근 권한 | 설명 |
|------|-----------|------|
| `SOUL.md` | Public | 봇의 성격 — 행동 방식을 바꾸려면 이 파일을 편집하세요. |
| `USER.md` | Public | 사용자 정보 — 이름, 선호도, 언어 설정 등. |
| `MEMORY.md` | Public | 장기 기억 — 봇이 항상 기억해야 할 내용들. |
| `SESSIONS.json` | Private | 지속적인 HTTP 세션 쿠키 정보. |
| `SECRET.env` | Private | `http_request` 도구에서 사용할 외부 API 키들. |
| `tg_12345.jsonl` | Private | 채팅 기록 — 사용자와의 대화 내역. |

## 사용 도구(Tools)

MimiClaw는 Anthropic의 도구 사용 프로토콜(ReAct 패턴)을 사용합니다. Claude는 도구를 호출하여 세상과 소통합니다.

| 도구명 | 설명 |
|--------|------|
| `web_search` | Brave Search API를 통해 웹에서 최신 정보를 검색합니다. |
| `http_request` | **(보안)** SSRF 방지 및 비밀값 치환 기능이 포함된 GET/POST 요청. |
| `wake_on_lan` | 홈 랩의 기기를 깨우기 위한 Magic Packet을 전송합니다. |
| `list_devices` | 배경에서 ICMP 서브넷 스캔을 통해 찾은 기기 목록을 보여줍니다. |
| `get_current_time` | HTTP를 통해 현재 날짜/시간을 가져오고 시스템 시계를 설정합니다. |

### 보안 HTTP 도구
`http_request` 도구는 보안을 위해 강화되었습니다:
- **SSRF 보호**: 로컬 네트워크 및 사설 IP에 대한 요청을 차단합니다.
- **비밀값 치환**: URL이나 본문에 `{{SECRET:KEY}}`를 사용하여 `SECRET.env`의 키를 안전하게 주입할 수 있습니다.
- **세션 지원**: `Set-Cookie` 헤더를 자동으로 유지하여 상태가 있는 상호작용이 가능합니다.
- **8KB 제한**: 대용량 응답으로 인한 메모리 고갈을 방지합니다.

Brave Search 웹 검색을 활성화하려면 `mimi_secrets.h`에서 `MIMI_SECRET_SEARCH_KEY`로 [Brave Search API 키](https://brave.com/search/api/)를 설정하세요.

## 텔레그램 미디어 처리

- **음성 메모(STT)**: Telegram 음성 메시지는 `streaming` 방식으로 수신되어 Groq Whisper(기본)로 STT 변환 후 텍스트로 LLM에 전달됩니다.
  - 기본 제한: 사진 1 MB, 음성 400 KB, 음성 10초 (변경 필요 시 `set_media_limits`)
- **사진 처리(Vision)**: Telegram `photo`를 받아 스트리밍으로 다운로드하고 base64 이미지 블록으로 변환한 뒤 LLM vision 입력으로 전달합니다.
- **안전 정책**: 용량 초과 또는 인식 실패 시 사용자에게 명확한 안내 문구를 반환합니다.
- **검색 툴 UX 개선**: 검색 결과가 0건일 때 `[ ]` 마크다운 토큰이 텔레그램 이탤릭 규칙에 의해 훼손되지 않도록 메시지 렌더링을 보완했습니다.

## 추가 포함 기능

- **WebSocket 게이트웨이** (18789 포트) — 내부망 어디서든 WebSocket 클라이언트로 접속 가능
- **OTA 업데이트** — USB 없이 WiFi를 통해 펌웨어 업데이트 가능
- **듀얼 코어** — 네트워크 I/O와 AI 처리를 별도의 CPU 코어에서 실행
- **HTTP 프록시** — 제한된 네트워크를 위한 CONNECT 터널 지원
- **도구 사용** — Anthropic 프로토콜 기반의 ReAct 에이전트 루프

## 하드웨어 최적화

MimiClaw는 저비용 ESP32-S3 보드(예: 4MB Flash / 2MB PSRAM)에 맞게 튜닝되었습니다.

- **파티션 테이블** (`partitions.csv`): AI 에이전트를 **4MB Flash** 내에 올릴 수 있도록 최적화된 레이아웃.
- **메모리 설정** (`sdkconfig.defaults.esp32s3`):
    - **Flash 모드**: **QIO 4MB**에 최적화.
    - **PSRAM**: 호환성이 높은 **Quad SPI** 기반 2MB 지원.
    - **TLS/Network**: 안정성을 유지하면서 RAM 사용량을 최소화하도록 버퍼 크기 조정.

## OpenAI 및 호환 서비스 지원

MimiClaw는 OpenAI Chat Completions API 형식을 따르는 모든 서비스를 지원합니다.

**예시: OpenRouter (Kimi/DeepSeek/GLM)**:
  ```bash
  set_provider openai
  set_base_url https://openrouter.ai/api/v1/chat/completions
  set_api_key YOUR_OPENROUTER_KEY
  set_model moonshotai/kimi-k2 # 또는 deepseek/deepseek-r1:free, z-ai/glm-4.5-air:free
  ```

**예시: OpenRouter (Claude)**
```bash
mimi> set_provider openai
mimi> set_base_url https://openrouter.ai/api/v1/chat/completions
mimi> set_api_key sk-or-v1-...
mimi> set_model anthropic/claude-3-opus
```

**예시: DeepSeek (V2)**
```bash
mimi> set_provider openai
mimi> set_base_url https://api.deepseek.com/chat/completions
mimi> set_api_key sk-ds-...
mimi> set_model deepseek-chat
```

## 개발자 가이드

기술적인 세부 사항은 `docs/` 폴더에서 확인할 수 있습니다:

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — 시스템 설계, 모듈 맵, 태스크 레이아웃, 메모리 할당, 프로토콜, 플래시 파티션 등
- **[docs/TODO.md](docs/TODO.md)** — 기능 누락 추적 및 로드맵

## 라이선스

MIT

## 감사의 글

[OpenClaw](https://github.com/openclaw/openclaw)와 [Nanobot](https://github.com/HKUDS/nanobot)에서 영감을 받았습니다. MimiClaw는 임베디드 하드웨어를 위해 핵심 AI 에이전트 구조를 재구현했습니다. Linux도, 서버도 필요 없는 단돈 $5짜리 칩을 위해 탄생했습니다.

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=memovai/mimiclaw&type=Date)](https://star-history.com/#memovai/mimiclaw&Date)
