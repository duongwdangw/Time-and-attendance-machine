#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

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

/* Mã confirmation trong giao thức AS608/R30x. */
#define AS608_OK         0x00
#define AS608_NO_FINGER  0x02
#define AS608_NO_MATCH   0x09

/* Nếu quá trình đăng ký (bấm B) không hoàn tất trong thời gian này thì tự hủy,
 * tránh khóa vĩnh viễn chức năng chấm công nếu admin bỏ đi giữa chừng. */
#define ENROLL_TIMEOUT_US (60LL * 1000000LL)

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
    ENROLL_WAIT_VERIFY_REMOVE,
    ENROLL_WAIT_VERIFY,
} enroll_state_t;

/*
 * Mutex bảo vệ TOÀN BỘ khối trạng thái đăng ký/admin bên dưới. keypad_task ghi,
 * rfid_task/fingerprint_task đọc và ghi - hai core khác nhau trên ESP32 nên
 * chỉ đánh dấu volatile là không đủ để đảm bảo tính nhất quán nhiều biến liên
 * quan với nhau (vd enroll_emp_id phải khớp với current_enroll_state).
 */
static SemaphoreHandle_t state_mutex;

static enroll_state_t current_enroll_state = ENROLL_NONE;
static bool admin_authenticated;
static uint16_t enroll_emp_id;
static uint8_t enroll_rfid_uid[RFID_UID_LEN];
static bool enroll_rfid_pending;
static int64_t enroll_started_us;
/* Đọc từ cảm biến khi khởi động, không dùng hằng số đoán dung lượng. */
static uint16_t fp_library_size = 300;

static char input_buffer[12];
static size_t input_len;

#define LOCK()   xSemaphoreTake(state_mutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(state_mutex)

/*
 * ============================================================================
 * GIAO DIỆN MENU ADMIN (chỉ dùng bên trong keypad_task, KHÔNG được task nào
 * khác đọc/ghi) - vì vậy các biến này KHÔNG cần nằm dưới state_mutex.
 * admin_authenticated / current_enroll_state (đã có mutex ở trên) vẫn là
 * "nguồn sự thật" cho rfid_task & fingerprint_task; admin_ui_mode chỉ là lớp
 * hiển thị/điều hướng phím phía trên, không được hai task kia đọc tới nên
 * không phá vỡ nguyên tắc đồng bộ hoá đã có sẵn trong code gốc.
 * ============================================================================
 */
typedef enum {
    ADMIN_UI_NONE = 0,      /* Chưa đăng nhập (đang nhập PIN hoặc màn hình chờ) */
    ADMIN_UI_MENU,          /* Đã đăng nhập, chờ chọn B (them) hoặc D (xoa)      */
    ADMIN_UI_ENTER_ADD_ID,  /* Đang nhập ID nhân viên cần THÊM                   */
    ADMIN_UI_ENTER_DEL_ID,  /* Đang nhập ID nhân viên cần XOA                    */
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

/*
 * Màn hình "nhập liệu" dùng chung cho: nhập PIN admin, nhập ID thêm nhân
 * viên, nhập ID xoá nhân viên. Hiển thị trực tiếp giá trị đang gõ trong một
 * khung nổi bật để dễ quan sát/soát lỗi, kèm dòng hướng dẫn phím phía dưới.
 * CHỈ THÊM MỚI - không đụng tới tft_message() / các hàm TFT phía trên.
 */
static void tft_input_screen(const char *title, const char *prompt,
                              const char *value, const char *hint1,
                              const char *hint2)
{
    if (!tft_io) return;
    tft_fill(0x0010);
    tft_box(0, 0, TFT_W, 22, 0x03E0);           /* thanh tieu de */
    tft_text(6, 7, title, 0xffff);

    tft_text(8, 34, prompt, 0xffe0);

    /* khung nhap lieu noi bat */
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
 * MỚI: tra ngược UID RFID theo emp_id (namespace "users" lưu key=UID,
 * value=emp_id nên cần duyệt toàn bộ). Dùng khi xoá nhân viên: ta chỉ có
 * ID nhập từ bàn phím, cần tìm ra đúng thẻ RFID tương ứng để xoá triệt để.
 * Không đụng tới các hàm NVS phía trên - chỉ đọc.
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

/* Đọc dưới lock để có snapshot nhất quán của cả hai cờ. */
static bool attendance_ready(void)
{
    LOCK();
    bool ready = !admin_authenticated && current_enroll_state == ENROLL_NONE;
    UNLOCK();
    return ready;
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
/*
 * Trả ESP_OK khi gói UART hợp lệ. Mã thành công/thất bại của AS608 nằm ở
 * *status; không được biến mọi lỗi thành "không có ngón tay".
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
     * FIX QUAN TRỌNG: uart_read_bytes bên dưới đọc "body_len + 2" byte (thêm
     * 2 byte checksum) vào `reply`. Điều kiện cũ chỉ so `body_len > *rlen`
     * nên khi body_len == *rlen (vd == 32, đúng bằng sizeof(reply[32])) thì
     * lệnh đọc sẽ ghi 34 byte vào một mảng chỉ có 32 byte -> tràn stack.
     * Một khung UART nhiễu (rớt dây, nhiễu điện) hoàn toàn có thể tạo ra giá
     * trị body_len sát biên như vậy trước khi checksum được kiểm tra ở dưới.
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

/* Search phải duyệt đúng kích thước thư viện của chính cảm biến. */
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
        ESP_LOGE(TAG, "Khong xoa duoc template loi ID %u", id);
}

/*
 * MỚI: xoá triệt để một nhân viên - vân tay (AS608) + RFID (NVS), coi như
 * thẻ RFID cũ trở thành thẻ "trắng" hoàn toàn (không còn ánh xạ tới ID nào).
 * Chỉ gọi từ keypad_task, sau khi admin đã xác thực - không đụng tới
 * current_enroll_state/admin_authenticated nên không cần state_mutex; các
 * hàm con (as608_delete_template/erase_rfid_from_nvs) vốn đã được gọi không
 * khoá ở nơi khác trong code gốc theo đúng khuôn mẫu tương tự.
 * Trả về true nếu tìm thấy và đã xoá bản ghi RFID tương ứng với emp_id.
 */
static bool delete_employee(uint16_t emp_id)
{
    uint8_t uid[RFID_UID_LEN];
    bool had_rfid = find_uid_by_emp_id(emp_id, uid);

    as608_delete_template(emp_id); /* an toàn dù ID chưa từng có mẫu vân tay */

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

/* Phải được gọi trong khi đã giữ LOCK(). */
static void cancel_admin_or_enrollment_locked(void)
{
    /* Nếu đã Store nhưng chưa qua quét kiểm chứng, không để dữ liệu nửa chừng.
     * Các lệnh AS608/NVS bên dưới không cần giữ mutex, nhưng ta chụp lại
     * (snapshot) các giá trị cần thiết trước khi rời khỏi vùng lock nếu cần
     * gọi I/O chậm; ở đây as608_delete_template/erase_rfid_from_nvs không
     * đụng tới state chung nên gọi thẳng là an toàn. */
    bool need_rollback = enroll_rfid_pending &&
        (current_enroll_state == ENROLL_WAIT_VERIFY_REMOVE ||
         current_enroll_state == ENROLL_WAIT_VERIFY);
    uint16_t rollback_id = enroll_emp_id;
    uint8_t rollback_uid[RFID_UID_LEN];
    memcpy(rollback_uid, enroll_rfid_uid, RFID_UID_LEN);

    admin_authenticated = false;
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

static void cancel_admin_or_enrollment(void)
{
    LOCK();
    cancel_admin_or_enrollment_locked();
    UNLOCK();
    tft_message("CHAM CONG", "SAN SANG", "CHO XAC THUC");
}

/* ---- Các màn hình admin mới (chỉ hiển thị - không đụng logic gốc) ---- */
static void ui_show_pin_entry(void)
{
    char shown[12];
    size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
    memcpy(shown, input_buffer, n); shown[n] = 0;
    tft_input_screen("DANG NHAP ADMIN", "MA PIN:", shown,
                      "#:XAC NHAN  C:XOA", "*:HUY");
}

static void ui_show_menu(void)
{
    tft_input_screen("ADMIN", "CHON CHUC NANG", "",
                      "B: THEM NHAN VIEN", "D: XOA NHAN VIEN");
}

static void ui_show_add_id_entry(void)
{
    char shown[12];
    size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
    memcpy(shown, input_buffer, n); shown[n] = 0;
    tft_input_screen("THEM NHAN VIEN", "NHAP MA SO NV:", shown,
                      "NHAN B DE HOAN TAT", "C:XOA  *:HUY");
}

static void ui_show_del_id_entry(void)
{
    char shown[12];
    size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
    memcpy(shown, input_buffer, n); shown[n] = 0;
    tft_input_screen("XOA NHAN VIEN", "NHAP MA SO NV:", shown,
                      "NHAN D DE XAC NHAN", "C:XOA  *:HUY");
}

static void keypad_task(void *arg)
{
    bool entering_master = false;
    admin_ui_mode_t admin_ui_mode = ADMIN_UI_NONE;
    bool prev_is_admin = false;

    while (true) {
        char key = keypad_scan();
        if (!key) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        ESP_LOGI(TAG, "=> Phim duoc nhan: [%c]", key);

        LOCK();
        bool is_admin = admin_authenticated;
        enroll_state_t enroll_now = current_enroll_state;
        UNLOCK();

        /*
         * MỚI: nếu admin_authenticated vừa chuyển true -> false do MỘT TASK
         * KHÁC gây ra (fingerprint_task hoàn tất/that bại đăng ký, hoặc
         * enroll_watchdog_task hết giờ), thì lớp menu cục bộ của bàn phím
         * phải tự đồng bộ lại về trạng thái NONE - tránh việc phím B/D/C bị
         * "kẹt" ở một menu không còn hợp lệ. Không đụng tới bất kỳ biến dùng
         * chung nào, chỉ reset biến cục bộ của chính keypad_task.
         */
        if (prev_is_admin && !is_admin) {
            admin_ui_mode = ADMIN_UI_NONE;
            entering_master = false;
            input_len = 0;
        }
        prev_is_admin = is_admin;

        if (key == '*') {
            cancel_admin_or_enrollment();
            entering_master = true;
            admin_ui_mode = ADMIN_UI_NONE;
            ESP_LOGI(TAG, "Nhap PIN admin, roi nhan #");
            ui_show_pin_entry();
        } else if (key == 'C') {
            /* Backspace: chỉ có tác dụng khi đang nhập PIN hoặc ID. */
            if ((entering_master || admin_ui_mode == ADMIN_UI_ENTER_ADD_ID ||
                 admin_ui_mode == ADMIN_UI_ENTER_DEL_ID) && input_len > 0) {
                input_len--;
                if (entering_master) ui_show_pin_entry();
                else if (admin_ui_mode == ADMIN_UI_ENTER_ADD_ID) ui_show_add_id_entry();
                else if (admin_ui_mode == ADMIN_UI_ENTER_DEL_ID) ui_show_del_id_entry();
            }
        } else if (key == '#') {
            input_buffer[input_len] = 0;
            if (entering_master && nvs_pin_ok(input_buffer)) {
                LOCK();
                admin_authenticated = true;
                UNLOCK();
                entering_master = false;
                input_len = 0;
                admin_ui_mode = ADMIN_UI_MENU;
                prev_is_admin = true;
                ESP_LOGI(TAG, "DANG NHAP ADMIN THANH CONG");
                ui_show_menu();
            } else if (entering_master) {
                input_len = 0;
                ESP_LOGW(TAG, "PIN admin sai");
                tft_message("LOI", "MA PIN SAI", "NHAN * DE THU LAI");
            }
        } else if (key == 'B') {
            if (is_admin && admin_ui_mode == ADMIN_UI_MENU && enroll_now == ENROLL_NONE) {
                /* Buoc 1: bat dau che do THEM nhan vien - chi hien man hinh,
                 * chua dong gi den trang thai enroll dung/goc. */
                admin_ui_mode = ADMIN_UI_ENTER_ADD_ID;
                input_len = 0;
                ui_show_add_id_entry();
            } else if (is_admin && admin_ui_mode == ADMIN_UI_ENTER_ADD_ID && input_len) {
                /* Buoc 2: da go xong ID, xac nhan bang B (dung logic goc
                 * khoi tao enroll, khong thay doi gi ca). */
                input_buffer[input_len] = 0;
                long id = strtol(input_buffer, NULL, 10);
                input_len = 0;
                if (id > 0 && id < fp_library_size) {
                    LOCK();
                    enroll_emp_id = (uint16_t)id;
                    enroll_rfid_pending = false;
                    current_enroll_state = ENROLL_WAIT_RFID;
                    enroll_started_us = esp_timer_get_time();
                    UNLOCK();
                    admin_ui_mode = ADMIN_UI_MENU;
                    ESP_LOGI(TAG, "DANG KY ID %u: QUET RFID", (unsigned)id);
                    tft_message("THEM NHAN VIEN", "QUET THE RFID", "D DE HUY");
                } else {
                    ESP_LOGW(TAG, "ID van tay ngoai dung luong %u: %ld", fp_library_size, id);
                    tft_message("LOI", "ID NGOAI DUNG LUONG", "NHAP LAI");
                    vTaskDelay(pdMS_TO_TICKS(1200));
                    ui_show_add_id_entry(); /* o lai man hinh de go lai ID */
                }
            }
        } else if (key == 'D') {
            if (is_admin && admin_ui_mode == ADMIN_UI_MENU && enroll_now == ENROLL_NONE) {
                /* Buoc 1: bat dau che do XOA nhan vien. */
                admin_ui_mode = ADMIN_UI_ENTER_DEL_ID;
                input_len = 0;
                ui_show_del_id_entry();
            } else if (is_admin && admin_ui_mode == ADMIN_UI_ENTER_DEL_ID && input_len) {
                /* Buoc 2: da go xong ID can xoa, xac nhan bang D. */
                input_buffer[input_len] = 0;
                long id = strtol(input_buffer, NULL, 10);
                input_len = 0;
                if (id > 0 && id < fp_library_size) {
                    bool existed = delete_employee((uint16_t)id);
                    char id_str[12];
                    snprintf(id_str, sizeof(id_str), "ID %ld", id);
                    if (existed) {
                        ESP_LOGI(TAG, "DA XOA NHAN VIEN ID %ld (RFID + van tay)", id);
                        tft_message("DA XOA", id_str, "THE DA THANH THE MOI");
                    } else {
                        ESP_LOGW(TAG, "Xoa ID %ld: khong tim thay the RFID lien ket", id);
                        tft_message("DA XOA VAN TAY", id_str, "(KHONG CO THE RFID)");
                    }
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    admin_ui_mode = ADMIN_UI_MENU;
                    ui_show_menu();
                } else {
                    ESP_LOGW(TAG, "ID xoa ngoai dung luong %u: %ld", fp_library_size, id);
                    tft_message("LOI", "ID NGOAI DUNG LUONG", "NHAP LAI");
                    vTaskDelay(pdMS_TO_TICKS(1200));
                    ui_show_del_id_entry(); /* o lai man hinh de go lai ID */
                }
            }
        } else if (key >= '0' && key <= '9' && input_len < sizeof(input_buffer) - 1) {
            /* Chi nhan so khi dang nhap PIN, hoac dang nhap ID them/xoa. */
            if (entering_master) {
                input_buffer[input_len++] = key;
                ui_show_pin_entry();
            } else if (admin_ui_mode == ADMIN_UI_ENTER_ADD_ID) {
                input_buffer[input_len++] = key;
                ui_show_add_id_entry();
            } else if (admin_ui_mode == ADMIN_UI_ENTER_DEL_ID) {
                input_buffer[input_len++] = key;
                ui_show_del_id_entry();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Task nền: nếu admin bấm B rồi bỏ đi giữa chừng (không hoàn tất, không bấm D),
 * hệ thống sẽ tự thoát khỏi trạng thái enroll sau ENROLL_TIMEOUT_US thay vì
 * khóa chức năng chấm công vô thời hạn. */
static void enroll_watchdog_task(void *arg)
{
    while (true) {
        LOCK();
        bool expired = current_enroll_state != ENROLL_NONE &&
            (esp_timer_get_time() - enroll_started_us) > ENROLL_TIMEOUT_US;
        UNLOCK();
        if (expired) {
            ESP_LOGW(TAG, "Het thoi gian dang ky, tu dong huy");
            cancel_admin_or_enrollment();
            tft_message("HET THOI GIAN", "DA HUY DANG KY", "SAN SANG");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ============================== Tasks ============================== */
static void rfid_task(void *arg)
{
    uint8_t uid[RFID_UID_LEN];
    char id[12];
    while (true) {
        if (rc522_poll(uid)) {
            LOCK();
            bool in_enroll_wait_rfid = (current_enroll_state == ENROLL_WAIT_RFID) &&
                                        admin_authenticated;
            uint16_t enroll_id_snapshot = enroll_emp_id;
            UNLOCK();

            if (in_enroll_wait_rfid) {
                uint16_t existing = check_rfid_in_nvs(uid);
                if (existing != 0 && existing != enroll_id_snapshot) {
                    ESP_LOGW(TAG, "The RFID da thuoc ID %u", existing);
                    tft_message("THE DA TON TAI", "QUET THE KHAC", "D DE HUY");
                } else if (!as608_is_online()) {
                    /* Không ghi thẻ khi cảm biến vân tay mất kết nối. */
                    ESP_LOGE(TAG, "AS608 khong phan hoi; chua luu RFID");
                    tft_message("LOI VAN TAY", "KIEM TRA AS608", "QUET LAI THE");
                } else {
                    LOCK();
                    /* Kiểm tra lại trạng thái còn đúng sau các thao tác I/O chậm ở trên
                     * (tránh ghi đè nếu admin đã bấm D hoặc watchdog đã hủy trong lúc chờ). */
                    if (current_enroll_state == ENROLL_WAIT_RFID &&
                        enroll_emp_id == enroll_id_snapshot) {
                        memcpy(enroll_rfid_uid, uid, sizeof(uid));
                        enroll_rfid_pending = true;
                        current_enroll_state = ENROLL_WAIT_FINGER_1;
                        enroll_started_us = esp_timer_get_time();
                        UNLOCK();
                        ESP_LOGI(TAG, "RFID OK; AS608 OK. DAT VAN TAY LAN 1");
                        tft_message("THE RFID OK", "DAT VAN TAY", "LAN 1");
                    } else {
                        UNLOCK();
                    }
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
                    ESP_LOGI(TAG, "Van tay lan 1 OK; nhac tay ra");
                    tft_message("VAN TAY OK", "NHAC TAY RA", "CHO LAN 2");
                }
            }
        } else if (st == ENROLL_WAIT_FINGER_REMOVE) {
            /* Chỉ status 0x02 mới thực sự có nghĩa là đã nhấc tay. */
            uint8_t status;
            esp_err_t err = as608_exec(0x01, NULL, 0, reply, &n, &status);
            if (err == ESP_OK && status == AS608_NO_FINGER) {
                LOCK();
                if (current_enroll_state == ENROLL_WAIT_FINGER_REMOVE) {
                    current_enroll_state = ENROLL_WAIT_FINGER_2;
                    enroll_started_us = esp_timer_get_time();
                }
                UNLOCK();
                tft_message("DAT LAI", "CUNG NGON TAY", "LAN 2");
                vTaskDelay(pdMS_TO_TICKS(400));
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "AS608 loi UART khi cho nhac tay: %s", esp_err_to_name(err));
            }
        } else if (st == ENROLL_WAIT_FINGER_2) {
            if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 2; n = sizeof(reply);
                if (!as608_cmd(0x02, &buffer, 1, reply, &n)) goto next;
                n = sizeof(reply);
                if (!as608_cmd(0x05, NULL, 0, reply, &n)) {
                    ESP_LOGW(TAG, "Hai lan van tay khong khop");
                    LOCK();
                    if (current_enroll_state == ENROLL_WAIT_FINGER_2) {
                        current_enroll_state = ENROLL_WAIT_FINGER_1;
                        enroll_started_us = esp_timer_get_time();
                    }
                    UNLOCK();
                    tft_message("VAN TAY KHAC", "DAT LAI LAN 1", "THU LAI");
                    goto next;
                }
                uint8_t store[] = {1, (uint8_t)(emp_id_snapshot >> 8), (uint8_t)emp_id_snapshot};
                n = sizeof(reply);
                if (!as608_cmd(0x06, store, sizeof(store), reply, &n)) {
                    ESP_LOGE(TAG, "Khong luu duoc template AS608");
                    tft_message("LOI LUU VAN TAY", "THU LAI", "D DE HUY");
                    goto next;
                }

                bool save_ok;
                LOCK();
                save_ok = enroll_rfid_pending &&
                          save_rfid_to_nvs(enroll_rfid_uid, emp_id_snapshot) == ESP_OK;
                if (save_ok && current_enroll_state == ENROLL_WAIT_FINGER_2) {
                    /* Store chỉ xác nhận đã ghi flash, chưa chứng minh Search tìm được.
                     * Bắt buộc nhấc tay và quét lần 3 để kiểm chứng xác thực thật. */
                    current_enroll_state = ENROLL_WAIT_VERIFY_REMOVE;
                    enroll_started_us = esp_timer_get_time();
                } else if (!save_ok) {
                    ESP_LOGE(TAG, "Template da luu nhung khong ghi duoc RFID vao NVS");
                    enroll_rfid_pending = false;
                    current_enroll_state = ENROLL_NONE;
                    admin_authenticated = false;
                }
                UNLOCK();

                if (save_ok) {
                    tft_message("DA LUU MAU", "NHAC TAY RA", "SE KIEM TRA");
                } else {
                    tft_message("LOI NVS", "THE CHUA DUOC LUU", "KIEM TRA NVS");
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
                tft_message("KIEM TRA", "DAT LAI VAN TAY", "LAN 3");
                vTaskDelay(pdMS_TO_TICKS(400));
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "AS608 loi UART khi cho xac minh: %s", esp_err_to_name(err));
            }
        } else if (st == ENROLL_WAIT_VERIFY) {
            if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 1; n = sizeof(reply);
                if (!as608_cmd(0x02, &buffer, 1, reply, &n)) {
                    ESP_LOGW(TAG, "Khong trich xuat duoc dac trung de kiem tra");
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
                    ESP_LOGI(TAG, "HOAN TAT DA KIEM CHUNG: ID %u, score %u", matched, score);
                    tft_message("THEM NHAN VIEN", "XAC THUC OK", "SAN SANG");
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    tft_message("CHAM CONG", "SAN SANG", "CHO XAC THUC");
                } else {
                    /* Không để lại tài khoản có thể đăng ký nhưng không xác thực được. */
                    ESP_LOGE(TAG, "Kiem chung that bai: status=0x%02X, match=%u, can=%u",
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
                    tft_message("LOI KIEM CHUNG", "DA HUY DU LIEU", "THU LAI");
                }
            }
        } else if (attendance_ready()) {
            if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 1; n = sizeof(reply);
                if (as608_cmd(0x02, &buffer, 1, reply, &n)) {
                    uint16_t matched, score;
                    uint8_t status;
                    if (as608_search(&matched, &score, &status)) {
                        char id[8]; snprintf(id, sizeof(id), "%u", matched);
                        ESP_LOGI(TAG, "Van tay match ID=%u score=%u", matched, score);
                        log_event("FP", id); grant("FINGER", id);
                    } else {
                        ESP_LOGW(TAG, "Van tay khong khop (AS608 status=0x%02X)", status);
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
    if (!state_mutex) {
        ESP_LOGE(TAG, "Khong tao duoc mutex trang thai - dung lai");
        abort();
    }

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
    if (as608_is_online()) {
        as608_read_capacity();
        ESP_LOGI(TAG, "AS608 online, dung luong template: %u", fp_library_size);
    } else {
        ESP_LOGE(TAG, "AS608 khong phan hoi - kiem tra TX/RX, GND va AS608_BAUD");
    }

    xTaskCreate(rfid_task, "rfid", 4096, NULL, 5, NULL);
    xTaskCreate(fingerprint_task, "finger", 4096, NULL, 5, NULL);
    xTaskCreate(keypad_task, "keypad", 3072, NULL, 4, NULL);
    xTaskCreate(cam_task, "cam", 3072, NULL, 5, NULL);
    xTaskCreate(presence_task, "presence", 2048, NULL, 4, NULL);
    xTaskCreate(enroll_watchdog_task, "enroll_wd", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "KHOI DONG XONG. RFID / VAN TAY SAN SANG.");
    ESP_LOGI(TAG, "ADMIN: * PIN # | B them NV | D xoa NV | C xoa ky tu");
    ESP_LOGI(TAG, "============================================");
}