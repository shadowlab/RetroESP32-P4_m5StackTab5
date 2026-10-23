/*
 * Wi-Fi file server (M5Stack Tab5): a small web page and HTTP API on port 80 for browsing the SD card,
 * uploading, downloading, renaming and deleting files from a browser on the same network.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     busy;          /* a transfer is running */
    bool     upload;        /* ...and it is an upload (else a download) */
    char     name[48];      /* file name of the running transfer */
    uint64_t done, total;   /* bytes */
    uint32_t uploads, downloads;    /* finished since the server started */
} file_server_activity_t;

esp_err_t file_server_start(void);
void file_server_stop(void);
void file_server_activity(file_server_activity_t *out);

#ifdef __cplusplus
}
#endif
