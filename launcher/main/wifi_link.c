/*
 * Wi-Fi link (M5Stack Tab5) - see wifi_link.h.
 *
 * The C6 runs ESP-Hosted 3.0.9 (shadowlab/tab5-p4-c6-sdio-ota) and is reached over SDMMC slot 1. The SD card is
 * on slot 0 of the same controller and is mounted first, at boot: ESP-IDF 5.5's sdmmc_host_init() then sees the
 * controller already running and skips its init, and both slots run at 40 MHz so they share the host clock.
 * esp_wifi_remote turns the esp_wifi_* calls below into requests to the C6.
 */
#include "sdkconfig.h"

#if CONFIG_BOARD_M5STACK_TAB5

#include "wifi_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "tab5_board.h"

static const char *TAG = "wifi_link";

#define BIT_GOT_IP      BIT0
#define BIT_FAILED      BIT1

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_started;
static bool s_hotspot;
static volatile bool s_want_connected;  /* reconnect after a drop */
static volatile bool s_connecting;      /* in wifi_link_connect(): count retries there */
static volatile int  s_last_reason;
static volatile int  s_ap_clients;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        s_last_reason = ev->reason;
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        if (s_connecting) {
            xEventGroupSetBits(s_events, BIT_FAILED);
        } else if (s_want_connected) {
            ESP_LOGW(TAG, "Connection lost (reason %d), reconnecting", ev->reason);
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

esp_err_t wifi_link_start(void)
{
    if (s_started) return ESP_OK;

    esp_err_t err = tab5_board_wifi_power_enable(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not switch the C6 on (0x%x)", err);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    if (esp_hosted_init() != 0 || esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "No answer from the C6 over SDIO");
        return ESP_FAIL;
    }
    esp_hosted_coprocessor_fwver_t ver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == 0) {
        ESP_LOGI(TAG, "C6 runs ESP-Hosted %d.%d.%d", (int)ver.major1, (int)ver.minor1, (int)ver.patch1);
    }

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, "retroesp-tab5");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed (0x%x)", err);
        return err;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);

    esp_wifi_set_storage(WIFI_STORAGE_RAM);   /* the launcher keeps the network in its own NVS */
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed (0x%x)", err);
        return err;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);            /* throughput over battery while transferring */
    s_started = true;
    return ESP_OK;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const wifi_link_ap_t *)b)->rssi - ((const wifi_link_ap_t *)a)->rssi;
}

int wifi_link_scan(wifi_link_ap_t *out, int max)
{
    if (!s_started) return -1;
    if (s_hotspot) {                   /* scanning needs station mode: the hotspot ends here */
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_hotspot = false;
    }
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) n = 40;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
    if (!recs) { esp_wifi_clear_ap_list(); return -1; }
    esp_wifi_scan_get_ap_records(&n, recs);

    int count = 0;
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') continue;
        int j;
        for (j = 0; j < count; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) break;
        }
        if (j < count) {                       /* same network, another access point: keep the strongest */
            if (recs[i].rssi > out[j].rssi) out[j].rssi = recs[i].rssi;
            continue;
        }
        if (count == max) continue;
        strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
        out[count].rssi = recs[i].rssi;
        out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN && recs[i].authmode != WIFI_AUTH_OWE;
        count++;
    }
    free(recs);
    qsort(out, count, sizeof(*out), cmp_rssi);
    return count;
}

wifi_link_result_t wifi_link_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!s_started) return WIFI_LINK_ERR_OTHER;

    s_want_connected = false;
    esp_wifi_disconnect();
    if (s_hotspot) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_hotspot = false;
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &wc);

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    wifi_link_result_t res = WIFI_LINK_ERR_TIMEOUT;
    s_connecting = true;
    for (int attempt = 0; attempt < 3; attempt++) {
        xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED);
        s_last_reason = 0;
        esp_wifi_connect();
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) break;
        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE, deadline - now);
        if (bits & BIT_GOT_IP) { res = WIFI_LINK_OK; break; }
        if (!(bits & BIT_FAILED)) { res = WIFI_LINK_ERR_TIMEOUT; break; }

        int r = s_last_reason;
        ESP_LOGW(TAG, "Connect to \"%s\" failed, reason %d", ssid, r);
        if (r == WIFI_REASON_NO_AP_FOUND) res = WIFI_LINK_ERR_NOT_FOUND;
        else if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                 r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE ||
                 r == WIFI_REASON_802_1X_AUTH_FAILED || r == WIFI_REASON_CONNECTION_FAIL)
            res = WIFI_LINK_ERR_AUTH;
        else res = WIFI_LINK_ERR_OTHER;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    s_connecting = false;

    if (res == WIFI_LINK_OK) {
        s_want_connected = true;
    } else {
        esp_wifi_disconnect();
    }
    return res;
}

esp_err_t wifi_link_start_hotspot(const char *ssid, const char *pass)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    s_want_connected = false;
    esp_wifi_disconnect();

    wifi_config_t wc = {0};
    strlcpy((char *)wc.ap.ssid, ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(ssid);
    strlcpy((char *)wc.ap.password, pass, sizeof(wc.ap.password));
    wc.ap.channel = 6;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Hotspot failed (0x%x)", err);
        return err;
    }
    s_hotspot = true;
    s_ap_clients = 0;
    return ESP_OK;
}

bool wifi_link_is_up(void)
{
    if (!s_started) return false;
    if (s_hotspot) return true;
    return (xEventGroupGetBits(s_events) & BIT_GOT_IP) != 0;
}

int wifi_link_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_hotspot || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

int wifi_link_hotspot_clients(void)
{
    return s_ap_clients;
}

void wifi_link_ip_str(char *buf, size_t len)
{
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(s_hotspot ? s_ap_netif : s_sta_netif, &ip);
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
}

bool wifi_link_load_saved(char *ssid, char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = WIFI_LINK_SSID_MAX + 1, pl = WIFI_LINK_PASS_MAX + 1;
    bool ok = nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK && ssid[0] != '\0';
    if (ok && nvs_get_str(h, "pass", pass, &pl) != ESP_OK) pass[0] = '\0';
    nvs_close(h);
    return ok;
}

void wifi_link_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

void wifi_link_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
}

void wifi_link_hotspot_password(char *pass, size_t len)
{
    nvs_handle_t h;
    size_t l = len;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        strlcpy(pass, "retroesp", len);
        return;
    }
    if (nvs_get_str(h, "ap_pass", pass, &l) != ESP_OK || strlen(pass) < 8) {
        /* Eight digits: easy to type on a phone, and WPA2's minimum length */
        snprintf(pass, len, "%08lu", (unsigned long)(esp_random() % 100000000UL));
        nvs_set_str(h, "ap_pass", pass);
        nvs_commit(h);
    }
    nvs_close(h);
}

void wifi_link_power_off(void)
{
    if (s_started) {
        s_want_connected = false;
        esp_wifi_stop();
    }
    tab5_board_wifi_power_enable(false);
}

#endif /* CONFIG_BOARD_M5STACK_TAB5 */
