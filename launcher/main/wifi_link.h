/*
 * Wi-Fi link (M5Stack Tab5): Wi-Fi through the ESP32-C6 co-processor (ESP-Hosted over SDIO), for the
 * launcher's Wi-Fi transfer screen. Station mode joins a saved or picked network; hotspot mode makes the
 * Tab5 its own access point for when there is no router.
 *
 * Wi-Fi only ever runs in the launcher: the C6 is switched off at boot and the launcher restarts when
 * Wi-Fi transfer ends, so the emulators keep all of their memory.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_LINK_SSID_MAX 32
#define WIFI_LINK_PASS_MAX 64

typedef struct {
    char    ssid[WIFI_LINK_SSID_MAX + 1];
    int8_t  rssi;
    bool    secure;     /* needs a password */
} wifi_link_ap_t;

typedef enum {
    WIFI_LINK_OK = 0,
    WIFI_LINK_ERR_TIMEOUT,      /* no answer from the network */
    WIFI_LINK_ERR_AUTH,         /* wrong password (or the network refused us) */
    WIFI_LINK_ERR_NOT_FOUND,    /* the network is not in range */
    WIFI_LINK_ERR_OTHER,
} wifi_link_result_t;

/* Power the C6, connect to it over SDIO and start Wi-Fi (no network joined yet). */
esp_err_t wifi_link_start(void);

/* Blocking scan. Fills up to max entries, strongest first, one entry per network name.
 * Returns the number of entries, or -1 on error. */
int wifi_link_scan(wifi_link_ap_t *out, int max);

/* Join a network and wait for an IP address. pass may be "" for an open network. */
wifi_link_result_t wifi_link_connect(const char *ssid, const char *pass, int timeout_ms);

/* Be an access point instead (WPA2, channel 6). The address is then 192.168.4.1. */
esp_err_t wifi_link_start_hotspot(const char *ssid, const char *pass);

/* Still connected (station) / clients on the hotspot. */
bool wifi_link_is_up(void);
int  wifi_link_rssi(void);
int  wifi_link_hotspot_clients(void);
void wifi_link_ip_str(char *buf, size_t len);

/* Saved network (NVS, namespace "wifi"). */
bool wifi_link_load_saved(char *ssid, char *pass);
void wifi_link_save(const char *ssid, const char *pass);
void wifi_link_forget(void);

/* The hotspot's password: made up once, then kept in NVS so a PC that joined it once can join again. */
void wifi_link_hotspot_password(char *pass, size_t len);

/* Switch the C6 off before a restart. */
void wifi_link_power_off(void);

#ifdef __cplusplus
}
#endif
