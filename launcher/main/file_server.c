/*
 * Wi-Fi file server (M5Stack Tab5) - see file_server.h.
 *
 * Paths in the API are relative to the card root ("/roms/nes"), always start with "/" and may not contain "..".
 *
 *   GET  /                      the page (file_server.html)
 *   GET  /api/info              {"total":..,"free":..} in bytes
 *   GET  /api/list?path=        [{"n":name,"d":0|1,"s":size}, ...]
 *   GET  /api/file?path=        download
 *   POST /api/file?path=        upload (raw body); missing folders are created, an existing file is replaced
 *   POST /api/delete?path=      file, or folder with everything in it
 *   POST /api/mkdir?path=
 *   POST /api/rename?from=&to=
 *
 * One request is handled at a time (one server task), so uploads and downloads never compete for the card.
 */
#include "sdkconfig.h"

#if CONFIG_BOARD_M5STACK_TAB5

#include "file_server.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "odroid_sdcard.h"

static const char *TAG = "file_server";

#define SD_ROOT         "/sd"
#define REL_MAX         256                 /* a path below the card root */
#define FULL_MAX        (REL_MAX + 8)
#define XFER_BUF        (32 * 1024)         /* whole FAT clusters go straight to the card from here */

extern const char page_start[] asm("_binary_file_server_html_start");
extern const char page_end[]   asm("_binary_file_server_html_end");

static httpd_handle_t s_server;
static file_server_activity_t s_act;
static portMUX_TYPE s_act_lock = portMUX_INITIALIZER_UNLOCKED;

/* ─── activity (read by the launcher screen) ─────────────────────── */

static void act_begin(bool upload, const char *path, uint64_t total)
{
    const char *base = strrchr(path, '/');
    portENTER_CRITICAL(&s_act_lock);
    s_act.busy = true;
    s_act.upload = upload;
    strlcpy(s_act.name, base ? base + 1 : path, sizeof(s_act.name));
    s_act.done = 0;
    s_act.total = total;
    portEXIT_CRITICAL(&s_act_lock);
}

static void act_progress(uint64_t done)
{
    portENTER_CRITICAL(&s_act_lock);
    s_act.done = done;
    portEXIT_CRITICAL(&s_act_lock);
}

static void act_end(bool ok)
{
    portENTER_CRITICAL(&s_act_lock);
    if (ok) {
        if (s_act.upload) s_act.uploads++;
        else s_act.downloads++;
    }
    s_act.busy = false;
    portEXIT_CRITICAL(&s_act_lock);
}

void file_server_activity(file_server_activity_t *out)
{
    portENTER_CRITICAL(&s_act_lock);
    *out = s_act;
    portEXIT_CRITICAL(&s_act_lock);
}

/* ─── paths ──────────────────────────────────────────────────────── */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 2;
        } else if (*s == '+') {
            *o++ = ' ';
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

/* Read query parameter `key` as a card path and build the full VFS path ("/sd/...") in full. */
static bool get_path_param(httpd_req_t *req, const char *key, char *full)
{
    bool ok = false;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 4 * REL_MAX) return false;
    char *query = malloc(qlen + 1);
    char *rel = malloc(3 * REL_MAX + 1);
    if (!query || !rel) goto out;
    if (httpd_req_get_url_query_str(req, query, qlen + 1) != ESP_OK) goto out;
    if (httpd_query_key_value(query, key, rel, 3 * REL_MAX + 1) != ESP_OK) goto out;
    url_decode(rel);

    size_t len = strlen(rel);
    if (rel[0] != '/' || len >= REL_MAX) goto out;
    while (len > 1 && rel[len - 1] == '/') rel[--len] = '\0';
    for (const char *p = rel; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == '\\') goto out;
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0')) goto out;
    }
    snprintf(full, FULL_MAX, SD_ROOT "%s", len == 1 ? "" : rel);
    if (full[0] && strcmp(full, SD_ROOT) == 0) strcpy(full, SD_ROOT "/");
    ok = true;
out:
    free(query);
    free(rel);
    return ok;
}

/* "/sd/roms/nes" -> "0:/roms/nes" for the FatFs calls */
static void to_fatfs_path(const char *full, char *out, size_t len)
{
    BYTE pdrv = ff_diskio_get_pdrv_card(odroid_sdcard_get_card());
    const char *rel = full + strlen(SD_ROOT);
    snprintf(out, len, "%u:%s", (unsigned)pdrv, rel[0] ? rel : "/");
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t bad_path(httpd_req_t *req)
{
    return send_err(req, "400 Bad Request", "Bad or missing path");
}

/* ─── handlers ───────────────────────────────────────────────────── */

static esp_err_t h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);   /* EMBED_TXTFILES adds a NUL */
}

static esp_err_t h_info(httpd_req_t *req)
{
    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(SD_ROOT, &total, &freeb);
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"total\":%llu,\"free\":%llu}", (unsigned long long)total, (unsigned long long)freeb);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static void json_escape(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (; *in && o + 7 < len; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) o += snprintf(out + o, len - o, "\\u%04x", c);
        else out[o++] = c;
    }
    out[o] = '\0';
}

static esp_err_t h_list(httpd_req_t *req)
{
    char full[FULL_MAX], fpath[FULL_MAX + 8];
    if (!get_path_param(req, "path", full)) return bad_path(req);
    to_fatfs_path(full, fpath, sizeof(fpath));

    /* FatFs directly: f_readdir has the size of each entry, a stat() per file would search the folder again */
    FF_DIR *dir = malloc(sizeof(FF_DIR));
    FILINFO *fi = malloc(sizeof(FILINFO));
    char *line = malloc(3 * 256 + 64);
    char *esc = malloc(6 * 256 + 1);
    if (!dir || !fi || !line || !esc) {
        free(dir); free(fi); free(line); free(esc);
        return send_err(req, "500 Internal Server Error", "Out of memory");
    }
    esp_err_t ret = ESP_OK;
    if (f_opendir(dir, fpath) != FR_OK) {
        ret = send_err(req, "404 Not Found", "No such folder");
        goto out;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    bool first = true;
    httpd_resp_send_chunk(req, "[", 1);
    while (f_readdir(dir, fi) == FR_OK && fi->fname[0]) {
        if (fi->fattrib & (AM_HID | AM_SYS)) continue;    /* "System Volume Information" and friends */
        json_escape(fi->fname, esc, 6 * 256 + 1);
        int n = snprintf(line, 3 * 256 + 64, "%s{\"n\":\"%s\",\"d\":%d,\"s\":%lu}", first ? "" : ",", esc,
                         (fi->fattrib & AM_DIR) ? 1 : 0, (unsigned long)fi->fsize);
        if (httpd_resp_send_chunk(req, line, n) != ESP_OK) { ret = ESP_FAIL; break; }
        first = false;
    }
    f_closedir(dir);
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, "]", 1);
        httpd_resp_send_chunk(req, NULL, 0);
    }
out:
    free(dir); free(fi); free(line); free(esc);
    return ret;
}

static void *xfer_buf_alloc(void)
{
    /* DMA-capable internal RAM lets the SD driver skip its bounce buffer; PSRAM is the fallback */
    void *b = heap_caps_malloc(XFER_BUF, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!b) b = heap_caps_aligned_alloc(64, XFER_BUF, MALLOC_CAP_SPIRAM);
    return b;
}

/* Send everything, also when the socket takes it in pieces */
static bool send_all(httpd_req_t *req, const char *buf, size_t len)
{
    while (len > 0) {
        int n = httpd_send(req, buf, len);
        if (n <= 0) return false;
        buf += n;
        len -= n;
    }
    return true;
}

static esp_err_t h_download(httpd_req_t *req)
{
    char full[FULL_MAX];
    if (!get_path_param(req, "path", full)) return bad_path(req);
    int fd = open(full, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        if (fd >= 0) close(fd);
        return send_err(req, "404 Not Found", "No such file");
    }
    char *buf = xfer_buf_alloc();
    if (!buf) {
        close(fd);
        return send_err(req, "500 Internal Server Error", "Out of memory");
    }

    /* Headers by hand, so the browser gets a Content-Length and can show progress (send_chunk can't) */
    const char *name = strrchr(full, '/') + 1;
    int n = snprintf(buf, 1024,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "Content-Length: %ld\r\n"
                     "Content-Disposition: attachment; filename*=UTF-8''",
                     (long)st.st_size);
    for (const unsigned char *p = (const unsigned char *)name; *p && n < 1000; p++) {
        if (isalnum(*p) || strchr("-_.~", *p)) buf[n++] = *p;
        else n += snprintf(buf + n, 1024 - n, "%%%02X", *p);
    }
    n += snprintf(buf + n, 1024 - n, "\r\nCache-Control: no-cache\r\n\r\n");

    bool ok = send_all(req, buf, n);
    act_begin(false, full, st.st_size);
    uint64_t done = 0;
    while (ok) {
        ssize_t r = read(fd, buf, XFER_BUF);
        if (r < 0) { ok = false; break; }
        if (r == 0) break;
        ok = send_all(req, buf, r);
        done += r;
        act_progress(done);
    }
    act_end(ok && done == (uint64_t)st.st_size);
    close(fd);
    free(buf);
    if (!ok) ESP_LOGW(TAG, "Download of %s stopped after %llu bytes", full, (unsigned long long)done);
    /* On failure the connection is in an unknown state: returning an error closes it */
    return ok ? ESP_OK : ESP_FAIL;
}

/* mkdir -p for the folders above `full` */
static void make_parents(const char *full)
{
    char tmp[FULL_MAX];
    strlcpy(tmp, full, sizeof(tmp));
    for (char *p = tmp + strlen(SD_ROOT) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0777);
        *p = '/';
    }
}

static esp_err_t h_upload(httpd_req_t *req)
{
    char full[FULL_MAX];
    if (!get_path_param(req, "path", full) || strcmp(full, SD_ROOT "/") == 0) return bad_path(req);

    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_ROOT, &total, &freeb) == ESP_OK && req->content_len > freeb) {
        return send_err(req, "507 Insufficient Storage", "Not enough space on the SD card");
    }
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        return send_err(req, "409 Conflict", "A folder with that name exists");
    }
    make_parents(full);
    int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        ESP_LOGE(TAG, "Cannot create %s (errno %d)", full, errno);
        return send_err(req, "500 Internal Server Error", "Cannot create the file");
    }
    char *buf = xfer_buf_alloc();
    if (!buf) {
        close(fd);
        unlink(full);
        return send_err(req, "500 Internal Server Error", "Out of memory");
    }

    act_begin(true, full, req->content_len);
    size_t left = req->content_len;
    int timeouts = 0;
    bool ok = true;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < XFER_BUF ? left : XFER_BUF);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0) { ok = false; break; }
        timeouts = 0;
        /* Fill the buffer before writing, so the card mostly sees whole 32 KB writes */
        while (r < XFER_BUF && (size_t)r < left) {
            int m = httpd_req_recv(req, buf + r, (left < XFER_BUF ? left : XFER_BUF) - r);
            if (m == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
            if (m <= 0) { ok = false; break; }
            r += m;
        }
        if (!ok) break;
        if (write(fd, buf, r) != r) { ok = false; break; }
        left -= r;
        act_progress(req->content_len - left);
    }
    close(fd);
    free(buf);
    act_end(ok);
    if (!ok) {
        ESP_LOGW(TAG, "Upload of %s failed with %u bytes left", full, (unsigned)left);
        unlink(full);           /* no half files on the card */
        return send_err(req, "500 Internal Server Error", "Upload failed");
    }
    ESP_LOGI(TAG, "Received %s (%u bytes)", full, (unsigned)req->content_len);
    return httpd_resp_sendstr(req, "OK");
}

/* Delete a file or a whole folder. path is a FULL_MAX buffer that is used as scratch space. */
static bool remove_tree(char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    if (depth > 16) return false;

    DIR *d = opendir(path);
    if (!d) return false;
    size_t len = strlen(path);
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (len + 1 + strlen(e->d_name) >= FULL_MAX) { ok = false; break; }
        path[len] = '/';
        strcpy(path + len + 1, e->d_name);
        ok = remove_tree(path, depth + 1);
        path[len] = '\0';
    }
    closedir(d);
    return ok && rmdir(path) == 0;
}

static esp_err_t h_delete(httpd_req_t *req)
{
    char full[FULL_MAX];
    if (!get_path_param(req, "path", full) || strcmp(full, SD_ROOT "/") == 0) return bad_path(req);
    if (!remove_tree(full, 0)) return send_err(req, "500 Internal Server Error", "Could not delete everything");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t h_mkdir(httpd_req_t *req)
{
    char full[FULL_MAX];
    if (!get_path_param(req, "path", full) || strcmp(full, SD_ROOT "/") == 0) return bad_path(req);
    make_parents(full);
    if (mkdir(full, 0777) != 0 && errno != EEXIST) return send_err(req, "500 Internal Server Error", "Could not create the folder");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t h_rename(httpd_req_t *req)
{
    char from[FULL_MAX], to[FULL_MAX];
    if (!get_path_param(req, "from", from) || !get_path_param(req, "to", to) ||
        strcmp(from, SD_ROOT "/") == 0 || strcmp(to, SD_ROOT "/") == 0) return bad_path(req);
    struct stat st;
    if (stat(to, &st) == 0) return send_err(req, "409 Conflict", "That name is taken");
    if (rename(from, to) != 0) return send_err(req, "500 Internal Server Error", "Could not rename");
    return httpd_resp_sendstr(req, "OK");
}

/* ─── server ─────────────────────────────────────────────────────── */

esp_err_t file_server_start(void)
{
    if (s_server) return ESP_OK;
    memset(&s_act, 0, sizeof(s_act));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed (0x%x)", err);
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = h_page },
        { .uri = "/index.html", .method = HTTP_GET,  .handler = h_page },
        { .uri = "/api/info",   .method = HTTP_GET,  .handler = h_info },
        { .uri = "/api/list",   .method = HTTP_GET,  .handler = h_list },
        { .uri = "/api/file",   .method = HTTP_GET,  .handler = h_download },
        { .uri = "/api/file",   .method = HTTP_POST, .handler = h_upload },
        { .uri = "/api/file",   .method = HTTP_PUT,  .handler = h_upload },
        { .uri = "/api/delete", .method = HTTP_POST, .handler = h_delete },
        { .uri = "/api/mkdir",  .method = HTTP_POST, .handler = h_mkdir },
        { .uri = "/api/rename", .method = HTTP_POST, .handler = h_rename },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "File server running on port %d", cfg.server_port);
    return ESP_OK;
}

void file_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

#endif /* CONFIG_BOARD_M5STACK_TAB5 */
