
#include "wifi_time.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"

#define WIFI_SSID       "Hai Dang"
#define WIFI_PASSWORD   "22226666"

#define NTP_SERVER_1    "pool.ntp.org"
#define NTP_SERVER_2    "time.google.com"
#define NTP_SERVER_3    "vn.pool.ntp.org"

#define WIFI_CONNECT_TIMEOUT_MS  15000
#define NTP_SYNC_TIMEOUT_MS      15000

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "WIFI_TIME";

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num = 0;

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "NTP: time synchronized");
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        if (s_wifi_retry_num < 10) {

            s_wifi_retry_num++;

            esp_wifi_connect();

            ESP_LOGW(TAG,
                     "Wi-Fi disconnected, retrying %d/10",
                     s_wifi_retry_num);

        } else {

            xEventGroupSetBits(
                s_wifi_event_group,
                WIFI_FAIL_BIT
            );

            ESP_LOGE(TAG,
                     "Wi-Fi: FAILED TO CONNECT");
        }

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG,
                 "Wi-Fi OK, IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        s_wifi_retry_num = 0;

        xEventGroupSetBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }
}

static bool wifi_connect(void)
{
    s_wifi_event_group = xEventGroupCreate();

    if (!s_wifi_event_group) {
        ESP_LOGE(TAG,
                 "Failed to create Wi-Fi event group");
        return false;
    }

    esp_err_t err;

    err = esp_netif_init();

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE) {

        ESP_LOGE(TAG,
                 "esp_netif_init failed: %s",
                 esp_err_to_name(err));

        return false;
    }

    err = esp_event_loop_create_default();

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE) {

        ESP_LOGE(TAG,
                 "esp_event_loop_create_default failed: %s",
                 esp_err_to_name(err));

        return false;
    }

    esp_netif_t *sta_netif =
        esp_netif_create_default_wifi_sta();

    if (!sta_netif) {
        ESP_LOGE(TAG,
                 "Failed to create STA netif");
        return false;
    }

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            &instance_any_id
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            &instance_got_ip
        )
    );

    wifi_config_t wifi_config = {0};

    strncpy(
        (char *)wifi_config.sta.ssid,
        WIFI_SSID,
        sizeof(wifi_config.sta.ssid) - 1
    );

    strncpy(
        (char *)wifi_config.sta.password,
        WIFI_PASSWORD,
        sizeof(wifi_config.sta.password) - 1
    );

    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_OPEN;

    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_LOGI(TAG,
             "Connecting to Wi-Fi: %s",
             WIFI_SSID);

    EventBits_t bits =
        xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT |
            WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(
                WIFI_CONNECT_TIMEOUT_MS
            )
        );

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static bool ntp_sync(void)
{
    /* Vietnam = UTC+7, no DST. */
    setenv("TZ", "ICT-7", 1);
    tzset();

    esp_sntp_setoperatingmode(
        SNTP_OPMODE_POLL
    );

    esp_sntp_setservername(
        0,
        NTP_SERVER_1
    );

    esp_sntp_setservername(
        1,
        NTP_SERVER_2
    );

    esp_sntp_setservername(
        2,
        NTP_SERVER_3
    );

    esp_sntp_set_time_sync_notification_cb(
        time_sync_notification_cb
    );

    esp_sntp_init();

    int64_t start_us =
        esp_timer_get_time();

    while (
        (esp_timer_get_time() - start_us) <
        ((int64_t)NTP_SYNC_TIMEOUT_MS * 1000LL)
    ) {

        time_t now = time(NULL);

        if (now >= 1700000000) {

            struct tm vn;

            localtime_r(
                &now,
                &vn
            );

            ESP_LOGI(
                TAG,
                "Local time: %02d:%02d:%02d %02d/%02d/%04d",
                vn.tm_hour,
                vn.tm_min,
                vn.tm_sec,
                vn.tm_mday,
                vn.tm_mon + 1,
                vn.tm_year + 1900
            );

            return true;
        }

        vTaskDelay(
            pdMS_TO_TICKS(250)
        );
    }

    ESP_LOGE(
        TAG,
        "NTP: TIMEOUT, time not synced"
    );

    return false;
}

bool wifi_time_init(void)
{
    ESP_LOGI(
        TAG,
        "Initializing Wi-Fi + NTP..."
    );

    if (!wifi_connect()) {

        ESP_LOGE(
            TAG,
            "Skipping NTP because Wi-Fi did not connect"
        );

        return false;
    }

    if (!ntp_sync()) {

        ESP_LOGE(
            TAG,
            "Skipping NTP because time sync failed"
        );

        return false;
    }

    return true;
}

bool wifi_time_get_display(char *day_out, size_t day_out_sz,
                           char *time_out, size_t time_out_sz,
                           char *date_out, size_t date_out_sz)
{
    if (!day_out || day_out_sz == 0 ||
        !time_out || time_out_sz == 0 ||
        !date_out || date_out_sz == 0) {

        return false;
    }

    time_t now = time(NULL);

    if (now < 1700000000) {

        snprintf(day_out, day_out_sz, "---------");
        snprintf(time_out, time_out_sz, "--:--:--");
        snprintf(date_out, date_out_sz, "--/--/----");

        return false;
    }

    struct tm vn;

    localtime_r(
        &now,
        &vn
    );

    /* %A gives the English weekday name in the default "C" locale
     * (e.g. "Thursday"); upper-case it to match the font, which only
     * has glyphs for A-Z, 0-9, ':' and '/'. */
    strftime(
        day_out,
        day_out_sz,
        "%A",
        &vn
    );
    for (char *p = day_out; *p; ++p) {
        *p = (char)toupper((unsigned char)*p);
    }

    snprintf(
        time_out,
        time_out_sz,
        "%02d : %02d : %02d",
        vn.tm_hour,
        vn.tm_min,
        vn.tm_sec
    );

    strftime(
        date_out,
        date_out_sz,
        "%d/%m/%Y",
        &vn
    );

    return true;
}