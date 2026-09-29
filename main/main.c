// #include <stdio.h>
// #include <stdbool.h>
// #include <string.h>
// #include <stdlib.h>
// #include <inttypes.h>
// #include "nvs_flash.h"

// /* Log tag for the main system. Wi-Fi/NTP uses its own TAG in wifi_time.c. */
// static const char *TAG = "ACCESS";
// #include "wifi_time.h"

// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/semphr.h"
// #include "driver/gpio.h"
// #include "driver/spi_master.h"
// #include "driver/uart.h"
// #include "esp_err.h"
// #include "esp_lcd_panel_io.h"
// #include "esp_log.h"
// #include "esp_rom_sys.h"
// #include "esp_timer.h"
// #include "nvs.h"
// #include "nvs_flash.h"

// /* ============================== PIN MAP ============================== */
// #define RC522_HOST       SPI2_HOST
// #define PIN_SCK          18
// #define PIN_MOSI         23
// #define PIN_MISO         19
// #define PIN_RC522_CS     21
// #define PIN_AS608_RX     16
// #define PIN_AS608_TX     17
// #define PIN_CAM_RX       32
// #define PIN_CAM_TX       33
// #define PIN_LD2410_OUT   35
// #define PIN_TFT_CS       15
// #define PIN_TFT_DC       2

// /* GPIO4/12/13/14 are the scanned lines; GPIO22/25/26/27 are the driven lines. */
// static const gpio_num_t rows[] = {4, 12, 13, 14};
// static const gpio_num_t cols[] = {22, 25, 26, 27};

// #define AS608_UART       UART_NUM_2
// #define CAM_UART         UART_NUM_1
// #define AS608_BAUD       57600 /* Change to 115200 if you already changed the AS608's baud. */
// #define CAM_BAUD         115200

// #define TFT_W            128
// #define TFT_H            160
// #define RFID_UID_LEN     4    /* This build targets 4-byte MIFARE UID cards. */

// /* Confirmation codes in the AS608/R30x protocol. */
// #define AS608_OK         0x00
// #define AS608_NO_FINGER  0x02
// #define AS608_NO_MATCH   0x09

// /* If enrollment (started with B) is not completed within this time, it is
//  * auto-cancelled, so attendance is never locked out forever if an admin
//  * walks away mid-enrollment. */
// #define ENROLL_TIMEOUT_US (60LL * 1000000LL)
// #define ADMIN_IDLE_TIMEOUT_US (10LL * 1000000LL)
// #define ATTENDANCE_RESULT_US (1500000LL)
// #define RESULT_GREEN 0x07E0
// #define RESULT_RED   0xF800

// static spi_device_handle_t rc522;
// static esp_lcd_panel_io_handle_t tft_io;
// static uint16_t tft_frame[TFT_W * TFT_H];
// static int64_t last_accept_us;
// static int64_t result_until_us;
// static bool result_active;
// static bool rfid_need_release;
// static bool fp_need_release;
// static SemaphoreHandle_t tft_mutex;

// typedef enum {
//     ENROLL_NONE = 0,
//     ENROLL_WAIT_RFID,
//     ENROLL_WAIT_FINGER_1,
//     ENROLL_WAIT_FINGER_REMOVE,
//     ENROLL_WAIT_FINGER_2,
//     ENROLL_WAIT_VERIFY_REMOVE,
//     ENROLL_WAIT_VERIFY,
// } enroll_state_t;

// /*
//  * Mutex protecting the ENTIRE admin/enrollment state block below.
//  * keypad_task writes it, rfid_task/fingerprint_task read and write it too -
//  * they run on different ESP32 cores, so a plain `volatile` is not enough to
//  * keep several related variables consistent with each other (e.g.
//  * enroll_emp_id must stay in sync with current_enroll_state).
//  */
// static SemaphoreHandle_t state_mutex;

// static enroll_state_t current_enroll_state = ENROLL_NONE;
// static bool admin_authenticated;
// /* True while the admin is on the "enter PIN" screen, i.e. after '*' is
//  * pressed but before the PIN has been accepted. Without this flag,
//  * attendance_ready() would still report "ready" during PIN entry (since
//  * admin_authenticated only flips to true *after* the PIN is verified),
//  * so a background task could redraw the idle/attendance screen over the
//  * PIN entry screen a second or so after '*' was pressed. */
// static bool admin_pin_entry_active;
// static uint16_t enroll_emp_id;
// static uint8_t enroll_rfid_uid[RFID_UID_LEN];
// static bool enroll_rfid_pending;
// static int64_t enroll_started_us;
// /* Read from the sensor at boot; never guess the capacity from a constant. */
// static uint16_t fp_library_size = 300;

// static char input_buffer[12];
// static size_t input_len;

// #define LOCK()   xSemaphoreTake(state_mutex, portMAX_DELAY)
// #define UNLOCK() xSemaphoreGive(state_mutex)

// /*
//  * ============================================================================
//  * ADMIN MENU UI (used ONLY inside keypad_task, never read/written by any
//  * other task) - so these variables do NOT need to sit under state_mutex.
//  * admin_authenticated / current_enroll_state (already covered by the mutex
//  * above) remain the "source of truth" for rfid_task & fingerprint_task;
//  * admin_ui_mode is just a display/navigation layer on top of that, never
//  * read by the other two tasks, so it does not break the existing
//  * synchronization rules.
//  * ============================================================================
//  */
// typedef enum {
//     ADMIN_UI_NONE = 0,      /* Not logged in (entering PIN, or idle screen)   */
//     ADMIN_UI_MENU,          /* Logged in, waiting for add/delete selection    */
//     ADMIN_UI_ENTER_ADD_ID,  /* Entering the employee ID to ADD                */
//     ADMIN_UI_ENTER_DEL_ID,  /* Entering the employee ID to DELETE             */
// } admin_ui_mode_t;

// /* ============================== TFT ============================== */
// static void tft_cmd(uint8_t cmd, const uint8_t *param, size_t len)
// {
//     ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(tft_io, cmd, param, len));
// }

// static void tft_flush(void)
// {
//     uint8_t x[] = {0, 0, 0, TFT_W - 1};
//     uint8_t y[] = {0, 0, 0, TFT_H - 1};
//     tft_cmd(0x2A, x, sizeof(x));
//     tft_cmd(0x2B, y, sizeof(y));
//     ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(tft_io, 0x2C, tft_frame,
//                                                sizeof(tft_frame)));
// }

// static void tft_fill(uint16_t color)
// {
//     for (size_t i = 0; i < TFT_W * TFT_H; ++i) tft_frame[i] = color;
// }

// static void tft_box(int x, int y, int w, int h, uint16_t color)
// {
//     for (int yy = y; yy < y + h && yy < TFT_H; ++yy)
//         for (int xx = x; xx < x + w && xx < TFT_W; ++xx)
//             if (xx >= 0 && yy >= 0) tft_frame[yy * TFT_W + xx] = color;
// }

// /* 5x7 font: A-Z, 0-9, ':' and '/'. Display only ever needs plain ASCII. */
// static const uint8_t glyph[][5] = {
//     {0,0,0,0,0}, {0x1e,0x05,0x05,0x1e,0}, {0x1f,0x15,0x15,0x0a,0},
//     {0x0e,0x11,0x11,0x11,0}, {0x1f,0x11,0x11,0x0e,0}, {0x1f,0x15,0x15,0x11,0},
//     {0x1f,0x05,0x05,0x01,0}, {0x0e,0x11,0x15,0x1d,0}, {0x1f,0x04,0x04,0x1f,0},
//     {0x11,0x1f,0x11,0,0}, {0x08,0x10,0x10,0x0f,0}, {0x1f,0x04,0x0a,0x11,0},
//     {0x1f,0x10,0x10,0x10,0}, {0x1f,0x02,0x04,0x02,0x1f}, {0x1f,0x02,0x04,0x1f,0},
//     {0x0e,0x11,0x11,0x0e,0}, {0x1f,0x05,0x05,0x02,0}, {0x0e,0x11,0x19,0x1e,0},
//     {0x1f,0x05,0x0d,0x12,0}, {0x12,0x15,0x15,0x09,0}, {0x01,0x1f,0x01,0,0},
//     {0x0f,0x10,0x10,0x0f,0}, {0x07,0x08,0x10,0x08,0x07}, {0x1f,0x08,0x04,0x08,0x1f},
//     {0x1b,0x04,0x04,0x1b,0}, {0x03,0x04,0x18,0x04,0x03}, {0x19,0x15,0x13,0,0},
//     {0x0e,0x11,0x11,0x0e,0}, {0x00,0x12,0x1f,0x10,0}, {0x19,0x15,0x15,0x12,0},
//     {0x11,0x15,0x15,0x0a,0}, {0x07,0x04,0x04,0x1f,0}, {0x17,0x15,0x15,0x09,0},
//     {0x0e,0x15,0x15,0x08,0}, {0x01,0x01,0x1d,0x03,0}, {0x0a,0x15,0x15,0x0a,0},
//     {0x02,0x15,0x15,0x0e,0},

//     /* ':' - two dots stacked in the MIDDLE column (col index 2), at rows 2
//      * and 4. The previous version put a single dot in columns 1 and 3 at
//      * the same row, which draws two dots side by side - i.e. it looked
//      * like ".." instead of ":". */
//     {0x00, 0x00, 0x14, 0x00, 0x00},

//     /* '/' - a forward slash must go from bottom-left to top-right: bottom
//      * row (row6) on the left column, top row (row0) on the right column.
//      * The previous version had the bit pattern of a BACKSLASH (top-left to
//      * bottom-right), which is why "24\09\2026" printed with '\' even
//      * though the code intended '/'. */
//     {0x40, 0x20, 0x08, 0x02, 0x01}
// };

// static int glyph_index(char c)
// {
//     if (c >= 'A' && c <= 'Z') return 1 + c - 'A';
//     if (c >= '0' && c <= '9') return 27 + c - '0';
//     if (c == ':') return 37;
//     if (c == '/') return 38;
//     return 0;
// }

// static void tft_text(int x, int y, const char *text, uint16_t color)
// {
//     for (; *text; ++text, x += 6) {
//         const uint8_t *g = glyph[glyph_index(*text)];
//         for (int col = 0; col < 5; ++col)
//             for (int row = 0; row < 7; ++row)
//                 if (g[col] & (1U << row)) tft_box(x + col, y + row, 1, 1, color);
//     }
// }

// static void tft_message_color(const char *title, const char *line1,
//                               const char *line2, uint16_t color)
// {
//     if (!tft_io) return;
//     if (tft_mutex) xSemaphoreTake(tft_mutex, portMAX_DELAY);
//     tft_fill(0x0010);
//     tft_box(0, 0, TFT_W, 22, color);
//     tft_text(6, 7, title ? title : "", 0xffff);
//     tft_text(5, 45, line1 ? line1 : "", color);
//     tft_text(5, 65, line2 ? line2 : "", color);
//     tft_flush();
//     if (tft_mutex) xSemaphoreGive(tft_mutex);
// }

// static void tft_message(const char *title, const char *line1, const char *line2)
// {
//     tft_message_color(title, line1, line2, 0x07e0);
// }

// static void tft_ready_screen(void)
// {
//     char day_str[12];
//     char time_str[16];
//     char date_str[16];

//     wifi_time_get_display(day_str, sizeof(day_str),
//                           time_str, sizeof(time_str),
//                           date_str, sizeof(date_str));

//     if (tft_mutex) xSemaphoreTake(tft_mutex, portMAX_DELAY);

//     tft_fill(0x0010);

//     tft_text(5, 40, "SYSTEM READY", 0x07e0);

//     /* Line 1: weekday, e.g. "THURSDAY" */
//     tft_text(5, 65, day_str, 0xffff);

//     /* Line 2: HH : MM : SS */
//     tft_text(5, 85, time_str, 0xffff);

//     /* Line 3: DD/MM/YYYY */
//     tft_text(5, 103, date_str, 0xffff);

//     tft_flush();

//     if (tft_mutex) xSemaphoreGive(tft_mutex);
// }

// static void tft_result_screen(const char *method, const char *id, bool success)
// {
//     char day_str[12];
//     char time_str[16];
//     char date_str[16];

//     wifi_time_get_display(day_str, sizeof(day_str),
//                           time_str, sizeof(time_str),
//                           date_str, sizeof(date_str));

//     if (tft_mutex) xSemaphoreTake(tft_mutex, portMAX_DELAY);
//     tft_fill(0x0010);
//     tft_box(0, 0, TFT_W, 22, success ? RESULT_GREEN : RESULT_RED);
//     tft_text(6, 7, success ? "SUCCESS" : "FAILED", 0xffff);

//     if (success) {
//         char line1[24];
//         snprintf(line1, sizeof(line1), "%s %s", method ? method : "",
//                  id ? id : "");
//         tft_text(5, 40, line1, RESULT_GREEN);
//         tft_text(5, 58, "THANK YOU", RESULT_GREEN);
//         tft_text(5, 78, day_str, 0xffff);
//         tft_text(5, 96, time_str, 0xffff);
//         tft_text(5, 114, date_str, 0xffff);
//     } else {
//         tft_text(5, 40, "ACCESS DENIED", RESULT_RED);
//         tft_text(5, 58, "PLEASE TRY AGAIN", RESULT_RED);
//         tft_text(5, 78, day_str, 0xffff);
//         tft_text(5, 96, time_str, 0xffff);
//         tft_text(5, 114, date_str, 0xffff);
//     }
//     tft_flush();
//     if (tft_mutex) xSemaphoreGive(tft_mutex);
// }

// /*
//  * "Input" screen shared by: entering the admin PIN, entering the ID to add,
//  * entering the ID to delete. Shows the value being typed directly inside a
//  * highlighted box for easy monitoring/proofreading, with a key-hint line
//  * below. ADDED ONLY - does not touch tft_message() / the TFT functions above.
//  */
// static void tft_input_screen(const char *title, const char *prompt,
//                               const char *value, const char *hint1,
//                               const char *hint2)
// {
//     if (!tft_io) return;
//     tft_fill(0x0010);
//     tft_box(0, 0, TFT_W, 22, 0x03E0);           /* title bar */
//     tft_text(6, 7, title, 0xffff);

//     tft_text(8, 34, prompt, 0xffe0);

//     /* highlighted input box */
//     tft_box(6, 52, TFT_W - 12, 18, 0x0000);
//     tft_box(6, 52, TFT_W - 12, 1, 0x07e0);
//     tft_box(6, 69, TFT_W - 12, 1, 0x07e0);
//     tft_text(10, 57, (value && *value) ? value : "-", 0x07e0);

//     tft_text(8, 100, hint1 ? hint1 : "", 0x07ff);
//     tft_text(8, 116, hint2 ? hint2 : "", 0x07ff);
//     tft_flush();
// }

// static void tft_init(void)
// {
//     esp_lcd_panel_io_spi_config_t io = {
//         .cs_gpio_num = PIN_TFT_CS, .dc_gpio_num = PIN_TFT_DC,
//         .spi_mode = 0, .pclk_hz = 20 * 1000 * 1000, .trans_queue_depth = 1,
//         .lcd_cmd_bits = 8, .lcd_param_bits = 8,
//     };
//     ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)RC522_HOST,
//                                                &io, &tft_io));
//     tft_cmd(0x01, NULL, 0); vTaskDelay(pdMS_TO_TICKS(150));
//     tft_cmd(0x11, NULL, 0); vTaskDelay(pdMS_TO_TICKS(120));
//     uint8_t madctl = 0xc8, colmod = 0x05;
//     tft_cmd(0x36, &madctl, 1); tft_cmd(0x3a, &colmod, 1);
//     tft_cmd(0x13, NULL, 0); tft_cmd(0x29, NULL, 0);
//     tft_ready_screen();
// }

// /* ============================== NVS / access ============================== */
// static void rfid_key(const uint8_t uid[RFID_UID_LEN], char key[16])
// {
//     snprintf(key, 16, "u%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
// }

// static esp_err_t save_rfid_to_nvs(const uint8_t uid[RFID_UID_LEN], uint16_t emp_id)
// {
//     char key[16]; rfid_key(uid, key);
//     nvs_handle_t h;
//     esp_err_t err = nvs_open("users", NVS_READWRITE, &h);
//     if (err != ESP_OK) return err;
//     err = nvs_set_u16(h, key, emp_id);
//     if (err == ESP_OK) err = nvs_commit(h);
//     nvs_close(h);
//     return err;
// }

// static esp_err_t erase_rfid_from_nvs(const uint8_t uid[RFID_UID_LEN])
// {
//     char key[16]; rfid_key(uid, key);
//     nvs_handle_t h;
//     esp_err_t err = nvs_open("users", NVS_READWRITE, &h);
//     if (err != ESP_OK) return err;
//     err = nvs_erase_key(h, key);
//     if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
//     if (err == ESP_OK) err = nvs_commit(h);
//     nvs_close(h);
//     return err;
// }

// static uint16_t check_rfid_in_nvs(const uint8_t uid[RFID_UID_LEN])
// {
//     char key[16]; rfid_key(uid, key);
//     uint16_t emp_id = 0;
//     nvs_handle_t h;
//     if (nvs_open("users", NVS_READONLY, &h) == ESP_OK) {
//         (void)nvs_get_u16(h, key, &emp_id);
//         nvs_close(h);
//     }
//     return emp_id;
// }

// /*
//  * Reverse-lookup the RFID UID for a given emp_id (the "users" namespace
//  * stores key=UID, value=emp_id, so the whole namespace must be scanned).
//  * Used when deleting an employee: we only have the ID typed on the keypad
//  * and need to find the matching RFID card to remove it completely.
//  * Does not touch the NVS functions above - read only.
//  */
// static bool find_uid_by_emp_id(uint16_t emp_id, uint8_t uid_out[RFID_UID_LEN])
// {
//     bool found = false;
//     nvs_handle_t h;
//     if (nvs_open("users", NVS_READONLY, &h) != ESP_OK) return false;

//     nvs_iterator_t it = NULL;
//     esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, "users", NVS_TYPE_U16, &it);
//     while (res == ESP_OK && it != NULL) {
//         nvs_entry_info_t info;
//         nvs_entry_info(it, &info);
//         uint16_t value = 0;
//         if (nvs_get_u16(h, info.key, &value) == ESP_OK && value == emp_id) {
//             unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0;
//             if (sscanf(info.key, "u%02x%02x%02x%02x", &b0, &b1, &b2, &b3) == 4) {
//                 uid_out[0] = (uint8_t)b0; uid_out[1] = (uint8_t)b1;
//                 uid_out[2] = (uint8_t)b2; uid_out[3] = (uint8_t)b3;
//                 found = true;
//             }
//             break;
//         }
//         res = nvs_entry_next(&it);
//     }
//     if (it) nvs_release_iterator(it);
//     nvs_close(h);
//     return found;
// }

// static bool nvs_pin_ok(const char *pin)
// {
//     char saved[12] = "123";
//     size_t n = sizeof(saved);
//     nvs_handle_t h;
//     if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
//         (void)nvs_get_str(h, "master", saved, &n);
//         nvs_close(h);
//     }
//     return strcmp(pin, saved) == 0;
// }

// static void log_event(const char *method, const char *id)
// {
//     nvs_handle_t h;
//     if (nvs_open("logs", NVS_READWRITE, &h) == ESP_OK) {
//         char value[64];
//         snprintf(value, sizeof(value), "%s:%s:%" PRId64,
//                  method, id, esp_timer_get_time() / 1000000);
//         (void)nvs_set_str(h, "latest", value);
//         (void)nvs_commit(h);
//         nvs_close(h);
//     }
// }

// /* Read both flags under lock for a consistent snapshot. */
// static bool attendance_ready(void)
// {
//     LOCK();
//     bool ready = !admin_authenticated &&
//                  !admin_pin_entry_active &&
//                  current_enroll_state == ENROLL_NONE &&
//                  !result_active;
//     UNLOCK();
//     return ready;
// }

// static void grant(const char *method, const char *id)
// {
//     int64_t now = esp_timer_get_time();

//     LOCK();
//     bool ready = !admin_authenticated &&
//                  !admin_pin_entry_active &&
//                  current_enroll_state == ENROLL_NONE &&
//                  !result_active;
//     if (!ready || now - last_accept_us < ATTENDANCE_RESULT_US) {
//         UNLOCK();
//         return;
//     }
//     last_accept_us = now;
//     result_active = true;
//     result_until_us = now + ATTENDANCE_RESULT_US;
//     if (method && strcmp(method, "RFID") == 0) rfid_need_release = true;
//     if (method && strcmp(method, "FINGER") == 0) fp_need_release = true;
//     UNLOCK();

//     ESP_LOGI(TAG, "=== AUTHENTICATION SUCCESSFUL! %s, ID: %s ===", method, id);
//     tft_result_screen(method, id, true);
// }

// /* ============================== RC522 ============================== */
// static uint8_t rcr(uint8_t reg)
// {
//     uint8_t tx[2] = {(uint8_t)((reg << 1) | 0x80), 0}, rx[2] = {0};
//     spi_transaction_t t = {.length = 16, .tx_buffer = tx, .rx_buffer = rx};
//     if (spi_device_transmit(rc522, &t) != ESP_OK) return 0;
//     return rx[1];
// }

// static void wcr(uint8_t reg, uint8_t value)
// {
//     uint8_t tx[2] = {(uint8_t)(reg << 1), value};
//     spi_transaction_t t = {.length = 16, .tx_buffer = tx};
//     ESP_ERROR_CHECK(spi_device_transmit(rc522, &t));
// }

// static void rset(uint8_t reg, uint8_t mask) { wcr(reg, rcr(reg) | mask); }
// static void rclr(uint8_t reg, uint8_t mask) { wcr(reg, rcr(reg) & ~mask); }

// static void rc522_init(void)
// {
//     spi_bus_config_t bus = {
//         .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCK,
//         .max_transfer_sz = TFT_W * TFT_H * 2,
//     };
//     spi_device_interface_config_t dev = {
//         .clock_speed_hz = 1000000, .mode = 0, .spics_io_num = PIN_RC522_CS,
//         .queue_size = 1,
//     };
//     ESP_ERROR_CHECK(spi_bus_initialize(RC522_HOST, &bus, SPI_DMA_CH_AUTO));
//     ESP_ERROR_CHECK(spi_bus_add_device(RC522_HOST, &dev, &rc522));
//     wcr(0x01, 0x0f);       /* soft reset */
//     vTaskDelay(pdMS_TO_TICKS(50));
//     wcr(0x2a, 0x8d); wcr(0x2b, 0x3e); /* timer */
//     wcr(0x2d, 30); wcr(0x2c, 0);
//     wcr(0x15, 0x40);       /* ModeReg: CRC preset 0x6363 */
//     rset(0x14, 0x03);      /* TxControlReg: turn on antenna */
// }

// static int rc522_xfer_bits(const uint8_t *send, int slen, uint8_t *back,
//                             int *blen, uint8_t tx_last_bits)
// {
//     wcr(0x01, 0x00);       /* CommandReg: Idle */
//     wcr(0x02, 0x77);       /* ComIEnReg */
//     wcr(0x04, 0x7f);       /* ComIrqReg: clear old flags */
//     rclr(0x0a, 0x80);      /* FIFOLevelReg: FlushBuffer */
//     for (int i = 0; i < slen; ++i) wcr(0x09, send[i]);
//     wcr(0x0d, tx_last_bits & 0x07); /* BitFramingReg */
//     wcr(0x01, 0x0c);       /* Transceive */
//     rset(0x0d, 0x80);      /* StartSend */

//     const int64_t deadline = esp_timer_get_time() + 25000;
//     uint8_t irq = 0;
//     do {
//         irq = rcr(0x04);
//         if (irq & 0x01) break; /* TimerIRq */
//         vTaskDelay(pdMS_TO_TICKS(1));
//     } while (!(irq & 0x30) && esp_timer_get_time() < deadline);
//     rclr(0x0d, 0x80);

//     if (!(irq & 0x30) || (rcr(0x06) & 0x1b)) return -1; /* ErrorReg */
//     int count = rcr(0x0a);
//     if (count > *blen) return -1;
//     for (int i = 0; i < count; ++i) back[i] = rcr(0x09);
//     *blen = count;
//     return count ? 0 : -1;
// }

// static int rc522_xfer(const uint8_t *send, int slen, uint8_t *back, int *blen)
// {
//     return rc522_xfer_bits(send, slen, back, blen, 0);
// }

// static bool rc522_poll(uint8_t uid[RFID_UID_LEN])
// {
//     uint8_t answer[18];
//     int n = sizeof(answer);
//     const uint8_t request[] = {0x26}; /* REQA must be a 7-bit frame */
//     if (rc522_xfer_bits(request, sizeof(request), answer, &n, 7) != 0 || n != 2)
//         return false;

//     const uint8_t anticollision[] = {0x93, 0x20};
//     n = sizeof(answer);
//     if (rc522_xfer(anticollision, sizeof(anticollision), answer, &n) != 0 || n < 5)
//         return false;
//     if ((uint8_t)(answer[0] ^ answer[1] ^ answer[2] ^ answer[3]) != answer[4])
//         return false;
//     memcpy(uid, answer, RFID_UID_LEN);
//     return true;
// }

// /* ============================== AS608 ============================== */
// /*
//  * Returns ESP_OK when the UART packet is valid. The AS608's own
//  * success/failure code lives in *status; every I/O error must NOT be
//  * turned into "no finger present".
//  */
// static esp_err_t as608_exec(uint8_t cmd, const uint8_t *data, size_t len,
//                              uint8_t *reply, size_t *rlen, uint8_t *status)
// {
//     if (len > 20 || !reply || !rlen || !status) return ESP_ERR_INVALID_ARG;
//     uint8_t packet[32] = {0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01,
//                           (uint8_t)((len + 3) >> 8), (uint8_t)(len + 3), cmd};
//     uint16_t sum = 1 + packet[7] + packet[8] + cmd;
//     for (size_t i = 0; i < len; ++i) { packet[10 + i] = data[i]; sum += data[i]; }
//     packet[10 + len] = (uint8_t)(sum >> 8);
//     packet[11 + len] = (uint8_t)sum;

//     uart_flush_input(AS608_UART);
//     if (uart_write_bytes(AS608_UART, (const char *)packet, 12 + len) < 0)
//         return ESP_FAIL;
//     uint8_t header[9];
//     if (uart_read_bytes(AS608_UART, header, sizeof(header), pdMS_TO_TICKS(800)) != sizeof(header) ||
//         header[0] != 0xef || header[1] != 0x01 || header[6] != 0x07)
//         return ESP_ERR_INVALID_RESPONSE;

//     int body_len = ((header[7] << 8) | header[8]) - 2; /* confirmation + parameters */
//     /*
//      * IMPORTANT FIX: the uart_read_bytes call below reads "body_len + 2"
//      * bytes (the extra 2 are the checksum) into `reply`. The previous
//      * check only compared body_len > *rlen, so when body_len == *rlen
//      * (e.g. == 32, exactly sizeof(reply[32])) the read would write 34
//      * bytes into an array that only holds 32 -> stack overflow. A noisy
//      * UART frame (loose wire, electrical noise) can absolutely produce a
//      * body_len value that close to the boundary before the checksum
//      * below gets checked.
//      */
//     if (body_len < 1 || body_len + 2 > (int)*rlen) return ESP_ERR_INVALID_SIZE;
//     if (uart_read_bytes(AS608_UART, reply, body_len + 2, pdMS_TO_TICKS(500)) != body_len + 2)
//         return ESP_ERR_TIMEOUT;

//     uint16_t checksum = 0;
//     for (int i = 6; i < 9; ++i) checksum += header[i];
//     for (int i = 0; i < body_len; ++i) checksum += reply[i];
//     if (checksum != (uint16_t)((reply[body_len] << 8) | reply[body_len + 1]))
//         return ESP_ERR_INVALID_CRC;
//     *rlen = body_len;
//     *status = reply[0];
//     return ESP_OK;
// }

// static bool as608_cmd(uint8_t cmd, const uint8_t *data, size_t len,
//                       uint8_t *reply, size_t *rlen)
// {
//     uint8_t status;
//     return as608_exec(cmd, data, len, reply, rlen, &status) == ESP_OK && status == AS608_OK;
// }

// static bool as608_is_online(void)
// {
//     uint8_t reply[32];
//     size_t n = sizeof(reply);
//     return as608_cmd(0x0f, NULL, 0, reply, &n); /* ReadSysPara */
// }

// static void as608_read_capacity(void)
// {
//     uint8_t reply[32], status;
//     size_t n = sizeof(reply);
//     if (as608_exec(0x0f, NULL, 0, reply, &n, &status) == ESP_OK &&
//         status == AS608_OK && n >= 17) {
//         uint16_t capacity = ((uint16_t)reply[5] << 8) | reply[6];
//         if (capacity > 0) fp_library_size = capacity;
//     }
// }

// /* Search must scan exactly the sensor's own reported library size. */
// static bool as608_search(uint16_t *matched_id, uint16_t *score, uint8_t *status)
// {
//     uint8_t reply[32];
//     const uint8_t request[] = {1, 0, 0,
//                                (uint8_t)(fp_library_size >> 8),
//                                (uint8_t)fp_library_size};
//     size_t n = sizeof(reply);
//     *matched_id = 0;
//     *score = 0;
//     *status = 0xff;
//     esp_err_t err = as608_exec(0x04, request, sizeof(request), reply, &n, status);
//     if (err != ESP_OK || *status != AS608_OK || n < 5) return false;
//     *matched_id = ((uint16_t)reply[1] << 8) | reply[2];
//     *score = ((uint16_t)reply[3] << 8) | reply[4];
//     return true;
// }

// static void as608_delete_template(uint16_t id)
// {
//     uint8_t reply[16];
//     uint8_t request[] = {(uint8_t)(id >> 8), (uint8_t)id, 0, 1};
//     size_t n = sizeof(reply);
//     if (!as608_cmd(0x0c, request, sizeof(request), reply, &n))
//         ESP_LOGE(TAG, "Failed to delete template, ID %u", id);
// }

// /*
//  * Fully deletes one employee - fingerprint (AS608) + RFID (NVS), so the old
//  * RFID card becomes a completely "blank" card again (no longer mapped to
//  * any ID). Only called from keypad_task, after the admin has already
//  * authenticated - does not touch current_enroll_state/admin_authenticated,
//  * so it does not need state_mutex; the helper functions
//  * (as608_delete_template/erase_rfid_from_nvs) are already called unlocked
//  * elsewhere in this same pattern.
//  * Returns true if a matching RFID record for emp_id was found and removed.
//  */
// static bool delete_employee(uint16_t emp_id)
// {
//     uint8_t uid[RFID_UID_LEN];
//     bool had_rfid = find_uid_by_emp_id(emp_id, uid);

//     as608_delete_template(emp_id); /* safe even if this ID never had a fingerprint */

//     if (had_rfid) {
//         (void)erase_rfid_from_nvs(uid);
//     }

//     char id_str[8];
//     snprintf(id_str, sizeof(id_str), "%u", emp_id);
//     log_event("DEL", id_str);
//     return had_rfid;
// }

// /* ============================== keypad ============================== */
// static int64_t last_press_us;
// static char last_key;
// #define DEBOUNCE_US 150000

// static char keypad_scan(void)
// {
//     static const char keymap[4][4] = {
//         {'1','2','3','A'}, {'4','5','6','B'}, {'7','8','9','C'}, {'*','0','#','D'}
//     };
//     char pressed = 0;
//     for (int c = 0; c < 4 && !pressed; ++c) {
//         for (int k = 0; k < 4; ++k) gpio_set_level(cols[k], k == c ? 0 : 1);
//         esp_rom_delay_us(20);
//         for (int r = 0; r < 4; ++r)
//             if (gpio_get_level(rows[r]) == 0) { pressed = keymap[c][r]; break; }
//     }
//     for (int k = 0; k < 4; ++k) gpio_set_level(cols[k], 1);
//     if (!pressed) { last_key = 0; return 0; }
//     int64_t now = esp_timer_get_time();
//     if (pressed == last_key && now - last_press_us < DEBOUNCE_US) return 0;
//     last_key = pressed; last_press_us = now;
//     return pressed;
// }

// /* Must be called while state_mutex is already held. */
// static void cancel_admin_or_enrollment_locked(void)
// {
//     /* If Store already ran but verification has not happened yet, do not
//      * leave half-finished data behind. The AS608/NVS calls below do not
//      * need the mutex held, but we snapshot the values we need before
//      * leaving the locked region for the slow I/O calls; here
//      * as608_delete_template/erase_rfid_from_nvs do not touch shared state
//      * so calling them directly is safe. */
//     bool need_rollback = enroll_rfid_pending &&
//         (current_enroll_state == ENROLL_WAIT_VERIFY_REMOVE ||
//          current_enroll_state == ENROLL_WAIT_VERIFY);
//     uint16_t rollback_id = enroll_emp_id;
//     uint8_t rollback_uid[RFID_UID_LEN];
//     memcpy(rollback_uid, enroll_rfid_uid, RFID_UID_LEN);

//     admin_authenticated = false;
//     admin_pin_entry_active = false;
//     current_enroll_state = ENROLL_NONE;
//     enroll_rfid_pending = false;
//     enroll_emp_id = 0;
//     input_len = 0;

//     if (need_rollback) {
//         UNLOCK();
//         as608_delete_template(rollback_id);
//         (void)erase_rfid_from_nvs(rollback_uid);
//         LOCK();
//     }
// }

// static void cancel_enrollment_keep_admin(void)
// {
//     LOCK();
//     bool need_rollback = enroll_rfid_pending &&
//         (current_enroll_state == ENROLL_WAIT_VERIFY_REMOVE ||
//          current_enroll_state == ENROLL_WAIT_VERIFY);
//     uint16_t rollback_id = enroll_emp_id;
//     uint8_t rollback_uid[RFID_UID_LEN];
//     memcpy(rollback_uid, enroll_rfid_uid, RFID_UID_LEN);

//     current_enroll_state = ENROLL_NONE;
//     enroll_rfid_pending = false;
//     enroll_emp_id = 0;
//     input_len = 0;
//     UNLOCK();

//     if (need_rollback) {
//         as608_delete_template(rollback_id);
//         (void)erase_rfid_from_nvs(rollback_uid);
//     }
// }

// static void cancel_admin_or_enrollment(void)
// {
//     LOCK();
//     cancel_admin_or_enrollment_locked();
//     UNLOCK();
//     tft_ready_screen();
// }

// /* ---- Admin screens (display only - does not touch the original logic) ---- */
// static void ui_show_pin_entry(void)
// {
//     char shown[12];
//     size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
//     memcpy(shown, input_buffer, n); shown[n] = 0;
//     tft_input_screen("ADMIN LOGIN", "PIN CODE:", shown,
//                      "B:DEL  C:CONFIRM", "#:BACK");
// }

// static void ui_show_menu(bool delete_selected)
// {
//     tft_input_screen("ADMIN", "SELECT ACTION",
//                      delete_selected ? "DELETE EMPLOYEE" : "ADD EMPLOYEE",
//                      "A:SWITCH  B:BACK", "C:SELECT");
// }

// static void ui_show_add_id_entry(void)
// {
//     char shown[12];
//     size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
//     memcpy(shown, input_buffer, n); shown[n] = 0;
//     tft_input_screen("ADD EMPLOYEE", "ENTER ID:", shown,
//                      "B:DEL  C:CONFIRM", "#:BACK");
// }

// static void ui_show_del_id_entry(void)
// {
//     char shown[12];
//     size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
//     memcpy(shown, input_buffer, n); shown[n] = 0;
//     tft_input_screen("DELETE EMPLOYEE", "ENTER ID:", shown,
//                      "B:DEL  C:CONFIRM", "#:BACK");
// }


// static void keypad_task(void *arg)
// {
//     bool entering_master = false;
//     admin_ui_mode_t admin_ui_mode = ADMIN_UI_NONE;
//     bool delete_selected = false;
//     int64_t last_ui_action_us = esp_timer_get_time();

//     while (true) {
//         char key = keypad_scan();

//         LOCK();
//         bool is_admin = admin_authenticated;
//         enroll_state_t enroll_now = current_enroll_state;
//         UNLOCK();

//         if (is_admin && esp_timer_get_time() - last_ui_action_us >=
//                         ADMIN_IDLE_TIMEOUT_US) {
//             ESP_LOGI(TAG, "Admin idle for 10s -> back to attendance mode");
//             cancel_admin_or_enrollment();
//             entering_master = false;
//             admin_ui_mode = ADMIN_UI_NONE;
//             delete_selected = false;
//             last_ui_action_us = esp_timer_get_time();
//             continue;
//         }

//         if (!key) {
//             vTaskDelay(pdMS_TO_TICKS(20));
//             continue;
//         }
//         last_ui_action_us = esp_timer_get_time();

//         LOCK();
//         is_admin = admin_authenticated;
//         enroll_now = current_enroll_state;
//         UNLOCK();

//         /* # = immediately cancel whatever is in progress and go back to
//          * the attendance screen. */
//         if (key == '#') {
//             cancel_admin_or_enrollment();
//             entering_master = false;
//             admin_ui_mode = ADMIN_UI_NONE;
//             delete_selected = false;
//             last_ui_action_us = esp_timer_get_time();
//             continue;
//         }

//         /* * = enter admin mode. */
//         if (key == '*') {
//             cancel_admin_or_enrollment();
//             LOCK();
//             admin_pin_entry_active = true;
//             UNLOCK();
//             entering_master = true;
//             admin_ui_mode = ADMIN_UI_NONE;
//             delete_selected = false;
//             input_len = 0;
//             ui_show_pin_entry();
//             continue;
//         }

//         if (entering_master) {
//             if (key == 'B') {
//                 if (input_len > 0) {
//                     input_len--;
//                     ui_show_pin_entry();
//                 }
//             } else if (key == 'C') {
//                 input_buffer[input_len] = 0;
//                 if (nvs_pin_ok(input_buffer)) {
//                     LOCK();
//                     admin_authenticated = true;
//                     admin_pin_entry_active = false;
//                     UNLOCK();
//                     entering_master = false;
//                     input_len = 0;
//                     admin_ui_mode = ADMIN_UI_MENU;
//                     delete_selected = false;
//                     ui_show_menu(false);
//                 } else {
//                     input_len = 0;
//                     tft_message_color("FAILED", "WRONG PIN",
//                                       "RETRY", RESULT_RED);
//                     vTaskDelay(pdMS_TO_TICKS(700));
//                     ui_show_pin_entry();
//                 }
//             } else if (key >= '0' && key <= '9' &&
//                        input_len < sizeof(input_buffer) - 1) {
//                 input_buffer[input_len++] = key;
//                 ui_show_pin_entry();
//             }
//             vTaskDelay(pdMS_TO_TICKS(20));
//             continue;
//         }

//         if (!is_admin) {
//             vTaskDelay(pdMS_TO_TICKS(20));
//             continue;
//         }

//         /* Currently enrolling: B goes back to the admin menu, only # exits
//          * all the way back to attendance mode. */
//         if (enroll_now != ENROLL_NONE) {
//             if (key == 'B') {
//                 cancel_enrollment_keep_admin();
//                 admin_ui_mode = ADMIN_UI_MENU;
//                 delete_selected = false;
//                 ui_show_menu(false);
//             }
//             vTaskDelay(pdMS_TO_TICKS(20));
//             continue;
//         }

//         if (admin_ui_mode == ADMIN_UI_MENU) {
//             if (key == 'A') {
//                 delete_selected = !delete_selected;
//                 ui_show_menu(delete_selected);
//             } else if (key == 'B') {
//                 cancel_admin_or_enrollment();
//                 admin_ui_mode = ADMIN_UI_NONE;
//                 delete_selected = false;
//                 tft_ready_screen();
//             } else if (key == 'C') {
//                 input_len = 0;
//                 if (delete_selected) {
//                     admin_ui_mode = ADMIN_UI_ENTER_DEL_ID;
//                     ui_show_del_id_entry();
//                 } else {
//                     admin_ui_mode = ADMIN_UI_ENTER_ADD_ID;
//                     ui_show_add_id_entry();
//                 }
//             }
//         } else if (admin_ui_mode == ADMIN_UI_ENTER_ADD_ID) {
//             if (key == 'B') {
//                 if (input_len > 0) {
//                     input_len--;
//                     ui_show_add_id_entry();
//                 } else {
//                     admin_ui_mode = ADMIN_UI_MENU;
//                     ui_show_menu(false);
//                 }
//             } else if (key == 'C') {
//                 input_buffer[input_len] = 0;
//                 long id = strtol(input_buffer, NULL, 10);

//                 if (id > 0 && id < fp_library_size) {
//                     LOCK();
//                     enroll_emp_id = (uint16_t)id;
//                     enroll_rfid_pending = false;
//                     current_enroll_state = ENROLL_WAIT_RFID;
//                     enroll_started_us = esp_timer_get_time();
//                     UNLOCK();

//                     input_len = 0;
//                     admin_ui_mode = ADMIN_UI_MENU;
//                     tft_message("ADD EMPLOYEE", "SCAN RFID CARD",
//                                 "B:BACK");
//                 } else {
//                     tft_message_color("FAILED", "INVALID ID",
//                                        "RE-ENTER", RESULT_RED);
//                     vTaskDelay(pdMS_TO_TICKS(700));
//                     ui_show_add_id_entry();
//                 }
//             } else if (key >= '0' && key <= '9' &&
//                        input_len < sizeof(input_buffer) - 1) {
//                 input_buffer[input_len++] = key;
//                 ui_show_add_id_entry();
//             }
//         } else if (admin_ui_mode == ADMIN_UI_ENTER_DEL_ID) {
//             if (key == 'B') {
//                 if (input_len > 0) {
//                     input_len--;
//                     ui_show_del_id_entry();
//                 } else {
//                     admin_ui_mode = ADMIN_UI_MENU;
//                     ui_show_menu(true);
//                 }
//             } else if (key == 'C') {
//                 input_buffer[input_len] = 0;
//                 long id = strtol(input_buffer, NULL, 10);

//                 if (id > 0 && id < fp_library_size) {
//                     bool existed = delete_employee((uint16_t)id);
//                     char id_str[12];
//                     snprintf(id_str, sizeof(id_str), "ID %ld", id);

//                     if (existed)
//                         tft_message("DELETE OK", id_str, "RFID & FP REMOVED");
//                     else
//                         tft_message("DELETE OK", id_str, "NO RFID FOUND");

//                     vTaskDelay(pdMS_TO_TICKS(1200));
//                     admin_ui_mode = ADMIN_UI_MENU;
//                     delete_selected = false;
//                     ui_show_menu(false);
//                 } else {
//                     tft_message_color("FAILED", "INVALID ID",
//                                        "RE-ENTER", RESULT_RED);
//                     vTaskDelay(pdMS_TO_TICKS(700));
//                     ui_show_del_id_entry();
//                 }
//             } else if (key >= '0' && key <= '9' &&
//                        input_len < sizeof(input_buffer) - 1) {
//                 input_buffer[input_len++] = key;
//                 ui_show_del_id_entry();
//             }
//         }

//         vTaskDelay(pdMS_TO_TICKS(20));
//     }
// }

// /* Background task: if the admin presses B and then walks away without
//  * finishing (never presses D), the system automatically exits the enroll
//  * state after ENROLL_TIMEOUT_US instead of locking attendance forever. */
// static void enroll_watchdog_task(void *arg)
// {
//     while (true) {
//         LOCK();
//         bool expired = current_enroll_state != ENROLL_NONE &&
//             (esp_timer_get_time() - enroll_started_us) > ENROLL_TIMEOUT_US;
//         UNLOCK();
//         if (expired) {
//             ESP_LOGW(TAG, "Enrollment timed out, auto-cancelled");
//             cancel_admin_or_enrollment();
//             tft_message_color("FAILED", "ENROLLMENT CANCELLED", "READY", RESULT_RED);
//             vTaskDelay(pdMS_TO_TICKS(1500));
//             if (attendance_ready()) tft_ready_screen();
//         }
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }

// static void ui_housekeeping_task(void *arg)
// {
//     int64_t last_clock_refresh = 0;

//     while (true) {
//         int64_t now = esp_timer_get_time();

//         LOCK();
//         bool active = result_active;
//         int64_t until = result_until_us;
//         UNLOCK();

//         if (active && now >= until) {
//             LOCK();
//             result_active = false;
//             UNLOCK();
//             if (attendance_ready()) tft_ready_screen();
//         }

//         if (!active && attendance_ready() &&
//             now - last_clock_refresh >= 1000000LL) {
//             last_clock_refresh = now;
//             tft_ready_screen();
//         }

//         vTaskDelay(pdMS_TO_TICKS(100));
//     }
// }

// /* ============================== Tasks ============================== */
// static void rfid_task(void *arg)
// {
//     uint8_t uid[RFID_UID_LEN];
//     char id[12];
//     while (true) {
//         bool card_present = rc522_poll(uid);
//         if (!card_present) {
//             LOCK();
//             if (rfid_need_release) rfid_need_release = false;
//             UNLOCK();
//         } else {
//             LOCK();
//             bool in_enroll_wait_rfid = (current_enroll_state == ENROLL_WAIT_RFID) &&
//                                         admin_authenticated;
//             uint16_t enroll_id_snapshot = enroll_emp_id;
//             UNLOCK();

//             if (in_enroll_wait_rfid) {
//                 uint16_t existing = check_rfid_in_nvs(uid);
//                 if (existing != 0 && existing != enroll_id_snapshot) {
//                     ESP_LOGW(TAG, "RFID card already belongs to ID %u", existing);
//                     tft_message("CARD EXISTS", "SCAN OTHER CARD", "B:CANCEL");
//                 } else if (!as608_is_online()) {
//                     /* Do not save the card if the fingerprint sensor is offline. */
//                     ESP_LOGE(TAG, "AS608 not responding; RFID not saved");
//                     tft_message("FINGERPRINT ERROR", "CHECK AS608", "SCAN CARD AGAIN");
//                 } else {
//                     LOCK();
//                     /* Re-check the state is still correct after the slow I/O
//                      * above (avoid overwriting if the admin already pressed
//                      * B or the watchdog already cancelled while we waited). */
//                     if (current_enroll_state == ENROLL_WAIT_RFID &&
//                         enroll_emp_id == enroll_id_snapshot) {
//                         memcpy(enroll_rfid_uid, uid, sizeof(uid));
//                         enroll_rfid_pending = true;
//                         current_enroll_state = ENROLL_WAIT_FINGER_1;
//                         enroll_started_us = esp_timer_get_time();
//                         UNLOCK();
//                         ESP_LOGI(TAG, "RFID OK; AS608 OK. Place finger 1st time");
//                         tft_message("CARD OK", "PLACE FINGER", "STEP 1");
//                     } else {
//                         UNLOCK();
//                     }
//                 }
//                 vTaskDelay(pdMS_TO_TICKS(800));
//             } else if (attendance_ready()) {
//                 LOCK();
//                 bool blocked = rfid_need_release;
//                 UNLOCK();
//                 if (blocked) {
//                     /* Only unlock RFID once the card has been pulled away. */
//                     continue;
//                 }
//                 uint16_t emp = check_rfid_in_nvs(uid);
//                 if (emp) {
//                     snprintf(id, sizeof(id), "%u", emp);
//                     log_event("RFID", id); grant("RFID", id);
//                 } else {
//                     ESP_LOGW(TAG, "Unregistered RFID card");
//                     tft_result_screen("RFID", "", false);
//                 }
//                 vTaskDelay(pdMS_TO_TICKS(800));
//             }
//         }
//         vTaskDelay(pdMS_TO_TICKS(80));
//     }
// }

// static void fingerprint_task(void *arg)
// {
//     uint8_t reply[32];
//     while (true) {
//         size_t n = sizeof(reply);

//         LOCK();
//         enroll_state_t st = current_enroll_state;
//         uint16_t emp_id_snapshot = enroll_emp_id;
//         UNLOCK();

//         if (st == ENROLL_WAIT_FINGER_1) {
//             if (as608_cmd(0x01, NULL, 0, reply, &n)) {
//                 uint8_t buffer = 1; n = sizeof(reply);
//                 if (as608_cmd(0x02, &buffer, 1, reply, &n)) {
//                     LOCK();
//                     if (current_enroll_state == ENROLL_WAIT_FINGER_1) {
//                         current_enroll_state = ENROLL_WAIT_FINGER_REMOVE;
//                         enroll_started_us = esp_timer_get_time();
//                     }
//                     UNLOCK();
//                     ESP_LOGI(TAG, "Fingerprint 1 OK; lift finger");
//                     tft_message("FINGER OK", "LIFT FINGER", "WAIT FOR STEP 2");
//                 }
//             }
//         } else if (st == ENROLL_WAIT_FINGER_REMOVE) {
//             /* Only status 0x02 really means the finger has been lifted. */
//             uint8_t status;
//             esp_err_t err = as608_exec(0x01, NULL, 0, reply, &n, &status);
//             if (err == ESP_OK && status == AS608_NO_FINGER) {
//                 LOCK();
//                 if (current_enroll_state == ENROLL_WAIT_FINGER_REMOVE) {
//                     current_enroll_state = ENROLL_WAIT_FINGER_2;
//                     enroll_started_us = esp_timer_get_time();
//                 }
//                 UNLOCK();
//                 tft_message("PLACE AGAIN", "SAME FINGER", "STEP 2");
//                 vTaskDelay(pdMS_TO_TICKS(400));
//             } else if (err != ESP_OK) {
//                 ESP_LOGW(TAG, "AS608 UART error while waiting for lift: %s", esp_err_to_name(err));
//             }
//         } else if (st == ENROLL_WAIT_FINGER_2) {
//             if (as608_cmd(0x01, NULL, 0, reply, &n)) {
//                 uint8_t buffer = 2; n = sizeof(reply);
//                 if (!as608_cmd(0x02, &buffer, 1, reply, &n)) goto next;
//                 n = sizeof(reply);
//                 if (!as608_cmd(0x05, NULL, 0, reply, &n)) {
//                     ESP_LOGW(TAG, "The two fingerprint scans do not match");
//                     LOCK();
//                     if (current_enroll_state == ENROLL_WAIT_FINGER_2) {
//                         current_enroll_state = ENROLL_WAIT_FINGER_1;
//                         enroll_started_us = esp_timer_get_time();
//                     }
//                     UNLOCK();
//                     tft_message("MISMATCH", "RESTART STEP 1", "RETRY");
//                     goto next;
//                 }
//                 uint8_t store[] = {1, (uint8_t)(emp_id_snapshot >> 8), (uint8_t)emp_id_snapshot};
//                 n = sizeof(reply);
//                 if (!as608_cmd(0x06, store, sizeof(store), reply, &n)) {
//                     ESP_LOGE(TAG, "Failed to store AS608 template");
//                     tft_message("SAVE ERROR", "RETRY", "B:CANCEL");
//                     goto next;
//                 }

//                 bool save_ok;
//                 LOCK();
//                 save_ok = enroll_rfid_pending &&
//                           save_rfid_to_nvs(enroll_rfid_uid, emp_id_snapshot) == ESP_OK;
//                 if (save_ok && current_enroll_state == ENROLL_WAIT_FINGER_2) {
//                     /* Store only confirms the data was written to flash, not
//                      * that Search can actually find it. Require lifting the
//                      * finger and scanning a 3rd time to verify for real. */
//                     current_enroll_state = ENROLL_WAIT_VERIFY_REMOVE;
//                     enroll_started_us = esp_timer_get_time();
//                 } else if (!save_ok) {
//                     ESP_LOGE(TAG, "Template saved but RFID not written to NVS");
//                     enroll_rfid_pending = false;
//                     current_enroll_state = ENROLL_NONE;
//                     admin_authenticated = false;
//                 }
//                 UNLOCK();

//                 if (save_ok) {
//                     tft_message("TEMPLATE SAVED", "LIFT FINGER", "VERIFYING NEXT");
//                 } else {
//                     tft_message("NVS ERROR", "CARD NOT SAVED", "CHECK NVS");
//                 }
//             }
//         } else if (st == ENROLL_WAIT_VERIFY_REMOVE) {
//             uint8_t status;
//             esp_err_t err = as608_exec(0x01, NULL, 0, reply, &n, &status);
//             if (err == ESP_OK && status == AS608_NO_FINGER) {
//                 LOCK();
//                 if (current_enroll_state == ENROLL_WAIT_VERIFY_REMOVE) {
//                     current_enroll_state = ENROLL_WAIT_VERIFY;
//                     enroll_started_us = esp_timer_get_time();
//                 }
//                 UNLOCK();
//                 tft_message("VERIFY", "PLACE FINGER AGAIN", "STEP 3");
//                 vTaskDelay(pdMS_TO_TICKS(400));
//             } else if (err != ESP_OK) {
//                 ESP_LOGW(TAG, "AS608 UART error while waiting for verify: %s", esp_err_to_name(err));
//             }
//         } else if (st == ENROLL_WAIT_VERIFY) {
//             if (as608_cmd(0x01, NULL, 0, reply, &n)) {
//                 uint8_t buffer = 1; n = sizeof(reply);
//                 if (!as608_cmd(0x02, &buffer, 1, reply, &n)) {
//                     ESP_LOGW(TAG, "Failed to extract features for verification");
//                     goto next;
//                 }
//                 uint16_t matched, score;
//                 uint8_t status;
//                 bool verify_ok = as608_search(&matched, &score, &status) &&
//                                   matched == emp_id_snapshot;

//                 if (verify_ok) {
//                     LOCK();
//                     if (current_enroll_state == ENROLL_WAIT_VERIFY) {
//                         enroll_rfid_pending = false;
//                         current_enroll_state = ENROLL_NONE;
//                         admin_authenticated = false;
//                     }
//                     UNLOCK();
//                     ESP_LOGI(TAG, "VERIFICATION COMPLETE: ID %u, score %u", matched, score);
//                     tft_message("ADD EMPLOYEE", "VERIFIED OK", "READY");
//                     vTaskDelay(pdMS_TO_TICKS(1500));
//                     tft_ready_screen();
//                 } else {
//                     /* Never leave behind an account that can enroll but
//                      * cannot actually authenticate. */
//                     ESP_LOGE(TAG, "Verification failed: status=0x%02X, match=%u, expected=%u",
//                              status, matched, emp_id_snapshot);
//                     uint8_t uid_snapshot[RFID_UID_LEN];
//                     LOCK();
//                     memcpy(uid_snapshot, enroll_rfid_uid, RFID_UID_LEN);
//                     enroll_rfid_pending = false;
//                     current_enroll_state = ENROLL_NONE;
//                     admin_authenticated = false;
//                     UNLOCK();
//                     as608_delete_template(emp_id_snapshot);
//                     (void)erase_rfid_from_nvs(uid_snapshot);
//                     tft_message_color("FAILED", "DATA ROLLED BACK", "RETRY", RESULT_RED);
//                     vTaskDelay(pdMS_TO_TICKS(1500));
//                     if (attendance_ready()) tft_ready_screen();
//                 }
//             }
//         } else if (attendance_ready()) {
//             LOCK();
//             bool blocked = fp_need_release;
//             UNLOCK();

//             if (blocked) {
//                 uint8_t release_status = 0xff;
//                 size_t rn = sizeof(reply);
//                 esp_err_t re = as608_exec(0x01, NULL, 0, reply, &rn,
//                                           &release_status);
//                 if (re == ESP_OK && release_status == AS608_NO_FINGER) {
//                     LOCK();
//                     fp_need_release = false;
//                     UNLOCK();
//                 }
//             } else if (as608_cmd(0x01, NULL, 0, reply, &n)) {
//                 uint8_t buffer = 1; n = sizeof(reply);
//                 if (as608_cmd(0x02, &buffer, 1, reply, &n)) {
//                     uint16_t matched, score;
//                     uint8_t status;
//                     if (as608_search(&matched, &score, &status)) {
//                         char id[8]; snprintf(id, sizeof(id), "%u", matched);
//                         ESP_LOGI(TAG, "Fingerprint match ID=%u score=%u", matched, score);
//                         log_event("FP", id); grant("FINGER", id);
//                     } else {
//                         ESP_LOGW(TAG, "Fingerprint no match (AS608 status=0x%02X)", status);
//                         tft_result_screen("FINGER", "", false);
//                         vTaskDelay(pdMS_TO_TICKS(700));
//                         if (attendance_ready()) tft_ready_screen();
//                     }
//                 }
//             }
//         }
// next:
//         vTaskDelay(pdMS_TO_TICKS(150));
//     }
// }

// static void cam_task(void *arg)
// {
//     char buffer[96];
//     while (true) {
//         int n = uart_read_bytes(CAM_UART, (uint8_t *)buffer, sizeof(buffer) - 1,
//                                 pdMS_TO_TICKS(100));
//         if (n > 0 && attendance_ready()) {
//             buffer[n] = 0;
//             char *p = strstr(buffer, "FACE_ID:");
//             if (p) {
//                 char *end = strpbrk(p, "\r\n"); if (end) *end = 0;
//                 log_event("FACE", p + 8); grant("FACE", p + 8);
//             }
//         }
//     }
// }

// static void presence_task(void *arg)
// {
//     int old = -1;
//     while (true) {
//         int now = gpio_get_level(PIN_LD2410_OUT);
//         if (now != old) {
//             old = now;
//             uart_write_bytes(CAM_UART, now ? "PRESENCE:1\n" : "PRESENCE:0\n", 11);
//             ESP_LOGI(TAG, "LD2410C presence=%d", now);
//         }
//         vTaskDelay(pdMS_TO_TICKS(100));
//     }
// }

// /* ============================== app_main ============================== */
// void app_main(void)
// {
//     state_mutex = xSemaphoreCreateMutex();
//     tft_mutex = xSemaphoreCreateMutex();
//     if (!state_mutex || !tft_mutex) {
//         ESP_LOGE(TAG, "Failed to create state mutex - halting");
//         abort();
//     }

//     esp_err_t err = nvs_flash_init();

//     if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         err = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(err);

//     /* Sync Vietnam time over Wi-Fi/NTP. */
//     wifi_time_init();

//     gpio_config_t presence = {
//         .pin_bit_mask = 1ULL << PIN_LD2410_OUT, .mode = GPIO_MODE_INPUT,
//         .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
//         .intr_type = GPIO_INTR_DISABLE,
//     };
//     ESP_ERROR_CHECK(gpio_config(&presence));
//     for (int i = 0; i < 4; ++i) {
//         ESP_ERROR_CHECK(gpio_set_direction(cols[i], GPIO_MODE_OUTPUT));
//         ESP_ERROR_CHECK(gpio_set_level(cols[i], 1));
//         ESP_ERROR_CHECK(gpio_set_direction(rows[i], GPIO_MODE_INPUT));
//         ESP_ERROR_CHECK(gpio_set_pull_mode(rows[i], GPIO_PULLUP_ONLY));
//     }

//     uart_config_t uart_cfg = {
//         .baud_rate = AS608_BAUD, .data_bits = UART_DATA_8_BITS,
//         .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
//         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
//     };
//     ESP_ERROR_CHECK(uart_param_config(AS608_UART, &uart_cfg));
//     ESP_ERROR_CHECK(uart_set_pin(AS608_UART, PIN_AS608_TX, PIN_AS608_RX,
//                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
//     ESP_ERROR_CHECK(uart_driver_install(AS608_UART, 512, 0, 0, NULL, 0));

//     uart_cfg.baud_rate = CAM_BAUD;
//     ESP_ERROR_CHECK(uart_param_config(CAM_UART, &uart_cfg));
//     ESP_ERROR_CHECK(uart_set_pin(CAM_UART, PIN_CAM_TX, PIN_CAM_RX,
//                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
//     ESP_ERROR_CHECK(uart_driver_install(CAM_UART, 512, 0, 0, NULL, 0));

//     rc522_init();
//     tft_init();
//     if (as608_is_online()) {
//         as608_read_capacity();
//         ESP_LOGI(TAG, "AS608 online, template capacity: %u", fp_library_size);
//     } else {
//         ESP_LOGE(TAG, "AS608 not responding - check TX/RX, GND and AS608_BAUD");
//     }

//     xTaskCreate(rfid_task, "rfid", 4096, NULL, 5, NULL);
//     xTaskCreate(fingerprint_task, "finger", 4096, NULL, 5, NULL);
//     xTaskCreate(keypad_task, "keypad", 3072, NULL, 4, NULL);
//     xTaskCreate(cam_task, "cam", 3072, NULL, 5, NULL);
//     xTaskCreate(presence_task, "presence", 2048, NULL, 4, NULL);
//     xTaskCreate(enroll_watchdog_task, "enroll_wd", 2048, NULL, 3, NULL);
//     xTaskCreate(ui_housekeeping_task, "ui_house", 3072, NULL, 3, NULL);

//     ESP_LOGI(TAG, "============================================");
//     ESP_LOGI(TAG, "SYSTEM READY.");
//     ESP_LOGI(TAG, "ADMIN: * | A:switch | B:back/delete | C:select/confirm | #:back to attendance");
//     ESP_LOGI(TAG, "============================================");
// }


#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "nvs_flash.h"

/* Log tag for the main system. Wi-Fi/NTP uses its own TAG in wifi_time.c. */
static const char *TAG = "ACCESS";
#include "wifi_time.h"
#include "google_sheet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

/* ============================== PIN MAP ============================== */
#define RC522_HOST       SPI2_HOST
#define PIN_SCK          18
#define PIN_MOSI         23
#define PIN_MISO         19
#define PIN_RC522_CS     21
#define PIN_AS608_RX     16
#define PIN_AS608_TX     17
#define PIN_CAM_RX       32
#define PIN_CAM_TX       33
#define PIN_LD2410_OUT   35
#define PIN_TFT_CS       15
#define PIN_TFT_DC       2

/* GPIO4/12/13/14 are the scanned lines; GPIO22/25/26/27 are the driven lines. */
static const gpio_num_t rows[] = {4, 12, 13, 14};
static const gpio_num_t cols[] = {22, 25, 26, 27};

#define AS608_UART       UART_NUM_2
#define CAM_UART         UART_NUM_1
#define AS608_BAUD       57600 /* Change to 115200 if you already changed the AS608's baud. */
#define CAM_BAUD         115200

#define TFT_W            128
#define TFT_H            160
#define RFID_UID_LEN     4    /* This build targets 4-byte MIFARE UID cards. */

/* Confirmation codes in the AS608/R30x protocol. */
#define AS608_OK         0x00
#define AS608_NO_FINGER  0x02
#define AS608_NO_MATCH   0x09

/* If enrollment (started with B) is not completed within this time, it is
 * auto-cancelled, so attendance is never locked out forever if an admin
 * walks away mid-enrollment. */
#define ENROLL_TIMEOUT_US (60LL * 1000000LL)
#define ADMIN_IDLE_TIMEOUT_US (10LL * 1000000LL)
#define ATTENDANCE_RESULT_US (1500000LL)
#define RESULT_GREEN 0x07E0
#define RESULT_RED   0xF800

static spi_device_handle_t rc522;
static esp_lcd_panel_io_handle_t tft_io;
static uint16_t tft_frame[TFT_W * TFT_H];
static int64_t last_accept_us;
static int64_t result_until_us;
static bool result_active;
static bool rfid_need_release;
static bool fp_need_release;
static SemaphoreHandle_t tft_mutex;

typedef enum {
    ENROLL_NONE = 0,
    ENROLL_WAIT_RFID,
    ENROLL_WAIT_FINGER_1,
    ENROLL_WAIT_FINGER_REMOVE,
    ENROLL_WAIT_FINGER_2,
    ENROLL_WAIT_VERIFY_REMOVE,
    ENROLL_WAIT_VERIFY,
} enroll_state_t;

/*
 * Mutex protecting the ENTIRE admin/enrollment state block below.
 * keypad_task writes it, rfid_task/fingerprint_task read and write it too -
 * they run on different ESP32 cores, so a plain `volatile` is not enough to
 * keep several related variables consistent with each other (e.g.
 * enroll_emp_id must stay in sync with current_enroll_state).
 */
static SemaphoreHandle_t state_mutex;

static enroll_state_t current_enroll_state = ENROLL_NONE;
static bool admin_authenticated;
/* True while the admin is on the "enter PIN" screen, i.e. after '*' is
 * pressed but before the PIN has been accepted. Without this flag,
 * attendance_ready() would still report "ready" during PIN entry (since
 * admin_authenticated only flips to true *after* the PIN is verified),
 * so a background task could redraw the idle/attendance screen over the
 * PIN entry screen a second or so after '*' was pressed. */
static bool admin_pin_entry_active;
static uint16_t enroll_emp_id;
static uint8_t enroll_rfid_uid[RFID_UID_LEN];
static bool enroll_rfid_pending;
static int64_t enroll_started_us;
/* Read from the sensor at boot; never guess the capacity from a constant. */
static uint16_t fp_library_size = 300;

static char input_buffer[12];
static size_t input_len;

#define LOCK()   xSemaphoreTake(state_mutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(state_mutex)

/*
 * ============================================================================
 * ADMIN MENU UI (used ONLY inside keypad_task, never read/written by any
 * other task) - so these variables do NOT need to sit under state_mutex.
 * admin_authenticated / current_enroll_state (already covered by the mutex
 * above) remain the "source of truth" for rfid_task & fingerprint_task;
 * admin_ui_mode is just a display/navigation layer on top of that, never
 * read by the other two tasks, so it does not break the existing
 * synchronization rules.
 * ============================================================================
 */
typedef enum {
    ADMIN_UI_NONE = 0,      /* Not logged in (entering PIN, or idle screen)   */
    ADMIN_UI_MENU,          /* Logged in, waiting for add/delete selection    */
    ADMIN_UI_ENTER_ADD_ID,  /* Entering the employee ID to ADD                */
    ADMIN_UI_ENTER_DEL_ID,  /* Entering the employee ID to DELETE             */
} admin_ui_mode_t;

/* ============================== TFT ============================== */
static void tft_cmd(uint8_t cmd, const uint8_t *param, size_t len)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(tft_io, cmd, param, len));
}

static void tft_flush(void)
{
    uint8_t x[] = {0, 0, 0, TFT_W - 1};
    uint8_t y[] = {0, 0, 0, TFT_H - 1};
    tft_cmd(0x2A, x, sizeof(x));
    tft_cmd(0x2B, y, sizeof(y));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(tft_io, 0x2C, tft_frame,
                                               sizeof(tft_frame)));
}

static void tft_fill(uint16_t color)
{
    for (size_t i = 0; i < TFT_W * TFT_H; ++i) tft_frame[i] = color;
}

static void tft_box(int x, int y, int w, int h, uint16_t color)
{
    for (int yy = y; yy < y + h && yy < TFT_H; ++yy)
        for (int xx = x; xx < x + w && xx < TFT_W; ++xx)
            if (xx >= 0 && yy >= 0) tft_frame[yy * TFT_W + xx] = color;
}

/* 5x7 font: A-Z, 0-9, ':' and '/'. Display only ever needs plain ASCII. */
static const uint8_t glyph[][5] = {
    {0,0,0,0,0}, {0x1e,0x05,0x05,0x1e,0}, {0x1f,0x15,0x15,0x0a,0},
    {0x0e,0x11,0x11,0x11,0}, {0x1f,0x11,0x11,0x0e,0}, {0x1f,0x15,0x15,0x11,0},
    {0x1f,0x05,0x05,0x01,0}, {0x0e,0x11,0x15,0x1d,0}, {0x1f,0x04,0x04,0x1f,0},
    {0x11,0x1f,0x11,0,0}, {0x08,0x10,0x10,0x0f,0}, {0x1f,0x04,0x0a,0x11,0},
    {0x1f,0x10,0x10,0x10,0}, {0x1f,0x02,0x04,0x02,0x1f}, {0x1f,0x02,0x04,0x1f,0},
    {0x0e,0x11,0x11,0x0e,0}, {0x1f,0x05,0x05,0x02,0}, {0x0e,0x11,0x19,0x1e,0},
    {0x1f,0x05,0x0d,0x12,0}, {0x12,0x15,0x15,0x09,0}, {0x01,0x1f,0x01,0,0},
    {0x0f,0x10,0x10,0x0f,0}, {0x07,0x08,0x10,0x08,0x07}, {0x1f,0x08,0x04,0x08,0x1f},
    {0x1b,0x04,0x04,0x1b,0}, {0x03,0x04,0x18,0x04,0x03}, {0x19,0x15,0x13,0,0},
    {0x0e,0x11,0x11,0x0e,0}, {0x00,0x12,0x1f,0x10,0}, {0x19,0x15,0x15,0x12,0},
    {0x11,0x15,0x15,0x0a,0}, {0x07,0x04,0x04,0x1f,0}, {0x17,0x15,0x15,0x09,0},
    {0x0e,0x15,0x15,0x08,0}, {0x01,0x01,0x1d,0x03,0}, {0x0a,0x15,0x15,0x0a,0},
    {0x02,0x15,0x15,0x0e,0},

    /* ':' - two dots stacked in the MIDDLE column (col index 2), at rows 2
     * and 4. The previous version put a single dot in columns 1 and 3 at
     * the same row, which draws two dots side by side - i.e. it looked
     * like ".." instead of ":". */
    {0x00, 0x00, 0x14, 0x00, 0x00},

    /* '/' - a forward slash must go from bottom-left to top-right: bottom
     * row (row6) on the left column, top row (row0) on the right column.
     * The previous version had the bit pattern of a BACKSLASH (top-left to
     * bottom-right), which is why "24\09\2026" printed with '\' even
     * though the code intended '/'. */
    {0x40, 0x20, 0x08, 0x02, 0x01}
};

static int glyph_index(char c)
{
    if (c >= 'A' && c <= 'Z') return 1 + c - 'A';
    if (c >= '0' && c <= '9') return 27 + c - '0';
    if (c == ':') return 37;
    if (c == '/') return 38;
    return 0;
}

static void tft_text(int x, int y, const char *text, uint16_t color)
{
    for (; *text; ++text, x += 6) {
        const uint8_t *g = glyph[glyph_index(*text)];
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (g[col] & (1U << row)) tft_box(x + col, y + row, 1, 1, color);
    }
}

static void tft_message_color(const char *title, const char *line1,
                              const char *line2, uint16_t color)
{
    if (!tft_io) return;
    if (tft_mutex) xSemaphoreTake(tft_mutex, portMAX_DELAY);
    tft_fill(0x0010);
    tft_box(0, 0, TFT_W, 22, color);
    tft_text(6, 7, title ? title : "", 0xffff);
    tft_text(5, 45, line1 ? line1 : "", color);
    tft_text(5, 65, line2 ? line2 : "", color);
    tft_flush();
    if (tft_mutex) xSemaphoreGive(tft_mutex);
}

static void tft_message(const char *title, const char *line1, const char *line2)
{
    tft_message_color(title, line1, line2, 0x07e0);
}

static void tft_ready_screen(void)
{
    char day_str[12];
    char time_str[16];
    char date_str[16];

    wifi_time_get_display(day_str, sizeof(day_str),
                          time_str, sizeof(time_str),
                          date_str, sizeof(date_str));

    if (tft_mutex) xSemaphoreTake(tft_mutex, portMAX_DELAY);

    tft_fill(0x0010);

    tft_text(5, 40, "SYSTEM READY", 0x07e0);

    /* Line 1: weekday, e.g. "THURSDAY" */
    tft_text(5, 65, day_str, 0xffff);

    /* Line 2: HH : MM : SS */
    tft_text(5, 85, time_str, 0xffff);

    /* Line 3: DD/MM/YYYY */
    tft_text(5, 103, date_str, 0xffff);

    tft_flush();

    if (tft_mutex) xSemaphoreGive(tft_mutex);
}

static void tft_result_screen(const char *method, const char *id, bool success)
{
    char day_str[12];
    char time_str[16];
    char date_str[16];

    wifi_time_get_display(day_str, sizeof(day_str),
                          time_str, sizeof(time_str),
                          date_str, sizeof(date_str));

    if (tft_mutex) xSemaphoreTake(tft_mutex, portMAX_DELAY);
    tft_fill(0x0010);
    tft_box(0, 0, TFT_W, 22, success ? RESULT_GREEN : RESULT_RED);
    tft_text(6, 7, success ? "SUCCESS" : "FAILED", 0xffff);

    if (success) {
        char line1[24];
        snprintf(line1, sizeof(line1), "%s %s", method ? method : "",
                 id ? id : "");
        tft_text(5, 40, line1, RESULT_GREEN);
        tft_text(5, 58, "THANK YOU", RESULT_GREEN);
        tft_text(5, 78, day_str, 0xffff);
        tft_text(5, 96, time_str, 0xffff);
        tft_text(5, 114, date_str, 0xffff);
    } else {
        tft_text(5, 40, "ACCESS DENIED", RESULT_RED);
        tft_text(5, 58, "PLEASE TRY AGAIN", RESULT_RED);
        tft_text(5, 78, day_str, 0xffff);
        tft_text(5, 96, time_str, 0xffff);
        tft_text(5, 114, date_str, 0xffff);
    }
    tft_flush();
    if (tft_mutex) xSemaphoreGive(tft_mutex);
}

/*
 * "Input" screen shared by: entering the admin PIN, entering the ID to add,
 * entering the ID to delete. Shows the value being typed directly inside a
 * highlighted box for easy monitoring/proofreading, with a key-hint line
 * below. ADDED ONLY - does not touch tft_message() / the TFT functions above.
 */
static void tft_input_screen(const char *title, const char *prompt,
                              const char *value, const char *hint1,
                              const char *hint2)
{
    if (!tft_io) return;
    tft_fill(0x0010);
    tft_box(0, 0, TFT_W, 22, 0x03E0);           /* title bar */
    tft_text(6, 7, title, 0xffff);

    tft_text(8, 34, prompt, 0xffe0);

    /* highlighted input box */
    tft_box(6, 52, TFT_W - 12, 18, 0x0000);
    tft_box(6, 52, TFT_W - 12, 1, 0x07e0);
    tft_box(6, 69, TFT_W - 12, 1, 0x07e0);
    tft_text(10, 57, (value && *value) ? value : "-", 0x07e0);

    tft_text(8, 100, hint1 ? hint1 : "", 0x07ff);
    tft_text(8, 116, hint2 ? hint2 : "", 0x07ff);
    tft_flush();
}

static void tft_init(void)
{
    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = PIN_TFT_CS, .dc_gpio_num = PIN_TFT_DC,
        .spi_mode = 0, .pclk_hz = 20 * 1000 * 1000, .trans_queue_depth = 1,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)RC522_HOST,
                                               &io, &tft_io));
    tft_cmd(0x01, NULL, 0); vTaskDelay(pdMS_TO_TICKS(150));
    tft_cmd(0x11, NULL, 0); vTaskDelay(pdMS_TO_TICKS(120));
    uint8_t madctl = 0xc8, colmod = 0x05;
    tft_cmd(0x36, &madctl, 1); tft_cmd(0x3a, &colmod, 1);
    tft_cmd(0x13, NULL, 0); tft_cmd(0x29, NULL, 0);
    tft_ready_screen();
}

/* ============================== NVS / access ============================== */
static void rfid_key(const uint8_t uid[RFID_UID_LEN], char key[16])
{
    snprintf(key, 16, "u%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
}

static esp_err_t save_rfid_to_nvs(const uint8_t uid[RFID_UID_LEN], uint16_t emp_id)
{
    char key[16]; rfid_key(uid, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open("users", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, key, emp_id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t erase_rfid_from_nvs(const uint8_t uid[RFID_UID_LEN])
{
    char key[16]; rfid_key(uid, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open("users", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static uint16_t check_rfid_in_nvs(const uint8_t uid[RFID_UID_LEN])
{
    char key[16]; rfid_key(uid, key);
    uint16_t emp_id = 0;
    nvs_handle_t h;
    if (nvs_open("users", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u16(h, key, &emp_id);
        nvs_close(h);
    }
    return emp_id;
}

/*
 * Reverse-lookup the RFID UID for a given emp_id (the "users" namespace
 * stores key=UID, value=emp_id, so the whole namespace must be scanned).
 * Used when deleting an employee: we only have the ID typed on the keypad
 * and need to find the matching RFID card to remove it completely.
 * Does not touch the NVS functions above - read only.
 */
static bool find_uid_by_emp_id(uint16_t emp_id, uint8_t uid_out[RFID_UID_LEN])
{
    bool found = false;
    nvs_handle_t h;
    if (nvs_open("users", NVS_READONLY, &h) != ESP_OK) return false;

    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, "users", NVS_TYPE_U16, &it);
    while (res == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        uint16_t value = 0;
        if (nvs_get_u16(h, info.key, &value) == ESP_OK && value == emp_id) {
            unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0;
            if (sscanf(info.key, "u%02x%02x%02x%02x", &b0, &b1, &b2, &b3) == 4) {
                uid_out[0] = (uint8_t)b0; uid_out[1] = (uint8_t)b1;
                uid_out[2] = (uint8_t)b2; uid_out[3] = (uint8_t)b3;
                found = true;
            }
            break;
        }
        res = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    nvs_close(h);
    return found;
}

static bool nvs_pin_ok(const char *pin)
{
    char saved[12] = "1234";
    size_t n = sizeof(saved);
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_str(h, "master", saved, &n);
        nvs_close(h);
    }
    return strcmp(pin, saved) == 0;
}

static void log_event(const char *method, const char *id)
{
    nvs_handle_t h;
    if (nvs_open("logs", NVS_READWRITE, &h) == ESP_OK) {
        char value[64];
        snprintf(value, sizeof(value), "%s:%s:%" PRId64,
                 method, id, esp_timer_get_time() / 1000000);
        (void)nvs_set_str(h, "latest", value);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

/* Read both flags under lock for a consistent snapshot. */
static bool attendance_ready(void)
{
    LOCK();
    bool ready = !admin_authenticated &&
                 !admin_pin_entry_active &&
                 current_enroll_state == ENROLL_NONE &&
                 !result_active;
    UNLOCK();
    return ready;
}

static void grant(const char *method, const char *id)
{
    int64_t now = esp_timer_get_time();

    LOCK();
    bool ready = !admin_authenticated &&
                 !admin_pin_entry_active &&
                 current_enroll_state == ENROLL_NONE &&
                 !result_active;
    if (!ready || now - last_accept_us < ATTENDANCE_RESULT_US) {
        UNLOCK();
        return;
    }
    last_accept_us = now;
    result_active = true;
    result_until_us = now + ATTENDANCE_RESULT_US;
    if (method && strcmp(method, "RFID") == 0) rfid_need_release = true;
    if (method && strcmp(method, "FINGER") == 0) fp_need_release = true;
    UNLOCK();

    ESP_LOGI(TAG, "=== AUTHENTICATION SUCCESSFUL! %s, ID: %s ===", method, id);
    tft_result_screen(method, id, true);

    /*
     * Queue the record for Google Sheets. This is non-blocking and does
     * NOT perform any network I/O on this task's stack (see google_sheet.c
     * for why that distinction matters) - it is therefore safe to call
     * here even though grant() runs on rfid_task/fingerprint_task/cam_task,
     * all of which have small stacks. The actual HTTPS request happens
     * later, on a dedicated background task with its own 8 KB stack.
     */
    google_sheet_log_attendance(id, method, "ESP32-GATE-1");
}

/* ============================== RC522 ============================== */
static uint8_t rcr(uint8_t reg)
{
    uint8_t tx[2] = {(uint8_t)((reg << 1) | 0x80), 0}, rx[2] = {0};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx, .rx_buffer = rx};
    if (spi_device_transmit(rc522, &t) != ESP_OK) return 0;
    return rx[1];
}

static void wcr(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {(uint8_t)(reg << 1), value};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx};
    ESP_ERROR_CHECK(spi_device_transmit(rc522, &t));
}

static void rset(uint8_t reg, uint8_t mask) { wcr(reg, rcr(reg) | mask); }
static void rclr(uint8_t reg, uint8_t mask) { wcr(reg, rcr(reg) & ~mask); }

static void rc522_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCK,
        .max_transfer_sz = TFT_W * TFT_H * 2,
    };
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 1000000, .mode = 0, .spics_io_num = PIN_RC522_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(RC522_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(RC522_HOST, &dev, &rc522));
    wcr(0x01, 0x0f);       /* soft reset */
    vTaskDelay(pdMS_TO_TICKS(50));
    wcr(0x2a, 0x8d); wcr(0x2b, 0x3e); /* timer */
    wcr(0x2d, 30); wcr(0x2c, 0);
    wcr(0x15, 0x40);       /* ModeReg: CRC preset 0x6363 */
    rset(0x14, 0x03);      /* TxControlReg: turn on antenna */
}

static int rc522_xfer_bits(const uint8_t *send, int slen, uint8_t *back,
                            int *blen, uint8_t tx_last_bits)
{
    wcr(0x01, 0x00);       /* CommandReg: Idle */
    wcr(0x02, 0x77);       /* ComIEnReg */
    wcr(0x04, 0x7f);       /* ComIrqReg: clear old flags */
    rclr(0x0a, 0x80);      /* FIFOLevelReg: FlushBuffer */
    for (int i = 0; i < slen; ++i) wcr(0x09, send[i]);
    wcr(0x0d, tx_last_bits & 0x07); /* BitFramingReg */
    wcr(0x01, 0x0c);       /* Transceive */
    rset(0x0d, 0x80);      /* StartSend */

    const int64_t deadline = esp_timer_get_time() + 25000;
    uint8_t irq = 0;
    do {
        irq = rcr(0x04);
        if (irq & 0x01) break; /* TimerIRq */
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (!(irq & 0x30) && esp_timer_get_time() < deadline);
    rclr(0x0d, 0x80);

    if (!(irq & 0x30) || (rcr(0x06) & 0x1b)) return -1; /* ErrorReg */
    int count = rcr(0x0a);
    if (count > *blen) return -1;
    for (int i = 0; i < count; ++i) back[i] = rcr(0x09);
    *blen = count;
    return count ? 0 : -1;
}

static int rc522_xfer(const uint8_t *send, int slen, uint8_t *back, int *blen)
{
    return rc522_xfer_bits(send, slen, back, blen, 0);
}

static bool rc522_poll(uint8_t uid[RFID_UID_LEN])
{
    uint8_t answer[18];
    int n = sizeof(answer);
    const uint8_t request[] = {0x26}; /* REQA must be a 7-bit frame */
    if (rc522_xfer_bits(request, sizeof(request), answer, &n, 7) != 0 || n != 2)
        return false;

    const uint8_t anticollision[] = {0x93, 0x20};
    n = sizeof(answer);
    if (rc522_xfer(anticollision, sizeof(anticollision), answer, &n) != 0 || n < 5)
        return false;
    if ((uint8_t)(answer[0] ^ answer[1] ^ answer[2] ^ answer[3]) != answer[4])
        return false;
    memcpy(uid, answer, RFID_UID_LEN);
    return true;
}

/* ============================== AS608 ============================== */
/*
 * Returns ESP_OK when the UART packet is valid. The AS608's own
 * success/failure code lives in *status; every I/O error must NOT be
 * turned into "no finger present".
 */
static esp_err_t as608_exec(uint8_t cmd, const uint8_t *data, size_t len,
                             uint8_t *reply, size_t *rlen, uint8_t *status)
{
    if (len > 20 || !reply || !rlen || !status) return ESP_ERR_INVALID_ARG;
    uint8_t packet[32] = {0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01,
                          (uint8_t)((len + 3) >> 8), (uint8_t)(len + 3), cmd};
    uint16_t sum = 1 + packet[7] + packet[8] + cmd;
    for (size_t i = 0; i < len; ++i) { packet[10 + i] = data[i]; sum += data[i]; }
    packet[10 + len] = (uint8_t)(sum >> 8);
    packet[11 + len] = (uint8_t)sum;

    uart_flush_input(AS608_UART);
    if (uart_write_bytes(AS608_UART, (const char *)packet, 12 + len) < 0)
        return ESP_FAIL;
    uint8_t header[9];
    if (uart_read_bytes(AS608_UART, header, sizeof(header), pdMS_TO_TICKS(800)) != sizeof(header) ||
        header[0] != 0xef || header[1] != 0x01 || header[6] != 0x07)
        return ESP_ERR_INVALID_RESPONSE;

    int body_len = ((header[7] << 8) | header[8]) - 2; /* confirmation + parameters */
    /*
     * IMPORTANT FIX: the uart_read_bytes call below reads "body_len + 2"
     * bytes (the extra 2 are the checksum) into `reply`. The previous
     * check only compared body_len > *rlen, so when body_len == *rlen
     * (e.g. == 32, exactly sizeof(reply[32])) the read would write 34
     * bytes into an array that only holds 32 -> stack overflow. A noisy
     * UART frame (loose wire, electrical noise) can absolutely produce a
     * body_len value that close to the boundary before the checksum
     * below gets checked.
     */
    if (body_len < 1 || body_len + 2 > (int)*rlen) return ESP_ERR_INVALID_SIZE;
    if (uart_read_bytes(AS608_UART, reply, body_len + 2, pdMS_TO_TICKS(500)) != body_len + 2)
        return ESP_ERR_TIMEOUT;

    uint16_t checksum = 0;
    for (int i = 6; i < 9; ++i) checksum += header[i];
    for (int i = 0; i < body_len; ++i) checksum += reply[i];
    if (checksum != (uint16_t)((reply[body_len] << 8) | reply[body_len + 1]))
        return ESP_ERR_INVALID_CRC;
    *rlen = body_len;
    *status = reply[0];
    return ESP_OK;
}

static bool as608_cmd(uint8_t cmd, const uint8_t *data, size_t len,
                      uint8_t *reply, size_t *rlen)
{
    uint8_t status;
    return as608_exec(cmd, data, len, reply, rlen, &status) == ESP_OK && status == AS608_OK;
}

static bool as608_is_online(void)
{
    uint8_t reply[32];
    size_t n = sizeof(reply);
    return as608_cmd(0x0f, NULL, 0, reply, &n); /* ReadSysPara */
}

static void as608_read_capacity(void)
{
    uint8_t reply[32], status;
    size_t n = sizeof(reply);
    if (as608_exec(0x0f, NULL, 0, reply, &n, &status) == ESP_OK &&
        status == AS608_OK && n >= 17) {
        uint16_t capacity = ((uint16_t)reply[5] << 8) | reply[6];
        if (capacity > 0) fp_library_size = capacity;
    }
}

/* Search must scan exactly the sensor's own reported library size. */
static bool as608_search(uint16_t *matched_id, uint16_t *score, uint8_t *status)
{
    uint8_t reply[32];
    const uint8_t request[] = {1, 0, 0,
                               (uint8_t)(fp_library_size >> 8),
                               (uint8_t)fp_library_size};
    size_t n = sizeof(reply);
    *matched_id = 0;
    *score = 0;
    *status = 0xff;
    esp_err_t err = as608_exec(0x04, request, sizeof(request), reply, &n, status);
    if (err != ESP_OK || *status != AS608_OK || n < 5) return false;
    *matched_id = ((uint16_t)reply[1] << 8) | reply[2];
    *score = ((uint16_t)reply[3] << 8) | reply[4];
    return true;
}

static void as608_delete_template(uint16_t id)
{
    uint8_t reply[16];
    uint8_t request[] = {(uint8_t)(id >> 8), (uint8_t)id, 0, 1};
    size_t n = sizeof(reply);
    if (!as608_cmd(0x0c, request, sizeof(request), reply, &n))
        ESP_LOGE(TAG, "Failed to delete template, ID %u", id);
}

/*
 * Fully deletes one employee - fingerprint (AS608) + RFID (NVS), so the old
 * RFID card becomes a completely "blank" card again (no longer mapped to
 * any ID). Only called from keypad_task, after the admin has already
 * authenticated - does not touch current_enroll_state/admin_authenticated,
 * so it does not need state_mutex; the helper functions
 * (as608_delete_template/erase_rfid_from_nvs) are already called unlocked
 * elsewhere in this same pattern.
 * Returns true if a matching RFID record for emp_id was found and removed.
 */
static bool delete_employee(uint16_t emp_id)
{
    uint8_t uid[RFID_UID_LEN];
    bool had_rfid = find_uid_by_emp_id(emp_id, uid);

    as608_delete_template(emp_id); /* safe even if this ID never had a fingerprint */

    if (had_rfid) {
        (void)erase_rfid_from_nvs(uid);
    }

    char id_str[8];
    snprintf(id_str, sizeof(id_str), "%u", emp_id);
    log_event("DEL", id_str);
    return had_rfid;
}

/* ============================== keypad ============================== */
static int64_t last_press_us;
static char last_key;
#define DEBOUNCE_US 150000

static char keypad_scan(void)
{
    static const char keymap[4][4] = {
        {'1','2','3','A'}, {'4','5','6','B'}, {'7','8','9','C'}, {'*','0','#','D'}
    };
    char pressed = 0;
    for (int c = 0; c < 4 && !pressed; ++c) {
        for (int k = 0; k < 4; ++k) gpio_set_level(cols[k], k == c ? 0 : 1);
        esp_rom_delay_us(20);
        for (int r = 0; r < 4; ++r)
            if (gpio_get_level(rows[r]) == 0) { pressed = keymap[c][r]; break; }
    }
    for (int k = 0; k < 4; ++k) gpio_set_level(cols[k], 1);
    if (!pressed) { last_key = 0; return 0; }
    int64_t now = esp_timer_get_time();
    if (pressed == last_key && now - last_press_us < DEBOUNCE_US) return 0;
    last_key = pressed; last_press_us = now;
    return pressed;
}

/* Must be called while state_mutex is already held. */
static void cancel_admin_or_enrollment_locked(void)
{
    /* If Store already ran but verification has not happened yet, do not
     * leave half-finished data behind. The AS608/NVS calls below do not
     * need the mutex held, but we snapshot the values we need before
     * leaving the locked region for the slow I/O calls; here
     * as608_delete_template/erase_rfid_from_nvs do not touch shared state
     * so calling them directly is safe. */
    bool need_rollback = enroll_rfid_pending &&
        (current_enroll_state == ENROLL_WAIT_VERIFY_REMOVE ||
         current_enroll_state == ENROLL_WAIT_VERIFY);
    uint16_t rollback_id = enroll_emp_id;
    uint8_t rollback_uid[RFID_UID_LEN];
    memcpy(rollback_uid, enroll_rfid_uid, RFID_UID_LEN);

    admin_authenticated = false;
    admin_pin_entry_active = false;
    current_enroll_state = ENROLL_NONE;
    enroll_rfid_pending = false;
    enroll_emp_id = 0;
    input_len = 0;

    if (need_rollback) {
        UNLOCK();
        as608_delete_template(rollback_id);
        (void)erase_rfid_from_nvs(rollback_uid);
        LOCK();
    }
}

static void cancel_enrollment_keep_admin(void)
{
    LOCK();
    bool need_rollback = enroll_rfid_pending &&
        (current_enroll_state == ENROLL_WAIT_VERIFY_REMOVE ||
         current_enroll_state == ENROLL_WAIT_VERIFY);
    uint16_t rollback_id = enroll_emp_id;
    uint8_t rollback_uid[RFID_UID_LEN];
    memcpy(rollback_uid, enroll_rfid_uid, RFID_UID_LEN);

    current_enroll_state = ENROLL_NONE;
    enroll_rfid_pending = false;
    enroll_emp_id = 0;
    input_len = 0;
    UNLOCK();

    if (need_rollback) {
        as608_delete_template(rollback_id);
        (void)erase_rfid_from_nvs(rollback_uid);
    }
}

static void cancel_admin_or_enrollment(void)
{
    LOCK();
    cancel_admin_or_enrollment_locked();
    UNLOCK();
    tft_ready_screen();
}

/* ---- Admin screens (display only - does not touch the original logic) ---- */
static void ui_show_pin_entry(void)
{
    char shown[12];
    size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
    memcpy(shown, input_buffer, n); shown[n] = 0;
    tft_input_screen("ADMIN LOGIN", "PIN CODE:", shown,
                     "B:DEL  C:CONFIRM", "#:BACK");
}

static void ui_show_menu(bool delete_selected)
{
    tft_input_screen("ADMIN", "SELECT ACTION",
                     delete_selected ? "DELETE EMPLOYEE" : "ADD EMPLOYEE",
                     "A:SWITCH  B:BACK", "C:SELECT");
}

static void ui_show_add_id_entry(void)
{
    char shown[12];
    size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
    memcpy(shown, input_buffer, n); shown[n] = 0;
    tft_input_screen("ADD EMPLOYEE", "ENTER ID:", shown,
                     "B:DEL  C:CONFIRM", "#:BACK");
}

static void ui_show_del_id_entry(void)
{
    char shown[12];
    size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
    memcpy(shown, input_buffer, n); shown[n] = 0;
    tft_input_screen("DELETE EMPLOYEE", "ENTER ID:", shown,
                     "B:DEL  C:CONFIRM", "#:BACK");
}


static void keypad_task(void *arg)
{
    bool entering_master = false;
    admin_ui_mode_t admin_ui_mode = ADMIN_UI_NONE;
    bool delete_selected = false;
    int64_t last_ui_action_us = esp_timer_get_time();

    while (true) {
        char key = keypad_scan();

        LOCK();
        bool is_admin = admin_authenticated;
        enroll_state_t enroll_now = current_enroll_state;
        UNLOCK();

        if (is_admin && esp_timer_get_time() - last_ui_action_us >=
                        ADMIN_IDLE_TIMEOUT_US) {
            ESP_LOGI(TAG, "Admin idle for 10s -> back to attendance mode");
            cancel_admin_or_enrollment();
            entering_master = false;
            admin_ui_mode = ADMIN_UI_NONE;
            delete_selected = false;
            last_ui_action_us = esp_timer_get_time();
            continue;
        }

        if (!key) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last_ui_action_us = esp_timer_get_time();

        LOCK();
        is_admin = admin_authenticated;
        enroll_now = current_enroll_state;
        UNLOCK();

        /* # = immediately cancel whatever is in progress and go back to
         * the attendance screen. */
        if (key == '#') {
            cancel_admin_or_enrollment();
            entering_master = false;
            admin_ui_mode = ADMIN_UI_NONE;
            delete_selected = false;
            last_ui_action_us = esp_timer_get_time();
            continue;
        }

        /* * = enter admin mode. */
        if (key == '*') {
            cancel_admin_or_enrollment();
            LOCK();
            admin_pin_entry_active = true;
            UNLOCK();
            entering_master = true;
            admin_ui_mode = ADMIN_UI_NONE;
            delete_selected = false;
            input_len = 0;
            ui_show_pin_entry();
            continue;
        }

        if (entering_master) {
            if (key == 'B') {
                if (input_len > 0) {
                    input_len--;
                    ui_show_pin_entry();
                }
            } else if (key == 'C') {
                input_buffer[input_len] = 0;
                if (nvs_pin_ok(input_buffer)) {
                    LOCK();
                    admin_authenticated = true;
                    admin_pin_entry_active = false;
                    UNLOCK();
                    entering_master = false;
                    input_len = 0;
                    admin_ui_mode = ADMIN_UI_MENU;
                    delete_selected = false;
                    ui_show_menu(false);
                } else {
                    input_len = 0;
                    tft_message_color("FAILED", "WRONG PIN",
                                      "RETRY", RESULT_RED);
                    vTaskDelay(pdMS_TO_TICKS(700));
                    ui_show_pin_entry();
                }
            } else if (key >= '0' && key <= '9' &&
                       input_len < sizeof(input_buffer) - 1) {
                input_buffer[input_len++] = key;
                ui_show_pin_entry();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!is_admin) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Currently enrolling: B goes back to the admin menu, only # exits
         * all the way back to attendance mode. */
        if (enroll_now != ENROLL_NONE) {
            if (key == 'B') {
                cancel_enrollment_keep_admin();
                admin_ui_mode = ADMIN_UI_MENU;
                delete_selected = false;
                ui_show_menu(false);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (admin_ui_mode == ADMIN_UI_MENU) {
            if (key == 'A') {
                delete_selected = !delete_selected;
                ui_show_menu(delete_selected);
            } else if (key == 'B') {
                cancel_admin_or_enrollment();
                admin_ui_mode = ADMIN_UI_NONE;
                delete_selected = false;
                tft_ready_screen();
            } else if (key == 'C') {
                input_len = 0;
                if (delete_selected) {
                    admin_ui_mode = ADMIN_UI_ENTER_DEL_ID;
                    ui_show_del_id_entry();
                } else {
                    admin_ui_mode = ADMIN_UI_ENTER_ADD_ID;
                    ui_show_add_id_entry();
                }
            }
        } else if (admin_ui_mode == ADMIN_UI_ENTER_ADD_ID) {
            if (key == 'B') {
                if (input_len > 0) {
                    input_len--;
                    ui_show_add_id_entry();
                } else {
                    admin_ui_mode = ADMIN_UI_MENU;
                    ui_show_menu(false);
                }
            } else if (key == 'C') {
                input_buffer[input_len] = 0;
                long id = strtol(input_buffer, NULL, 10);

                if (id > 0 && id < fp_library_size) {
                    LOCK();
                    enroll_emp_id = (uint16_t)id;
                    enroll_rfid_pending = false;
                    current_enroll_state = ENROLL_WAIT_RFID;
                    enroll_started_us = esp_timer_get_time();
                    UNLOCK();

                    input_len = 0;
                    admin_ui_mode = ADMIN_UI_MENU;
                    tft_message("ADD EMPLOYEE", "SCAN RFID CARD",
                                "B:BACK");
                } else {
                    tft_message_color("FAILED", "INVALID ID",
                                       "RE-ENTER", RESULT_RED);
                    vTaskDelay(pdMS_TO_TICKS(700));
                    ui_show_add_id_entry();
                }
            } else if (key >= '0' && key <= '9' &&
                       input_len < sizeof(input_buffer) - 1) {
                input_buffer[input_len++] = key;
                ui_show_add_id_entry();
            }
        } else if (admin_ui_mode == ADMIN_UI_ENTER_DEL_ID) {
            if (key == 'B') {
                if (input_len > 0) {
                    input_len--;
                    ui_show_del_id_entry();
                } else {
                    admin_ui_mode = ADMIN_UI_MENU;
                    ui_show_menu(true);
                }
            } else if (key == 'C') {
                input_buffer[input_len] = 0;
                long id = strtol(input_buffer, NULL, 10);

                if (id > 0 && id < fp_library_size) {
                    bool existed = delete_employee((uint16_t)id);
                    char id_str[12];
                    snprintf(id_str, sizeof(id_str), "ID %ld", id);

                    if (existed)
                        tft_message("DELETE OK", id_str, "RFID & FP REMOVED");
                    else
                        tft_message("DELETE OK", id_str, "NO RFID FOUND");

                    vTaskDelay(pdMS_TO_TICKS(1200));
                    admin_ui_mode = ADMIN_UI_MENU;
                    delete_selected = false;
                    ui_show_menu(false);
                } else {
                    tft_message_color("FAILED", "INVALID ID",
                                       "RE-ENTER", RESULT_RED);
                    vTaskDelay(pdMS_TO_TICKS(700));
                    ui_show_del_id_entry();
                }
            } else if (key >= '0' && key <= '9' &&
                       input_len < sizeof(input_buffer) - 1) {
                input_buffer[input_len++] = key;
                ui_show_del_id_entry();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Background task: if the admin presses B and then walks away without
 * finishing (never presses D), the system automatically exits the enroll
 * state after ENROLL_TIMEOUT_US instead of locking attendance forever. */
static void enroll_watchdog_task(void *arg)
{
    while (true) {
        LOCK();
        bool expired = current_enroll_state != ENROLL_NONE &&
            (esp_timer_get_time() - enroll_started_us) > ENROLL_TIMEOUT_US;
        UNLOCK();
        if (expired) {
            ESP_LOGW(TAG, "Enrollment timed out, auto-cancelled");
            cancel_admin_or_enrollment();
            tft_message_color("FAILED", "ENROLLMENT CANCELLED", "READY", RESULT_RED);
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (attendance_ready()) tft_ready_screen();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void ui_housekeeping_task(void *arg)
{
    int64_t last_clock_refresh = 0;

    while (true) {
        int64_t now = esp_timer_get_time();

        LOCK();
        bool active = result_active;
        int64_t until = result_until_us;
        UNLOCK();

        if (active && now >= until) {
            LOCK();
            result_active = false;
            UNLOCK();
            if (attendance_ready()) tft_ready_screen();
        }

        if (!active && attendance_ready() &&
            now - last_clock_refresh >= 1000000LL) {
            last_clock_refresh = now;
            tft_ready_screen();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ============================== Tasks ============================== */
static void rfid_task(void *arg)
{
    uint8_t uid[RFID_UID_LEN];
    char id[12];
    while (true) {
        bool card_present = rc522_poll(uid);
        if (!card_present) {
            LOCK();
            if (rfid_need_release) rfid_need_release = false;
            UNLOCK();
        } else {
            LOCK();
            bool in_enroll_wait_rfid = (current_enroll_state == ENROLL_WAIT_RFID) &&
                                        admin_authenticated;
            uint16_t enroll_id_snapshot = enroll_emp_id;
            UNLOCK();

            if (in_enroll_wait_rfid) {
                uint16_t existing = check_rfid_in_nvs(uid);
                if (existing != 0 && existing != enroll_id_snapshot) {
                    ESP_LOGW(TAG, "RFID card already belongs to ID %u", existing);
                    tft_message("CARD EXISTS", "SCAN OTHER CARD", "B:CANCEL");
                } else if (!as608_is_online()) {
                    /* Do not save the card if the fingerprint sensor is offline. */
                    ESP_LOGE(TAG, "AS608 not responding; RFID not saved");
                    tft_message("FINGERPRINT ERROR", "CHECK AS608", "SCAN CARD AGAIN");
                } else {
                    LOCK();
                    /* Re-check the state is still correct after the slow I/O
                     * above (avoid overwriting if the admin already pressed
                     * B or the watchdog already cancelled while we waited). */
                    if (current_enroll_state == ENROLL_WAIT_RFID &&
                        enroll_emp_id == enroll_id_snapshot) {
                        memcpy(enroll_rfid_uid, uid, sizeof(uid));
                        enroll_rfid_pending = true;
                        current_enroll_state = ENROLL_WAIT_FINGER_1;
                        enroll_started_us = esp_timer_get_time();
                        UNLOCK();
                        ESP_LOGI(TAG, "RFID OK; AS608 OK. Place finger 1st time");
                        tft_message("CARD OK", "PLACE FINGER", "STEP 1");
                    } else {
                        UNLOCK();
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(800));
            } else if (attendance_ready()) {
                LOCK();
                bool blocked = rfid_need_release;
                UNLOCK();
                if (blocked) {
                    /* Only unlock RFID once the card has been pulled away. */
                    continue;
                }
                uint16_t emp = check_rfid_in_nvs(uid);
                if (emp) {
                    snprintf(id, sizeof(id), "%u", emp);
                    log_event("RFID", id); grant("RFID", id);
                } else {
                    ESP_LOGW(TAG, "Unregistered RFID card");
                    tft_result_screen("RFID", "", false);
                }
                vTaskDelay(pdMS_TO_TICKS(800));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static void fingerprint_task(void *arg)
{
    uint8_t reply[32];
    while (true) {
        size_t n = sizeof(reply);

        LOCK();
        enroll_state_t st = current_enroll_state;
        uint16_t emp_id_snapshot = enroll_emp_id;
        UNLOCK();

        if (st == ENROLL_WAIT_FINGER_1) {
            if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 1; n = sizeof(reply);
                if (as608_cmd(0x02, &buffer, 1, reply, &n)) {
                    LOCK();
                    if (current_enroll_state == ENROLL_WAIT_FINGER_1) {
                        current_enroll_state = ENROLL_WAIT_FINGER_REMOVE;
                        enroll_started_us = esp_timer_get_time();
                    }
                    UNLOCK();
                    ESP_LOGI(TAG, "Fingerprint 1 OK; lift finger");
                    tft_message("FINGER OK", "LIFT FINGER", "WAIT FOR STEP 2");
                }
            }
        } else if (st == ENROLL_WAIT_FINGER_REMOVE) {
            /* Only status 0x02 really means the finger has been lifted. */
            uint8_t status;
            esp_err_t err = as608_exec(0x01, NULL, 0, reply, &n, &status);
            if (err == ESP_OK && status == AS608_NO_FINGER) {
                LOCK();
                if (current_enroll_state == ENROLL_WAIT_FINGER_REMOVE) {
                    current_enroll_state = ENROLL_WAIT_FINGER_2;
                    enroll_started_us = esp_timer_get_time();
                }
                UNLOCK();
                tft_message("PLACE AGAIN", "SAME FINGER", "STEP 2");
                vTaskDelay(pdMS_TO_TICKS(400));
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "AS608 UART error while waiting for lift: %s", esp_err_to_name(err));
            }
        } else if (st == ENROLL_WAIT_FINGER_2) {
            if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 2; n = sizeof(reply);
                if (!as608_cmd(0x02, &buffer, 1, reply, &n)) goto next;
                n = sizeof(reply);
                if (!as608_cmd(0x05, NULL, 0, reply, &n)) {
                    ESP_LOGW(TAG, "The two fingerprint scans do not match");
                    LOCK();
                    if (current_enroll_state == ENROLL_WAIT_FINGER_2) {
                        current_enroll_state = ENROLL_WAIT_FINGER_1;
                        enroll_started_us = esp_timer_get_time();
                    }
                    UNLOCK();
                    tft_message("MISMATCH", "RESTART STEP 1", "RETRY");
                    goto next;
                }
                uint8_t store[] = {1, (uint8_t)(emp_id_snapshot >> 8), (uint8_t)emp_id_snapshot};
                n = sizeof(reply);
                if (!as608_cmd(0x06, store, sizeof(store), reply, &n)) {
                    ESP_LOGE(TAG, "Failed to store AS608 template");
                    tft_message("SAVE ERROR", "RETRY", "B:CANCEL");
                    goto next;
                }

                bool save_ok;
                LOCK();
                save_ok = enroll_rfid_pending &&
                          save_rfid_to_nvs(enroll_rfid_uid, emp_id_snapshot) == ESP_OK;
                if (save_ok && current_enroll_state == ENROLL_WAIT_FINGER_2) {
                    /* Store only confirms the data was written to flash, not
                     * that Search can actually find it. Require lifting the
                     * finger and scanning a 3rd time to verify for real. */
                    current_enroll_state = ENROLL_WAIT_VERIFY_REMOVE;
                    enroll_started_us = esp_timer_get_time();
                } else if (!save_ok) {
                    ESP_LOGE(TAG, "Template saved but RFID not written to NVS");
                    enroll_rfid_pending = false;
                    current_enroll_state = ENROLL_NONE;
                    admin_authenticated = false;
                }
                UNLOCK();

                if (save_ok) {
                    tft_message("TEMPLATE SAVED", "LIFT FINGER", "VERIFYING NEXT");
                } else {
                    tft_message("NVS ERROR", "CARD NOT SAVED", "CHECK NVS");
                }
            }
        } else if (st == ENROLL_WAIT_VERIFY_REMOVE) {
            uint8_t status;
            esp_err_t err = as608_exec(0x01, NULL, 0, reply, &n, &status);
            if (err == ESP_OK && status == AS608_NO_FINGER) {
                LOCK();
                if (current_enroll_state == ENROLL_WAIT_VERIFY_REMOVE) {
                    current_enroll_state = ENROLL_WAIT_VERIFY;
                    enroll_started_us = esp_timer_get_time();
                }
                UNLOCK();
                tft_message("VERIFY", "PLACE FINGER AGAIN", "STEP 3");
                vTaskDelay(pdMS_TO_TICKS(400));
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "AS608 UART error while waiting for verify: %s", esp_err_to_name(err));
            }
        } else if (st == ENROLL_WAIT_VERIFY) {
            if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 1; n = sizeof(reply);
                if (!as608_cmd(0x02, &buffer, 1, reply, &n)) {
                    ESP_LOGW(TAG, "Failed to extract features for verification");
                    goto next;
                }
                uint16_t matched, score;
                uint8_t status;
                bool verify_ok = as608_search(&matched, &score, &status) &&
                                  matched == emp_id_snapshot;

                if (verify_ok) {
                    LOCK();
                    if (current_enroll_state == ENROLL_WAIT_VERIFY) {
                        enroll_rfid_pending = false;
                        current_enroll_state = ENROLL_NONE;
                        admin_authenticated = false;
                    }
                    UNLOCK();
                    ESP_LOGI(TAG, "VERIFICATION COMPLETE: ID %u, score %u", matched, score);
                    tft_message("ADD EMPLOYEE", "VERIFIED OK", "READY");
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    tft_ready_screen();
                } else {
                    /* Never leave behind an account that can enroll but
                     * cannot actually authenticate. */
                    ESP_LOGE(TAG, "Verification failed: status=0x%02X, match=%u, expected=%u",
                             status, matched, emp_id_snapshot);
                    uint8_t uid_snapshot[RFID_UID_LEN];
                    LOCK();
                    memcpy(uid_snapshot, enroll_rfid_uid, RFID_UID_LEN);
                    enroll_rfid_pending = false;
                    current_enroll_state = ENROLL_NONE;
                    admin_authenticated = false;
                    UNLOCK();
                    as608_delete_template(emp_id_snapshot);
                    (void)erase_rfid_from_nvs(uid_snapshot);
                    tft_message_color("FAILED", "DATA ROLLED BACK", "RETRY", RESULT_RED);
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    if (attendance_ready()) tft_ready_screen();
                }
            }
        } else if (attendance_ready()) {
            LOCK();
            bool blocked = fp_need_release;
            UNLOCK();

            if (blocked) {
                uint8_t release_status = 0xff;
                size_t rn = sizeof(reply);
                esp_err_t re = as608_exec(0x01, NULL, 0, reply, &rn,
                                          &release_status);
                if (re == ESP_OK && release_status == AS608_NO_FINGER) {
                    LOCK();
                    fp_need_release = false;
                    UNLOCK();
                }
            } else if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 1; n = sizeof(reply);
                if (as608_cmd(0x02, &buffer, 1, reply, &n)) {
                    uint16_t matched, score;
                    uint8_t status;
                    if (as608_search(&matched, &score, &status)) {
                        char id[8]; snprintf(id, sizeof(id), "%u", matched);
                        ESP_LOGI(TAG, "Fingerprint match ID=%u score=%u", matched, score);
                        log_event("FP", id); grant("FINGER", id);
                    } else {
                        ESP_LOGW(TAG, "Fingerprint no match (AS608 status=0x%02X)", status);
                        tft_result_screen("FINGER", "", false);
                        vTaskDelay(pdMS_TO_TICKS(700));
                        if (attendance_ready()) tft_ready_screen();
                    }
                }
            }
        }
next:
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void cam_task(void *arg)
{
    char buffer[96];
    while (true) {
        int n = uart_read_bytes(CAM_UART, (uint8_t *)buffer, sizeof(buffer) - 1,
                                pdMS_TO_TICKS(100));
        if (n > 0 && attendance_ready()) {
            buffer[n] = 0;
            char *p = strstr(buffer, "FACE_ID:");
            if (p) {
                char *end = strpbrk(p, "\r\n"); if (end) *end = 0;
                log_event("FACE", p + 8); grant("FACE", p + 8);
            }
        }
    }
}

static void presence_task(void *arg)
{
    int old = -1;
    while (true) {
        int now = gpio_get_level(PIN_LD2410_OUT);
        if (now != old) {
            old = now;
            uart_write_bytes(CAM_UART, now ? "PRESENCE:1\n" : "PRESENCE:0\n", 11);
            ESP_LOGI(TAG, "LD2410C presence=%d", now);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ============================== app_main ============================== */
void app_main(void)
{
    state_mutex = xSemaphoreCreateMutex();
    tft_mutex = xSemaphoreCreateMutex();
    if (!state_mutex || !tft_mutex) {
        ESP_LOGE(TAG, "Failed to create state mutex - halting");
        abort();
    }

    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Sync Vietnam time over Wi-Fi/NTP. */
    wifi_time_init();

    /*
     * Create the Google Sheet logging queue + its dedicated 8 KB-stack
     * background task BEFORE any sensor task can call grant() (which
     * calls google_sheet_log_attendance()). Must run after Wi-Fi/NTP init
     * above so the background task's first HTTPS attempt has a working
     * network and correct clock (for TLS certificate validation), but it
     * can run before the sensor tasks are created below regardless -
     * google_sheet_init() itself does not touch the network.
     */
    if (!google_sheet_init()) {
        ESP_LOGE(TAG, "Google Sheet logging disabled (init failed)");
    }

    gpio_config_t presence = {
        .pin_bit_mask = 1ULL << PIN_LD2410_OUT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&presence));
    for (int i = 0; i < 4; ++i) {
        ESP_ERROR_CHECK(gpio_set_direction(cols[i], GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(cols[i], 1));
        ESP_ERROR_CHECK(gpio_set_direction(rows[i], GPIO_MODE_INPUT));
        ESP_ERROR_CHECK(gpio_set_pull_mode(rows[i], GPIO_PULLUP_ONLY));
    }

    uart_config_t uart_cfg = {
        .baud_rate = AS608_BAUD, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(AS608_UART, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(AS608_UART, PIN_AS608_TX, PIN_AS608_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(AS608_UART, 512, 0, 0, NULL, 0));

    uart_cfg.baud_rate = CAM_BAUD;
    ESP_ERROR_CHECK(uart_param_config(CAM_UART, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(CAM_UART, PIN_CAM_TX, PIN_CAM_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(CAM_UART, 512, 0, 0, NULL, 0));

    rc522_init();
    tft_init();
    if (as608_is_online()) {
        as608_read_capacity();
        ESP_LOGI(TAG, "AS608 online, template capacity: %u", fp_library_size);
    } else {
        ESP_LOGE(TAG, "AS608 not responding - check TX/RX, GND and AS608_BAUD");
    }

    xTaskCreate(rfid_task, "rfid", 4096, NULL, 5, NULL);
    xTaskCreate(fingerprint_task, "finger", 4096, NULL, 5, NULL);
    xTaskCreate(keypad_task, "keypad", 3072, NULL, 4, NULL);
    xTaskCreate(cam_task, "cam", 3072, NULL, 5, NULL);
    xTaskCreate(presence_task, "presence", 2048, NULL, 4, NULL);
    xTaskCreate(enroll_watchdog_task, "enroll_wd", 2048, NULL, 3, NULL);
    xTaskCreate(ui_housekeeping_task, "ui_house", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "SYSTEM READY.");
    ESP_LOGI(TAG, "ADMIN: * | A:switch | B:back/delete | C:select/confirm | #:back to attendance");
    ESP_LOGI(TAG, "============================================");
}