#ifndef GOOGLE_SHEET_H
#define GOOGLE_SHEET_H

#include <stdbool.h>

/*
 * Must be called ONCE from app_main(), before any attendance events can
 * happen (i.e. before xTaskCreate for rfid_task/fingerprint_task/cam_task,
 * or at least before the first grant() can fire). It is fine to call this
 * before Wi-Fi finishes connecting - it only creates a queue and a
 * background task; no network I/O happens until a record is queued.
 *
 * Returns false if the queue/task could not be created (out of memory) -
 * in that case google_sheet_log_attendance() will log a warning and drop
 * every record instead of crashing anything.
 */
bool google_sheet_init(void);

/*
 * Queues one attendance record to be sent to the Google Apps Script Web
 * App in the background, on a dedicated task with its own large stack.
 *
 * IMPORTANT: this function does NOT perform any network I/O itself and
 * does NOT block - it only copies a small fixed-size struct into a queue
 * and returns immediately. This makes it safe to call directly from
 * grant() in main.c, from any of rfid_task / fingerprint_task / cam_task,
 * all of which have small stacks (3-4 KB) that must never be used to run
 * an HTTPS/TLS request (an HTTPS request through esp_http_client +
 * esp-tls + mbedtls needs several KB of stack for the TLS handshake and
 * certificate-bundle verification alone - doing that on those small
 * stacks previously caused a stack overflow that corrupted unrelated
 * memory and crashed the whole device).
 *
 *   id     -> employee ID as typed/scanned (e.g. "101")
 *   method -> "RFID", "FINGER", or "FACE"
 *   device -> free-text device/station name, e.g. "ESP32-GATE-1"
 *
 * Returns true if the record was queued (this does NOT yet mean it was
 * successfully sent - that happens later, in the background; check
 * ESP_LOG output tag "GSHEET" for the actual send result). Returns false
 * if google_sheet_init() was never called, id/method is NULL, or the
 * internal queue is momentarily full (e.g. long Wi-Fi outage with many
 * pending events) - in every failure case the record is simply dropped,
 * it never blocks or crashes the caller.
 */
bool google_sheet_log_attendance(const char *id, const char *method,
                                  const char *device);

#endif