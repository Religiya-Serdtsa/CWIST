# CWIST 웹 개발 튜토리얼 (A to Z)

Spring Boot나 ReactJS에 익숙한 개발자라면 환영합니다! CWIST는 C 언어로 작성된 고성능, 초경량 웹 프레임워크입니다. 이 튜토리얼은 CWIST의 철학과 구조를 빠르게 파악하고, 실제 웹 애플리케이션 개발에 즉시 활용할 수 있도록 작성되었습니다.

> **철학**: "명시적이고, 가볍고, 안전하게." CWIST는 마법(Magic)을 최소화하고, C의 성능을 100% 활용하면서도 현대적인 웹 개발 경험(라우팅, JSON, DB, JWT, Websocket)을 제공합니다.

---

## 목차
1. [Hello World: 첫 서버 띄우기](#1-hello-world-첫-서버-띄우기)
2. [라우팅과 핸들러 (Mux Router)](#2-라우팅과-핸들러-mux-router)
3. [JSON 파싱과 Zod 스키마 검증](#3-json-파싱과-zod-스키마-검증)
4. [미들웨어 (CORS, Logging)](#4-미들웨어-cors-logging)
5. [데이터베이스와 마이그레이션 (SQLite)](#5-데이터베이스와-마이그레이션-sqlite)
6. [인증 (JWT 및 DB 암호화)](#6-인증-jwt-및-db-암호화)
7. [PQC TLS (양자내성 하이브리드 키 교환)](#7-pqc-tls-양자내성-하이브리드-키-교환)
8. [웹소켓 연동 (실시간 양방향 통신)](#8-웹소켓-연동-실시간-양방향-통신)
9. [템플릿 엔진과 정적 파일 제공](#9-템플릿-엔진과-정적-파일-제공)
10. [동적 CSS 합성기 (WASM 및 SSR)](#10-동적-css-합성기-wasm-및-ssr)

---

## 1. Hello World: 첫 서버 띄우기

CWIST의 핵심은 `cwist_app` 객체입니다. Spring의 `ApplicationContext`와 유사하게 앱의 수명 주기를 관리합니다.

```c
#include <cwist/sys/app/app.h>
#include <cwist/net/http/mux.h>
#include <cwist/core/sstring/sstring.h>

// 요청 핸들러 (Spring의 @GetMapping)
void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_sstring_assign(res->body, "Hello, CWIST World!");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

int main() {
    // 1. 앱 생성
    cwist_app *app = cwist_app_create();
    
    // 2. 라우터 설정
    cwist_mux *router = cwist_mux_create();
    cwist_mux_add_route(router, CWIST_HTTP_GET, "/hello", hello_handler);
    
    // 3. 포트 8080에서 서버 실행 (블로킹)
    cwist_app_start(app, router, 8080);
    
    cwist_mux_destroy(router);
    cwist_app_destroy(app);
    return 0;
}
```

---

## 2. 라우팅과 핸들러 (Mux Router)

동적 경로(Path Parameter)와 쿼리 문자열(Query Parameter)을 처리하는 방법입니다. React Router나 Express와 매우 유사한 직관적인 패턴을 제공합니다.

```c
// 예: /users/:id?role=admin
void user_profile_handler(cwist_http_request *req, cwist_http_response *res) {
    // Path 파라미터 읽기 (:id)
    const char *user_id = cwist_query_map_get(req->path_params, "id");
    
    // Query 파라미터 읽기 (?role=admin)
    const char *role = cwist_query_map_get(req->query_params, "role");

    char buf[256];
    snprintf(buf, sizeof(buf), "User ID: %s, Role: %s", user_id, role ? role : "user");
    
    cwist_sstring_assign(res->body, buf);
}

int main() {
    cwist_mux *router = cwist_mux_create();
    
    // 동적 라우팅 등록
    cwist_mux_add_route(router, CWIST_HTTP_GET, "/users/:id", user_profile_handler);
    // ...
}
```

---

## 3. JSON 파싱과 Zod 스키마 검증

TypeScript 진영의 `zod`에서 영감을 받은 강력한 런타임 스키마 검증기입니다. 클라이언트가 보낸 JSON을 안전하게 파싱하고 타입을 검증합니다.

```c
#include <cwist/core/utils/zod.h>
#include <cwist/core/utils/json_builder.h>

void create_user_handler(cwist_http_request *req, cwist_http_response *res) {
    // 1. 스키마 정의 (name은 필수 문자열, age는 필수 숫자)
    cwist_schema *schema = cwist_schema_create(CWIST_TYPE_OBJECT);
    cwist_schema_add_prop(schema, "name", cwist_schema_create(CWIST_TYPE_STRING));
    cwist_schema_add_prop(schema, "age", cwist_schema_create(CWIST_TYPE_NUMBER));

    // 2. 검증 (Body -> JSON)
    cJSON *parsed_json = NULL;
    cwist_zod_result z_res = cwist_zod_parse(req->body->data, schema, &parsed_json);

    if (!z_res.success) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, z_res.error.message);
    } else {
        res->status_code = CWIST_HTTP_CREATED;
        
        // 3. JSON 응답 생성 (cJSON Builder 패턴)
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "status", "User created");
        cJSON_AddStringToObject(reply, "name", cJSON_GetObjectItem(parsed_json, "name")->valuestring);
        
        char *json_str = cJSON_PrintUnformatted(reply);
        cwist_sstring_assign(res->body, json_str);
        
        free(json_str);
        cJSON_Delete(reply);
        cJSON_Delete(parsed_json);
    }
    
    cwist_schema_destroy(schema);
}
```

---

## 4. 미들웨어 (CORS, Logging)

모든 요청을 거쳐가는 파이프라인(Spring의 Interceptor, Express의 Middleware)을 쉽게 구축할 수 있습니다.

```c
#include <cwist/sys/app/middleware.h>

// CORS 처리를 위한 미들웨어
cwist_error_t cors_middleware(cwist_http_request *req, cwist_http_response *res) {
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", "*");
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    
    // OPTIONS 요청 시 바로 응답 (체인 중단)
    if (req->method == CWIST_HTTP_OPTIONS) {
        res->status_code = CWIST_HTTP_NO_CONTENT;
        return make_error(CWIST_ERR_INT16); // 중단
    }
    
    // 에러 타입을 0으로 반환하면 다음 핸들러로 진행
    cwist_error_t proceed = make_error(CWIST_ERR_INT16);
    proceed.error.err_i16 = 0;
    return proceed; 
}

int main() {
    cwist_app *app = cwist_app_create();
    
    // 미들웨어 등록 (전역 적용)
    cwist_app_use_middleware(app, cors_middleware);
    // ...
}
```

---

## 5. 데이터베이스와 마이그레이션 (SQLite)

CWIST는 내장형 SQLite를 완벽히 지원하며, 앱 라이프사이클에 연결된 커넥션 풀 및 마이그레이션 도구를 제공합니다.

```c
#include <cwist/core/db/sql.h>
#include <cwist/core/db/migrate.h>

void get_users_handler(cwist_http_request *req, cwist_http_response *res) {
    // req->db는 앱 구동 시 자동 연결된 DB 인스턴스
    cwist_db *db = req->db;
    
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db->conn, "SELECT id, name FROM users", -1, &stmt, NULL);
    
    cJSON *users_array = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON *user = cJSON_CreateObject();
        cJSON_AddNumberToObject(user, "id", sqlite3_column_int(stmt, 0));
        cJSON_AddStringToObject(user, "name", (const char *)sqlite3_column_text(stmt, 1));
        cJSON_AddItemToArray(users_array, user);
    }
    sqlite3_finalize(stmt);
    
    char *json_str = cJSON_PrintUnformatted(users_array);
    cwist_sstring_assign(res->body, json_str);
    free(json_str);
    cJSON_Delete(users_array);
}

int main() {
    cwist_app *app = cwist_app_create();
    
    // DB 연결 ("file.db" 또는 ":memory:")
    cwist_app_connect_db(app, "app_data.db");
    
    // 스키마 마이그레이션 자동 적용
    cwist_migration_t migrations[] = {
        {"001_create_users", "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);"},
        {"002_insert_seed", "INSERT INTO users (name) VALUES ('Alice'), ('Bob');"}
    };
    cwist_migrate_up(app->db, migrations, 2);

    // ... 라우팅 설정
}
```

---

## 6. 인증 (JWT 및 DB 암호화)

현대 웹의 필수인 Stateless JWT 인증을 내장 함수로 지원합니다.

```c
#include <cwist/security/jwt/jwt.h>

#define SECRET_KEY "my_super_secret"

// 로그인 성공 시 JWT 발급
void login_handler(cwist_http_request *req, cwist_http_response *res) {
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "user_id", "12345");
    cJSON_AddStringToObject(payload, "role", "admin");
    
    // 3600초(1시간) 유효기간의 토큰 생성
    cwist_sstring *token = cwist_jwt_sign(payload, SECRET_KEY, 3600);
    
    cwist_sstring_assign(res->body, token->data);
    cwist_sstring_destroy(token);
    cJSON_Delete(payload);
}

// API 요청 시 JWT 검증 미들웨어
cwist_error_t auth_middleware(cwist_http_request *req, cwist_http_response *res) {
    const char *auth_header = cwist_http_header_get(req->headers, "Authorization");
    
    if (auth_header && strncmp(auth_header, "Bearer ", 7) == 0) {
        const char *token_str = auth_header + 7;
        cwist_error_t verify = cwist_jwt_verify(token_str, SECRET_KEY);
        
        if (verify.errtype == CWIST_ERR_INT16 && verify.error.err_i16 == 0) {
            // 토큰 유효함, 통과!
            return verify;
        }
    }
    
    // 실패 시 401 응답 및 중단
    res->status_code = CWIST_HTTP_UNAUTHORIZED;
    cwist_sstring_assign(res->body, "{\"error\": \"Unauthorized\"}");
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}
```

---

## 7. PQC TLS (양자내성 하이브리드 키 교환)

CWIST는 단 한 줄의 코드로 양자내성(Post-Quantum) 하이브리드 TLS를 활성화할 수 있습니다. 이는 기존 X25519 ECDH에 NIST 표준 ML-KEM-768(Kyber 계열)을 결합한 hybrid KEM 방식으로, 양자 컴퓨터 환경에서도 키 교환이 안전합니다.

```c
#include <cwist/sys/app/app.h>

int main() {
    cwist_app *app = cwist_app_create();

    // HTTPS 활성화
    cwist_app_use_https(app, "server.crt", "server.key");

    // PQC 하이브리드 레이어 활성화 — 한 줄이면 충분
    cwist_app_use_pqc_layer(app, true);

    // 이제 모든 TLS 1.3 연결은 X25519MLKEM768:X25519:P-256 그룹을 사용합니다.
    // TLS 1.2 이하는 자동 비활성화됩니다.
    cwist_app_listen(app, 8443);
    cwist_app_destroy(app);
    return 0;
}
```

### 보안 정책 요약

| 항목 | 설정 |
|------|------|
| Key Exchange Group | `X25519MLKEM768:X25519:P-256` |
| 최소 TLS 버전 | 1.3 |
| 레거시 TLS | 비활성화 (1.0, 1.1, 1.2 제거) |
| downgrade 보호 | 활성화 |

> **참고**: 이 설정은 **transport 계층**의 키 교환에만 적용됩니다. 인증서 서명(signature)까지 PQC로 전환하려면 별도의 `cwist_app_use_pqc_cert()` 같은 기능이 필요하며, 이는 현재 생태계에서 아직 과도한 단계로 간주됩니다.

---

## 8. 웹소켓 연동 (실시간 양방향 통신)

CWIST는 동일한 포트와 라우터 안에서 HTTP 통신을 WebSocket으로 쉽게 업그레이드 할 수 있습니다.

```c
#include <cwist/net/websocket/websocket.h>

// 메시지를 받을 때마다 호출되는 콜백
void on_ws_message(cwist_websocket *ws, const char *msg, size_t len, int opcode) {
    if (opcode == 1) { // Text Message
        printf("Received: %s\n", msg);
        // 에코 응답 (클라이언트로 다시 전송)
        cwist_ws_send_text(ws, msg);
    }
}

// 클라이언트 연결 해제 콜백
void on_ws_close(cwist_websocket *ws) {
    printf("Client disconnected.\n");
}

// 웹소켓 엔드포인트 핸들러
void ws_handler(cwist_http_request *req, cwist_http_response *res) {
    // 1. 요청이 유효한 WS 업그레이드 요청인지 확인
    if (cwist_ws_is_upgrade_request(req)) {
        // 2. 업그레이드 수행. 이후 핸들러가 소켓 제어권을 넘겨받음
        cwist_ws_upgrade(req, res, on_ws_message, on_ws_close);
    } else {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "Expected WebSocket Upgrade");
    }
}

int main() {
    cwist_mux *router = cwist_mux_create();
    cwist_mux_add_route(router, CWIST_HTTP_GET, "/chat", ws_handler);
    // ...
}
```

---

## 9. 템플릿 엔진과 정적 파일 제공

HTML 기반의 SSR(Server-Side Rendering) 프로젝트를 구축하거나, React의 빌드 결과물(정적 파일)을 서비스할 때 유용합니다.

```c
#include <cwist/core/template/template.h>

void render_home_handler(cwist_http_request *req, cwist_http_response *res) {
    // 템플릿 파일 읽기
    cwist_template *tpl = cwist_template_load("views/index.html");
    
    // 데이터 주입 (예: {{ title }} 변수 치환)
    cwist_template_set(tpl, "title", "CWIST Homepage");
    cwist_template_set(tpl, "user", "Developer");
    
    // 렌더링 후 응답
    cwist_sstring *output = cwist_template_render(tpl);
    cwist_sstring_assign(res->body, output->data);
    cwist_http_header_add(&res->headers, "Content-Type", "text/html");
    
    cwist_sstring_destroy(output);
    cwist_template_free(tpl);
}

int main() {
    cwist_app *app = cwist_app_create();
    cwist_mux *router = cwist_mux_create();
    
    cwist_mux_add_route(router, CWIST_HTTP_GET, "/", render_home_handler);
    
    // 정적 디렉토리 마운트 (React/Vue 빌드 결과물 서빙 시)
    // "/public" URL로 들어오면 "./public" 폴더의 파일을 제공
    cwist_app_static(app, router, "/public", "./public");
    
    cwist_app_start(app, router, 8080);
    return 0;
}
```

---

## 9. 동적 CSS 합성기 (WASM 및 SSR)

CWIST는 단순 백엔드 역할을 넘어, C 언어의 강력한 수학적 연산력을 바탕으로 **디자인 시스템을 런타임에 합성해내는 CSS Composer** 기능을 내장하고 있습니다. 색상의 명도(Lightness)를 수학적으로 추론하여 Hover/Active 상태를 만들고, 곡률(Roundness)과 여백(Spacing)을 동적으로 계산합니다.

이를 활용하는 두 가지 대표적인 방식을 소개합니다.

### 방법 A: 100% Server-Side Rendering (SSR)
서버에서 동적으로 테마 CSS를 생성하여 렌더링 시점에 주입하는 방식입니다. 사용자별 커스텀 테마를 제공할 때 매우 유용합니다.

```c
#include <cwist/core/html/css_composer.h>

void theme_css_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_css_config cfg;
    cwist_css_config_init(&cfg);

    // 쿼리 파라미터로 받은 헥스(Hex) 코드를 파싱하여 메인 컬러 지정
    const char *color = cwist_query_map_get(req->query_params, "color");
    if (color) {
        cfg.primary_color = cwist_color_hex_to_rgb(color);
    }
    
    // 다크모드 여부 지정
    const char *dark = cwist_query_map_get(req->query_params, "dark");
    cfg.is_dark_mode = (dark && strcmp(dark, "1") == 0);

    // CSS 합성에 필요한 수학적 계산(HSL 변환 등) 수행 후 스타일시트 문자열 반환
    cwist_sstring *css_output = cwist_css_generate_stylesheet(&cfg);

    cwist_sstring_assign(res->body, css_output->data);
    cwist_http_header_add(&res->headers, "Content-Type", "text/css");
    
    cwist_sstring_destroy(css_output);
}

int main() {
    cwist_app *app = cwist_app_create();
    cwist_mux *router = cwist_mux_create();
    
    // <link rel="stylesheet" href="/theme.css?color=ff5733&dark=1"> 로 접근 가능
    cwist_mux_add_route(router, CWIST_HTTP_GET, "/theme.css", theme_css_handler);
    
    cwist_app_start(app, router, 8080);
    return 0;
}
```

### 방법 B: React + WebAssembly (WASM) 클라이언트 사이드 합성
CWIST의 `css_composer.c`는 프레임워크 독립적으로 작성되어, Emscripten을 통해 `.wasm`으로 빌드한 뒤 React 앱 내부로 가져와 클라이언트 측에서 즉각적으로 연산시킬 수도 있습니다.

1. **WASM 빌드 (Emscripten)**
```bash
emcc src/core/html/css_composer.c -Iinclude \
    -s EXPORTED_FUNCTIONS="['_cwist_color_hex_to_rgb', '_cwist_css_generate_stylesheet', '_malloc', '_free']" \
    -o public/css_composer.js
```

2. **React에서 활용 (동적 디자인 시스템)**
```javascript
import React, { useEffect, useState } from 'react';

function DynamicThemeApp() {
  const [themeColor, setThemeColor] = useState("#3B82F6");

  useEffect(() => {
    // 1. WASM 모듈 로드
    window.Module().then((module) => {
      // 2. 입력받은 hex 코드를 C의 RGB 구조체로 변환
      const hexPtr = module.allocateUTF8(themeColor);
      const rgb = module._cwist_color_hex_to_rgb(hexPtr);
      module._free(hexPtr);

      // (가상 예시) C의 config 구조체를 메모리에 구성 후 CSS 합성
      // 실제로는 JS <-> C 브릿지 함수(wrapper)를 만들어 호출하는 것이 편리합니다.
      const cssStringPtr = module._cwist_css_generate_stylesheet(/* config_ptr */);
      const cssString = module.UTF8ToString(cssStringPtr);

      // 3. 브라우저 DOM에 즉시 주입
      document.getElementById('dynamic-theme').innerText = cssString;
    });
  }, [themeColor]);

  return (
    <div className="bg-body text-main">
      <style id="dynamic-theme"></style>
      <input type="color" value={themeColor} onChange={e => setThemeColor(e.target.value)} />
      <button className="bg-primary radius-md">CWIST Themed Button</button>
    </div>
  );
}
```

---

### 마치며
이 튜토리얼을 통해 C 기반 환경임에도 불구하고 얼마나 친숙하고 선언적으로 웹 개발을 할 수 있는지 확인하셨길 바랍니다. `cwist_app` 구조체가 전체 생명주기를, `cwist_mux`가 라우팅을 담당한다는 점만 기억하면 기존 모던 프레임워크와 동일한 아키텍처로 개발을 진행할 수 있습니다.
