#include "google_sheet.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"

static const char *TAG = "GSHEET";

/*
 * ============================================================================
 * IMPORTANT - fill this in with YOUR deployment URL.
 *
 * In the Apps Script editor: Deploy -> New deployment -> type "Web app" ->
 * "Execute as: Me", "Who has access: Anyone" -> Deploy. Copy the URL it
 * gives you (it ends in /exec, NOT /edit - the /edit link is the editor,
 * not the callable endpoint).
 *
 *   https://script.google.com/macros/s/XXXXXXXXXXXXXXXXXXXXXXXX/exec
 *
 * Every time you click "New deployment" (not "Manage deployments" -> edit
 * existing) you get a NEW url, so keep this in sync with what you deployed.
 * ============================================================================
 */
#define GSHEET_URL   "https://script.google.com/macros/s/AKfycbwLZmASNkulDk4Z0ukA9Q__ce7F0aUjePFPlifpsTAPBiUrWX2HK_jeqGNMFn3Izhgu4g/exec"

/* Must match the API_TOKEN constant in Code.gs exactly. */
#define GSHEET_TOKEN "ESP32_CHAM_CONG_2026"

#define GSHEET_HTTP_TIMEOUT_MS 8000

/* Response bodies here are always a couple hundred bytes at most
 * (id/name/method/device/error), so a fixed buffer is fine. */
#define GSHEET_RESP_BUF_SIZE 512

/* Apps Script web apps 302-redirect once (sometimes twice) from
 * script.google.com to script.googleusercontent.com/macros/echo?... .
 * Google's echo endpoint only accepts POST, but esp_http_client's
 * built-in auto-redirect silently rewrites POST -> GET when following a
 * 301/302/303 (this is old, technically-standard-but-surprising HTTP
 * behavior), which made every request land as a GET on an endpoint that
 * only accepts POST -> "HTTP status 405" every time, even though the TLS
 * handshake and Apps Script code were both completely fine.
 *
 * Fix: auto-redirect is disabled below, and redirects are followed
 * manually, re-issuing each hop as a POST with the same body. */
#define GSHEET_MAX_MANUAL_REDIRECTS 3
#define GSHEET_LOCATION_BUF_SIZE 512

/*
 * ============================================================================
 * WHY THIS FILE IS STRUCTURED AS A QUEUE + DEDICATED TASK
 * ----------------------------------------------------------------------------
 * google_sheet_log_attendance() does ZERO network I/O and never blocks. It
 * only memcpy's a small fixed-size record into a FreeRTOS queue and
 * returns - safe to call from any task, however small its stack (this
 * matters: an HTTPS request through esp_http_client + esp-tls + mbedtls
 * needs several KB of stack for the TLS handshake and certificate-bundle
 * verification alone, which is more than the 3-4 KB stacks used by
 * rfid_task/fingerprint_task/cam_task - calling it there directly used to
 * cause a stack overflow). A single dedicated "gsheet" task, created by
 * google_sheet_init() with its own generous 8 KB stack, drains the queue
 * and performs the actual blocking HTTP POST (including the manual
 * redirect handling above).
 * ============================================================================
 */
typedef struct {
    char id[32];
    char method[16];
    char device[48];
} gsheet_record_t;

#define GSHEET_QUEUE_LEN   8
#define GSHEET_TASK_STACK  8192   /* sized for HTTPS/TLS - do not reduce */
#define GSHEET_TASK_PRIO   4

static QueueHandle_t s_gsheet_queue;

typedef struct {
    char *buf;
    int len;
    char location[GSHEET_LOCATION_BUF_SIZE];
} gsheet_resp_ctx_t;

static esp_err_t gsheet_http_event_handler(esp_http_client_event_t *evt)
{
    gsheet_resp_ctx_t *ctx = (gsheet_resp_ctx_t *)evt->user_data;
    if (!ctx) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (evt->header_key && strcasecmp(evt->header_key, "Location") == 0 &&
            evt->header_value) {
            snprintf(ctx->location, sizeof(ctx->location), "%s", evt->header_value);
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA &&
               !esp_http_client_is_chunked_response(evt->client)) {
        if (ctx->buf) {
            int space = GSHEET_RESP_BUF_SIZE - 1 - ctx->len;
            int to_copy = evt->data_len < space ? evt->data_len : space;
            if (to_copy > 0) {
                memcpy(ctx->buf + ctx->len, evt->data, to_copy);
                ctx->len += to_copy;
                ctx->buf[ctx->len] = 0;
            }
        }
    }
    return ESP_OK;
}

/*
 * Very small, dependency-free JSON scan: this project does not otherwise
 * link a JSON library, and the response shape is fixed/known (from your
 * own Code.gs), so a full parser would be overkill. Looks for
 * `"ok":true` or `"ok": true` anywhere in the body.
 */
static bool gsheet_response_ok(const char *body)
{
    if (!body) return false;
    return strstr(body, "\"ok\":true") != NULL ||
           strstr(body, "\"ok\": true") != NULL;
}

/* Escapes '"' and '\\' so id/method/device can never break the JSON we
 * hand-build below (e.g. a mis-scanned FACE_ID string from the camera
 * UART, which is free text - see cam_task() in main.c). */
static void gsheet_json_escape(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    if (!in) { if (out_sz) out[0] = 0; return; }
    for (; *in && o + 2 < out_sz; ++in) {
        if (*in == '"' || *in == '\\') {
            if (o + 3 >= out_sz) break;
            out[o++] = '\\';
        }
        out[o++] = *in;
    }
    out[o] = 0;
}

/*
 * Performs the actual blocking HTTPS POST, following redirects manually
 * (see the big comment near GSHEET_MAX_MANUAL_REDIRECTS above for why).
 * Only ever called from gsheet_task() below, which has an 8 KB stack
 * sized for this - never call this directly from anywhere else.
 */
static bool gsheet_http_send(const char *id, const char *method, const char *device)
{
    char id_esc[64], method_esc[32], device_esc[96];
    gsheet_json_escape(id, id_esc, sizeof(id_esc));
    gsheet_json_escape(method, method_esc, sizeof(method_esc));
    gsheet_json_escape(device ? device : "", device_esc, sizeof(device_esc));

    char post_body[256];
    int body_len = snprintf(post_body, sizeof(post_body),
        "{\"token\":\"%s\",\"id\":\"%s\",\"method\":\"%s\",\"device\":\"%s\"}",
        GSHEET_TOKEN, id_esc, method_esc, device_esc);
    if (body_len < 0 || body_len >= (int)sizeof(post_body)) {
        ESP_LOGE(TAG, "POST body truncated, aborting send");
        return false;
    }

    char resp_buf[GSHEET_RESP_BUF_SIZE];
    char url[GSHEET_LOCATION_BUF_SIZE];
    snprintf(url, sizeof(url), "%s", GSHEET_URL);

    /*
     * Created ONCE, before the redirect loop, and its address (&ctx) is
     * handed to esp_http_client_init() as user_data. esp_http_client has
     * no public "set_user_data" call to re-point an already-initialized
     * client at a different context, so instead of creating a new ctx per
     * hop, the SAME ctx is reused for every hop and its fields
     * (len/buf contents/location) are simply reset before each
     * esp_http_client_perform() call below.
     */
    gsheet_resp_ctx_t ctx = { .buf = resp_buf, .len = 0 };
    ctx.location[0] = 0;

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = gsheet_http_event_handler,
        .user_data = &ctx,
        .timeout_ms = GSHEET_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true, /* we follow redirects ourselves, as POST */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }

    bool ok = false;

    for (int hop = 0; hop <= GSHEET_MAX_MANUAL_REDIRECTS; ++hop) {
        /* Reset per-hop response state; ctx.buf still points at resp_buf,
         * which we also clear so a stale body from a previous hop can
         * never be misread as this hop's response. */
        resp_buf[0] = 0;
        ctx.len = 0;
        ctx.location[0] = 0;

        esp_http_client_set_url(client, url);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_body, body_len);

        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP request failed (hop %d): %s", hop, esp_err_to_name(err));
            break;
        }

        int status = esp_http_client_get_status_code(client);

        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
            ctx.location[0]) {
            ESP_LOGI(TAG, "Following redirect (hop %d): %d -> %s", hop, status, ctx.location);
            snprintf(url, sizeof(url), "%s", ctx.location);
            continue; /* re-POST to the new location on the next loop iteration */
        }

        if (status == 200) {
            ok = gsheet_response_ok(resp_buf);
            if (ok) {
                ESP_LOGI(TAG, "Logged to Google Sheet: id=%s method=%s", id, method);
            } else {
                ESP_LOGW(TAG, "Server rejected record: %s", resp_buf);
            }
        } else {
            ESP_LOGW(TAG, "HTTP status %d, body: %s", status, resp_buf);
        }
        break;
    }

    if (client) esp_http_client_cleanup(client);
    return ok;
}

/* The only task in this file that ever touches the network. 8 KB stack,
 * created once by google_sheet_init(). */
static void gsheet_task(void *arg)
{
    gsheet_record_t rec;
    for (;;) {
        if (xQueueReceive(s_gsheet_queue, &rec, portMAX_DELAY) == pdTRUE) {
            gsheet_http_send(rec.id, rec.method, rec.device);
        }
    }
}

bool google_sheet_init(void)
{
    if (s_gsheet_queue) return true; /* already initialized */

    s_gsheet_queue = xQueueCreate(GSHEET_QUEUE_LEN, sizeof(gsheet_record_t));
    if (!s_gsheet_queue) {
        ESP_LOGE(TAG, "Failed to create queue");
        return false;
    }

    BaseType_t created = xTaskCreate(gsheet_task, "gsheet", GSHEET_TASK_STACK,
                                     NULL, GSHEET_TASK_PRIO, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create gsheet task");
        vQueueDelete(s_gsheet_queue);
        s_gsheet_queue = NULL;
        return false;
    }

    ESP_LOGI(TAG, "Google Sheet logging task started");
    return true;
}

bool google_sheet_log_attendance(const char *id, const char *method,
                                  const char *device)
{
    if (!s_gsheet_queue) {
        ESP_LOGW(TAG, "google_sheet_init() was never called; dropping record");
        return false;
    }
    if (!id || !method) {
        ESP_LOGE(TAG, "id/method must not be NULL");
        return false;
    }

    gsheet_record_t rec = {0};
    strncpy(rec.id, id, sizeof(rec.id) - 1);
    strncpy(rec.method, method, sizeof(rec.method) - 1);
    strncpy(rec.device, device ? device : "", sizeof(rec.device) - 1);

    /* Never block the caller (rfid_task/fingerprint_task/cam_task): if
     * the queue is momentarily full, drop the record rather than wait. */
    if (xQueueSend(s_gsheet_queue, &rec, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, dropping attendance record for id=%s", id);
        return false;
    }
    return true;
}