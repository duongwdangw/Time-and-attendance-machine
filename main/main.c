#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* GPIO4/12/13/14 là các đường được quét; GPIO22/25/26/27 là đường drive. */
static const gpio_num_t rows[] = {4, 12, 13, 14};
static const gpio_num_t cols[] = {22, 25, 26, 27};

#define AS608_UART       UART_NUM_2
#define CAM_UART         UART_NUM_1
#define AS608_BAUD       57600 /* Đổi thành 115200 nếu bạn đã đổi baud trên AS608. */
#define CAM_BAUD         115200

#define TFT_W            128
#define TFT_H            160
#define RFID_UID_LEN     4    /* Bản này dành cho thẻ MIFARE UID 4 byte. */
#define FINGER_ID_MAX    1000 /* Phù hợp phần lớn AS608 có 1000 template. */

static const char *TAG = "ACCESS";
static spi_device_handle_t rc522;
static esp_lcd_panel_io_handle_t tft_io;
static uint16_t tft_frame[TFT_W * TFT_H];
static int64_t last_accept_us;

typedef enum {
    ENROLL_NONE = 0,
    ENROLL_WAIT_RFID,
    ENROLL_WAIT_FINGER_1,
    ENROLL_WAIT_FINGER_REMOVE,
    ENROLL_WAIT_FINGER_2,
} enroll_state_t;

/* Các biến này chỉ được thay đổi theo từng task operation nguyên tử trên ESP32. */
static volatile enroll_state_t current_enroll_state = ENROLL_NONE;
static volatile bool admin_authenticated;
static uint16_t enroll_emp_id;
static uint8_t enroll_rfid_uid[RFID_UID_LEN];
static bool enroll_rfid_pending;

static char input_buffer[12];
static size_t input_len;

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

/* Font 5x7: A-Z và 0-9. Màn hình chỉ dùng ASCII không dấu. */
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
    {0x02,0x15,0x15,0x0e,0}
};

static int glyph_index(char c)
{
    if (c >= 'A' && c <= 'Z') return 1 + c - 'A';
    if (c >= '0' && c <= '9') return 27 + c - '0';
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

static void tft_message(const char *title, const char *line1, const char *line2)
{
    if (!tft_io) return;
    tft_fill(0x0010);
    tft_box(0, 0, TFT_W, 22, 0x03E0);
    tft_text(6, 7, title, 0xffff);
    tft_text(8, 45, line1, 0xffe0);
    tft_text(8, 65, line2, 0xffff);
    tft_text(8, 130, "SAN SANG", 0x07ff);
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
    tft_message("CHAM CONG", "KHOI DONG XONG", "CHO XAC THUC");
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

static bool attendance_ready(void)
{
    return !admin_authenticated && current_enroll_state == ENROLL_NONE;
}

static void grant(const char *method, const char *id)
{
    int64_t now = esp_timer_get_time();
    if (now - last_accept_us < 3000000) return; /* chống chấm trùng trong 3 giây */
    last_accept_us = now;
    ESP_LOGI(TAG, "=== XAC THUC THANH CONG! %s, ID: %s ===", method, id);
    tft_message("XAC THUC DUNG", method, id);
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
    rset(0x14, 0x03);      /* TxControlReg: bật antenna */
}

static int rc522_xfer_bits(const uint8_t *send, int slen, uint8_t *back,
                            int *blen, uint8_t tx_last_bits)
{
    wcr(0x01, 0x00);       /* CommandReg: Idle */
    wcr(0x02, 0x77);       /* ComIEnReg */
    wcr(0x04, 0x7f);       /* ComIrqReg: xóa cờ cũ */
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
    const uint8_t request[] = {0x26}; /* REQA phải là frame 7 bit */
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
static bool as608_cmd(uint8_t cmd, const uint8_t *data, size_t len,
                      uint8_t *reply, size_t *rlen)
{
    if (len > 20) return false;
    uint8_t packet[32] = {0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01,
                          (uint8_t)((len + 3) >> 8), (uint8_t)(len + 3), cmd};
    uint16_t sum = 1 + packet[7] + packet[8] + cmd;
    for (size_t i = 0; i < len; ++i) { packet[10 + i] = data[i]; sum += data[i]; }
    packet[10 + len] = (uint8_t)(sum >> 8);
    packet[11 + len] = (uint8_t)sum;

    uart_flush_input(AS608_UART);
    if (uart_write_bytes(AS608_UART, (const char *)packet, 12 + len) < 0) return false;
    uint8_t header[9];
    if (uart_read_bytes(AS608_UART, header, sizeof(header), pdMS_TO_TICKS(800)) != sizeof(header) ||
        header[0] != 0xef || header[1] != 0x01 || header[6] != 0x07) return false;

    int body_len = ((header[7] << 8) | header[8]) - 2; /* confirmation + parameters */
    if (body_len < 1 || body_len > (int)*rlen) return false;
    if (uart_read_bytes(AS608_UART, reply, body_len + 2, pdMS_TO_TICKS(500)) != body_len + 2)
        return false;

    uint16_t checksum = 0;
    for (int i = 6; i < 9; ++i) checksum += header[i];
    for (int i = 0; i < body_len; ++i) checksum += reply[i];
    if (checksum != (uint16_t)((reply[body_len] << 8) | reply[body_len + 1])) return false;
    *rlen = body_len;
    return reply[0] == 0x00;
}

static bool as608_is_online(void)
{
    uint8_t reply[32];
    size_t n = sizeof(reply);
    return as608_cmd(0x0f, NULL, 0, reply, &n); /* ReadSysPara */
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

static void cancel_admin_or_enrollment(void)
{
    admin_authenticated = false;
    current_enroll_state = ENROLL_NONE;
    enroll_rfid_pending = false;
    enroll_emp_id = 0;
    input_len = 0;
    tft_message("CHAM CONG", "SAN SANG", "CHO XAC THUC");
}

static void keypad_task(void *arg)
{
    bool entering_master = false;
    while (true) {
        char key = keypad_scan();
        if (!key) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        ESP_LOGI(TAG, "=> Phim duoc nhan: [%c]", key);

        if (key == '*') {
            cancel_admin_or_enrollment();
            entering_master = true;
            ESP_LOGI(TAG, "Nhap PIN admin, roi nhan #");
            tft_message("ADMIN", "NHAP PIN", "ROI NHAN #");
        } else if (key == 'D' && admin_authenticated) {
            cancel_admin_or_enrollment();
            entering_master = false;
            ESP_LOGI(TAG, "Da thoat admin");
        } else if (key == '#') {
            input_buffer[input_len] = 0;
            if (entering_master && nvs_pin_ok(input_buffer)) {
                admin_authenticated = true;
                entering_master = false;
                input_len = 0;
                ESP_LOGI(TAG, "DANG NHAP ADMIN THANH CONG");
                tft_message("ADMIN OK", "NHAP ID ROI B", "D DE THOAT");
            } else if (entering_master) {
                input_len = 0;
                ESP_LOGW(TAG, "PIN admin sai");
                tft_message("LOI", "MA PIN SAI", "THU LAI");
            }
        } else if (key == 'B' && admin_authenticated && current_enroll_state == ENROLL_NONE && input_len) {
            input_buffer[input_len] = 0;
            long id = strtol(input_buffer, NULL, 10);
            input_len = 0;
            if (id > 0 && id < FINGER_ID_MAX) {
                enroll_emp_id = (uint16_t)id;
                enroll_rfid_pending = false;
                current_enroll_state = ENROLL_WAIT_RFID;
                ESP_LOGI(TAG, "DANG KY ID %u: QUET RFID", enroll_emp_id);
                tft_message("THEM NHAN VIEN", "QUET THE RFID", "D DE HUY");
            } else {
                ESP_LOGW(TAG, "ID vân tay không hợp lệ: %ld", id);
                tft_message("LOI", "ID 1 DEN 999", "NHAP LAI");
            }
        } else if (key >= '0' && key <= '9' && input_len < sizeof(input_buffer) - 1) {
            /* Chỉ nhận số cho PIN hoặc ID admin. Ngoài admin, RFID/FP luôn sẵn sàng. */
            if (entering_master || admin_authenticated) input_buffer[input_len++] = key;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ============================== Tasks ============================== */
static void rfid_task(void *arg)
{
    uint8_t uid[RFID_UID_LEN];
    char id[12];
    while (true) {
        if (rc522_poll(uid)) {
            if (current_enroll_state == ENROLL_WAIT_RFID && admin_authenticated) {
                uint16_t existing = check_rfid_in_nvs(uid);
                if (existing != 0 && existing != enroll_emp_id) {
                    ESP_LOGW(TAG, "The RFID da thuoc ID %u", existing);
                    tft_message("THE DA TON TAI", "QUET THE KHAC", "D DE HUY");
                } else if (!as608_is_online()) {
                    /* Không ghi thẻ khi cảm biến vân tay mất kết nối. */
                    ESP_LOGE(TAG, "AS608 khong phan hoi; chua luu RFID");
                    tft_message("LOI VAN TAY", "KIEM TRA AS608", "QUET LAI THE");
                } else {
                    memcpy(enroll_rfid_uid, uid, sizeof(uid));
                    enroll_rfid_pending = true;
                    current_enroll_state = ENROLL_WAIT_FINGER_1;
                    ESP_LOGI(TAG, "RFID OK; AS608 OK. DAT VAN TAY LAN 1");
                    tft_message("THE RFID OK", "DAT VAN TAY", "LAN 1");
                }
                vTaskDelay(pdMS_TO_TICKS(800));
            } else if (attendance_ready()) {
                uint16_t emp = check_rfid_in_nvs(uid);
                if (emp) {
                    snprintf(id, sizeof(id), "%u", emp);
                    log_event("RFID", id); grant("RFID", id);
                } else {
                    ESP_LOGW(TAG, "The RFID chua dang ky");
                    tft_message("THE CHUA DANG KY", "LIEN HE ADMIN", "SAN SANG");
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
        if (current_enroll_state == ENROLL_WAIT_FINGER_1) {
            if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 1; n = sizeof(reply);
                if (as608_cmd(0x02, &buffer, 1, reply, &n)) {
                    ESP_LOGI(TAG, "Van tay lan 1 OK; nhac tay ra");
                    current_enroll_state = ENROLL_WAIT_FINGER_REMOVE;
                    tft_message("VAN TAY OK", "NHAC TAY RA", "CHO LAN 2");
                }
            }
        } else if (current_enroll_state == ENROLL_WAIT_FINGER_REMOVE) {
            /* GetImage lỗi khi không còn ngón tay: đó là điều kiện chuyển sang lần 2. */
            if (!as608_cmd(0x01, NULL, 0, reply, &n)) {
                current_enroll_state = ENROLL_WAIT_FINGER_2;
                tft_message("DAT LAI", "CUNG NGON TAY", "LAN 2");
                vTaskDelay(pdMS_TO_TICKS(400));
            }
        } else if (current_enroll_state == ENROLL_WAIT_FINGER_2) {
            if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 2; n = sizeof(reply);
                if (!as608_cmd(0x02, &buffer, 1, reply, &n)) goto next;
                n = sizeof(reply);
                if (!as608_cmd(0x05, NULL, 0, reply, &n)) {
                    ESP_LOGW(TAG, "Hai lan van tay khong khop");
                    current_enroll_state = ENROLL_WAIT_FINGER_1;
                    tft_message("VAN TAY KHAC", "DAT LAI LAN 1", "THU LAI");
                    goto next;
                }
                uint8_t store[] = {1, (uint8_t)(enroll_emp_id >> 8), (uint8_t)enroll_emp_id};
                n = sizeof(reply);
                if (!as608_cmd(0x06, store, sizeof(store), reply, &n)) {
                    ESP_LOGE(TAG, "Khong luu duoc template AS608");
                    tft_message("LOI LUU VAN TAY", "THU LAI", "D DE HUY");
                    goto next;
                }
                if (!enroll_rfid_pending ||
                    save_rfid_to_nvs(enroll_rfid_uid, enroll_emp_id) != ESP_OK) {
                    ESP_LOGE(TAG, "Template da luu nhung khong ghi duoc RFID vao NVS");
                    enroll_rfid_pending = false;
                    current_enroll_state = ENROLL_NONE;
                    admin_authenticated = false;
                    tft_message("LOI NVS", "THE CHUA DUOC LUU", "KIEM TRA NVS");
                    goto next;
                }
                enroll_rfid_pending = false;
                current_enroll_state = ENROLL_NONE;
                admin_authenticated = false;
                ESP_LOGI(TAG, "HOAN TAT: RFID va van tay cua ID %u", enroll_emp_id);
                tft_message("THEM NHAN VIEN", "HOAN TAT", "SAN SANG");
                vTaskDelay(pdMS_TO_TICKS(1500));
                tft_message("CHAM CONG", "SAN SANG", "CHO XAC THUC");
            }
        } else if (attendance_ready()) {
            if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 1; n = sizeof(reply);
                if (as608_cmd(0x02, &buffer, 1, reply, &n)) {
                    const uint8_t search[] = {1, 0, 0, 0, 0, 0xa3};
                    n = sizeof(reply);
                    if (as608_cmd(0x04, search, sizeof(search), reply, &n) && n >= 3) {
                        uint16_t matched = ((uint16_t)reply[1] << 8) | reply[2];
                        char id[8]; snprintf(id, sizeof(id), "%u", matched);
                        log_event("FP", id); grant("FINGER", id);
                    } else {
                        ESP_LOGW(TAG, "Van tay khong khop");
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
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

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
    ESP_LOGI(TAG, "AS608 %s", as608_is_online() ? "online" : "khong phan hoi");

    xTaskCreate(rfid_task, "rfid", 4096, NULL, 5, NULL);
    xTaskCreate(fingerprint_task, "finger", 4096, NULL, 5, NULL);
    xTaskCreate(keypad_task, "keypad", 3072, NULL, 4, NULL);
    xTaskCreate(cam_task, "cam", 3072, NULL, 5, NULL);
    xTaskCreate(presence_task, "presence", 2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "KHOI DONG XONG. RFID / VAN TAY SAN SANG.");
    ESP_LOGI(TAG, "ADMIN: * PIN # | ID B de them | D de thoat");
    ESP_LOGI(TAG, "============================================");
}