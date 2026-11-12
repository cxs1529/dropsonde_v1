/**
 * ==============================================================================
 * @file    web_server.c
 * @brief   Wi-Fi Access Point and HTTP REST API Handlers.
 * 
 * @details
 * This file hosts the FreeRTOS web server that allows smartphones/laptops to 
 * interface with the SD card securely. It registers URIs for live JSON polling,
 * file listing/downloading, and Map configuration uploads.
 * 
 * Features:
 * - Decouples HTTP responses from Socket Drop errors to prevent 500 crashes
 * - Parses and groups companion files (GPSDATA + BMEDATA) in the `/api/list` endpoint
 * - Redirects background OS internet checks (Captive Portal)
 * ==============================================================================
 */

#include "web_server.h"
#include "dns_server.h"
#include "config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "driver/gpio.h"
#include <sys/statvfs.h>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "WEB_SERVER";
static httpd_handle_t web_server_handle = NULL;
extern char force_new_file_note[256];

// Assembly declarations to serve HTML directly from flash memory
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[]   asm("_binary_favicon_ico_end");

/**
 * @brief Serves the main React-style HTML dashboard.
 */
static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, (index_html_end - index_html_start));
    return ESP_OK; // Ignore socket drops from clients
}

/**
 * @brief Provides a 1Hz 500ms JSON feed of the current telemetry and sensor states.
 */
static esp_err_t live_api_get_handler(httpd_req_t *req) {
    char json_resp[512]; 
    gps_msg_t local_data; 
    bool local_logging;
    char local_active_file[64];

    // Safely copy GPS data out of the cross-thread context
    xSemaphoreTake(live_data_mutex, portMAX_DELAY);
    memcpy(&local_data, &live_gps_data, sizeof(gps_msg_t));
    local_logging = live_is_logging;
    strncpy(local_active_file, live_active_file, sizeof(local_active_file));
    xSemaphoreGive(live_data_mutex);

    // Safely copy BME sensor data out of the cross-thread context
    float t = 0, h = 0, p = 0;
    if (sensor_data_mutex) {
        xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
        t = current_sensor_data.temperature;
        h = current_sensor_data.humidity;
        p = current_sensor_data.pressure;
        xSemaphoreGive(sensor_data_mutex);
    }

    snprintf(json_resp, sizeof(json_resp), 
        "{\"date\":\"%02d/%02d/%02d\",\"time\":\"%02d:%02d:%02d\",\"lat\":\"%.6f\",\"lon\":\"%.6f\",\"pdop\":\"%.1f\",\"fix\":\"%s\",\"logging\":%s,\"active_file\":\"%s\",\"temp\":\"%.2f\",\"rh\":\"%.2f\",\"press\":\"%.2f\",\"volt\":\"X V\"}", 
        local_data.month, local_data.day, local_data.year, local_data.hour, local_data.min, local_data.sec, 
        local_data.lat, local_data.lon, local_data.pdop, local_data.fix_desc, local_logging ? "true" : "false", local_active_file,
        t, h, p);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, strlen(json_resp));
    return ESP_OK; 
}

/**
 * @brief Streams chunked JSON file arrays and pairs BMEDATA to the core GPSDATA deployment.
 */
static esp_err_t list_get_handler(httpd_req_t *req) {
    DIR *dir = opendir(DATA_DIR);
    if (!dir) { httpd_resp_send_500(req); return ESP_FAIL; }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1); 
    
    struct dirent *ent; 
    bool first_item = true; 
    char chunk[768]; 
    
    while ((ent = readdir(dir)) != NULL) {
        // Only trigger on the core GPS file to prevent duplicate UI rows
        char *ext = strstr(ent->d_name, ".dat");
        if (ext == NULL || strcmp(ext, ".dat") != 0) continue;
        if (strncmp(ent->d_name, "GPSDATA_", 8) != 0) continue;

        struct stat st; 
        char path[300]; 
        snprintf(path, sizeof(path), "%s/%s", DATA_DIR, ent->d_name);
        stat(path, &st);
        
        char meta_path[300];
        char note_path[300];
        int base_len = strlen(ent->d_name) - 4; // Strip .dat extension
        
        snprintf(meta_path, sizeof(meta_path), "%s/%.*s_meta.txt", META_DIR, base_len, ent->d_name);
        snprintf(note_path, sizeof(note_path), "%s/%.*s_note.txt", META_DIR, base_len, ent->d_name);
        
        long lines = 0; 
        char first_line[64] = "N/A", last_line[64] = "N/A", note_line[256] = "N/A";
        
        FILE *fm = fopen(meta_path, "r");
        if (fm) {
            char buf[128];
            if (fgets(buf, sizeof(buf), fm)) lines = atol(buf);
            if (fgets(buf, sizeof(buf), fm)) { buf[strcspn(buf, "\r\n")] = 0; strncpy(first_line, buf, 63); }
            if (fgets(buf, sizeof(buf), fm)) { buf[strcspn(buf, "\r\n")] = 0; strncpy(last_line, buf, 63); }
            fclose(fm);
        }

        FILE *fn = fopen(note_path, "r");
        if (fn) {
            char buf[256];
            if (fgets(buf, sizeof(buf), fn)) { buf[strcspn(buf, "\r\n")] = 0; strncpy(note_line, buf, 255); }
            fclose(fn);
        }
        
        snprintf(chunk, sizeof(chunk), "%s{\"name\":\"%s\",\"size\":%ld,\"lines\":%ld,\"first\":\"%s\",\"last\":\"%s\",\"note\":\"%s\"}", 
                 first_item ? "" : ",", ent->d_name, (long)st.st_size, lines, first_line, last_line, note_line);
        httpd_resp_send_chunk(req, chunk, strlen(chunk));
        first_item = false;
    }
    
    closedir(dir);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0); 
    return ESP_OK;
}

/**
 * @brief Steams downloads using attachment dispositions so browsers save instead of opening.
 */
static esp_err_t download_get_handler(httpd_req_t *req) {
    char filepath[256]; 
    char filename[128];
    
    if (httpd_req_get_url_query_str(req, filename, sizeof(filename)) != ESP_OK ||
        httpd_query_key_value(filename, "file", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_404(req); 
        return ESP_FAIL;
    }

    snprintf(filepath, sizeof(filepath), "%s/%s", DATA_DIR, filename);
    FILE *fd = fopen(filepath, "r");
    if (!fd) { 
        httpd_resp_send_404(req); 
        return ESP_FAIL; 
    }

    httpd_resp_set_type(req, "application/octet-stream");

    char header_val[160];
    snprintf(header_val, sizeof(header_val), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", header_val);

    char chunk[1024]; 
    size_t chunksize;
    do {
        chunksize = fread(chunk, 1, sizeof(chunk), fd);
        if (chunksize > 0) {
            httpd_resp_send_chunk(req, chunk, chunksize);
        }
    } while (chunksize != 0);

    fclose(fd);
    httpd_resp_send_chunk(req, NULL, 0); 
    return ESP_OK;
}

/**
 * @brief Streams file contents purely to the browser DOM for Map and Chart viewing.
 */
static esp_err_t view_get_handler(httpd_req_t *req) {
    char filepath[256]; char filename[128];
    if (httpd_req_get_url_query_str(req, filename, sizeof(filename)) != ESP_OK ||
        httpd_query_key_value(filename, "file", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_404(req); return ESP_FAIL;
    }
    snprintf(filepath, sizeof(filepath), "%s/%s", DATA_DIR, filename);
    FILE *fd = fopen(filepath, "r");
    if (!fd) { httpd_resp_send_404(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "text/plain"); 
    char chunk[1024]; size_t chunksize;
    do {
        chunksize = fread(chunk, 1, sizeof(chunk), fd);
        if (chunksize > 0) httpd_resp_send_chunk(req, chunk, chunksize);
    } while (chunksize != 0);
    fclose(fd);
    httpd_resp_send_chunk(req, NULL, 0); 
    return ESP_OK;
}

/**
 * @brief POST handler handling File Deletion and New Deployment Triggers.
 */
static esp_err_t manage_post_handler(httpd_req_t *req) {
    char query[512]; 
    char action[32], file[64], path[128];
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return ESP_FAIL;
    httpd_query_key_value(query, "action", action, sizeof(action));

    if (strcmp(action, "new_file") == 0) {
        char url_decoded_note[256] = "N/A";
        char raw_note[256] = "";
        
        if (httpd_query_key_value(query, "note", raw_note, sizeof(raw_note)) == ESP_OK) {
            int r = 0, w = 0;
            while(raw_note[r] != '\0' && w < 255) {
                if (raw_note[r] == '%' && raw_note[r+1] != '\0' && raw_note[r+2] != '\0') {
                    int val;
                    if (sscanf(&raw_note[r+1], "%2x", &val) == 1) {
                        url_decoded_note[w++] = (char)val;
                        r += 3;
                    } else {
                        url_decoded_note[w++] = raw_note[r++];
                    }
                } else if (raw_note[r] == '+') {
                    url_decoded_note[w++] = ' ';
                    r++;
                } else {
                    url_decoded_note[w++] = raw_note[r++];
                }
            }
            url_decoded_note[w] = '\0';
        }

        xSemaphoreTake(live_data_mutex, portMAX_DELAY);
        strncpy(force_new_file_note, url_decoded_note, sizeof(force_new_file_note) - 1);
        force_new_file_note[255] = '\0'; 
        force_new_file_flag = true; 
        xSemaphoreGive(live_data_mutex);
        
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
    }

    if (strcmp(action, "delete") == 0) {
        httpd_query_key_value(query, "file", file, sizeof(file));

        xSemaphoreTake(live_data_mutex, portMAX_DELAY);
        bool is_protected = (strcmp(file, live_active_file) == 0);
        xSemaphoreGive(live_data_mutex);

        if (is_protected) {
            ESP_LOGW(TAG, "Rejected attempt to delete ACTIVE file: %s", file);
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cannot delete active file.");
            return ESP_FAIL;
        }

        snprintf(path, sizeof(path), "%s/%s", DATA_DIR, file);
        
        char meta_path[128];
        char note_path[128];
        int base_len = strlen(file) - 4; 
        
        snprintf(meta_path, sizeof(meta_path), "%s/%.*s_meta.txt", META_DIR, base_len, file);
        snprintf(note_path, sizeof(note_path), "%s/%.*s_note.txt", META_DIR, base_len, file);

        unlink(path);
        unlink(meta_path);
        unlink(note_path);
        
        // Also delete BMEDATA companion file symmetrically
        char base_timestamp[32] = {0};
        sscanf(file, "GPSDATA_%[^.]", base_timestamp);
        char bme_file[64];
        snprintf(bme_file, sizeof(bme_file), "BMEDATA_%s.dat", base_timestamp);
        char bme_path[128];
        snprintf(bme_path, sizeof(bme_path), "%s/%s", DATA_DIR, bme_file);
        unlink(bme_path);

        printf("\n[WEB SERVER DELETE] Deleted %s and associated files\n", file);
    }
    
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/**
 * @brief Drains aborted POST connections from phones running background captive portal checks.
 */
static esp_err_t captive_portal_404_handler(httpd_req_t *req, httpd_err_code_t err) {
    if (req->method == HTTP_POST && req->content_len > 0) {
        char buf[256];
        int remaining = req->content_len;
        while (remaining > 0) {
            int to_recv = remaining < sizeof(buf) ? remaining : sizeof(buf);
            if (httpd_req_recv(req, buf, to_recv) <= 0) break;
            remaining -= to_recv;
        }
    }
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://aoml-dropsonde.com/");
    httpd_resp_set_hdr(req, "Connection", "close"); 
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void init_wifi_base(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_AP_SSID, 
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = 1, 
            .password = WIFI_AP_PASS,
            .max_connection = MAX_STA_CONN, 
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
}

/**
 * @brief Universal asset server handling Map files (Leaflet, uPlot, geoJSON).
 */
static esp_err_t map_asset_get_handler(httpd_req_t *req) {
    char filepath[256]; char filename[128];
    if (httpd_req_get_url_query_str(req, filename, sizeof(filename)) != ESP_OK ||
        httpd_query_key_value(filename, "file", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_404(req); return ESP_FAIL;
    }

    snprintf(filepath, sizeof(filepath), "%s/.MAP/%s", MOUNT_POINT, filename);
    FILE *fd = fopen(filepath, "r");
    if (!fd) { httpd_resp_send_404(req); return ESP_FAIL; }

    if (strstr(filename, ".css")) httpd_resp_set_type(req, "text/css");
    else if (strstr(filename, ".js")) httpd_resp_set_type(req, "application/javascript");
    else if (strstr(filename, ".geojson") || strstr(filename, ".json")) httpd_resp_set_type(req, "application/json");
    else if (strstr(filename, ".html")) httpd_resp_set_type(req, "text/html");
    else httpd_resp_set_type(req, "text/plain");

    char chunk[1024]; size_t chunksize;
    do {
        chunksize = fread(chunk, 1, sizeof(chunk), fd);
        if (chunksize > 0) httpd_resp_send_chunk(req, chunk, chunksize);
    } while (chunksize != 0);
    fclose(fd);
    
    httpd_resp_send_chunk(req, NULL, 0); 
    return ESP_OK;
}

static esp_err_t favicon_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/x-icon");
    const size_t favicon_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_size);
    return ESP_OK;
}

static esp_err_t storage_api_get_handler(httpd_req_t *req) {
    FATFS *fs = NULL;
    DWORD fre_clust = 0;

    FRESULT res = f_getfree("0:", &fre_clust, &fs);
    if (res != FR_OK || fs == NULL) {
        res = f_getfree("", &fre_clust, &fs);
    }

    uint64_t total_bytes = 0, free_bytes = 0, used_bytes = 0;

    if (res == FR_OK && fs != NULL) {
        uint64_t tot_sect = (uint64_t)(fs->n_fatent - 2) * fs->csize;
        uint64_t fre_sect = (uint64_t)fre_clust * fs->csize;
#if FF_MAX_SS != FF_MIN_SS
        uint64_t sector_size = fs->ssize;
#else
        uint64_t sector_size = 512;
#endif
        total_bytes = tot_sect * sector_size;
        free_bytes  = fre_sect * sector_size;
        used_bytes  = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;
    }

    char json_response[128];
    snprintf(json_response, sizeof(json_response), 
             "{\"total\":%llu,\"used\":%llu,\"free\":%llu}", 
             (unsigned long long)total_bytes, 
             (unsigned long long)used_bytes, 
             (unsigned long long)free_bytes);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t upload_targets_post_handler(httpd_req_t *req) {
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/.MAP/targets.csv", MOUNT_POINT);
    
    FILE *fd = fopen(filepath, "w");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to open targets file for writing");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "File open failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int received;
    int remaining = req->content_len;

    while (remaining > 0) {
        int to_recv = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        if ((received = httpd_req_recv(req, buf, to_recv)) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            fclose(fd);
            unlink(filepath); 
            return ESP_FAIL;
        }
        fwrite(buf, 1, received, fd);
        remaining -= received;
    }
    
    fclose(fd);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_req_hdr_len = 2048;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 12; // Adjusted above the default 8 limit
    config.stack_size = 8192;     // Increased to accommodate large chunked arrays

    if (httpd_start(&web_server_handle, &config) == ESP_OK) {

        httpd_register_err_handler(web_server_handle, HTTPD_404_NOT_FOUND, captive_portal_404_handler);
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler };
        httpd_uri_t live_uri  = { .uri = "/api/live", .method = HTTP_GET, .handler = live_api_get_handler };
        httpd_uri_t list_uri  = { .uri = "/api/list", .method = HTTP_GET, .handler = list_get_handler };
        httpd_uri_t dl_uri    = { .uri = "/download*", .method = HTTP_GET, .handler = download_get_handler };
        httpd_uri_t view_uri  = { .uri = "/view*", .method = HTTP_GET, .handler = view_get_handler };
        httpd_uri_t mng_uri   = { .uri = "/api/manage*", .method = HTTP_POST, .handler = manage_post_handler };
        httpd_uri_t map_asset_uri = { .uri = "/map_asset*", .method = HTTP_GET, .handler = map_asset_get_handler };
        httpd_uri_t favicon_uri = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler };
        httpd_uri_t storage_uri = { .uri = "/api/storage", .method = HTTP_GET, .handler = storage_api_get_handler };
        httpd_uri_t upload_targets_uri = { .uri = "/api/upload_targets", .method = HTTP_POST, .handler = upload_targets_post_handler };
        
        httpd_register_uri_handler(web_server_handle, &index_uri);
        httpd_register_uri_handler(web_server_handle, &live_uri);
        httpd_register_uri_handler(web_server_handle, &list_uri);
        httpd_register_uri_handler(web_server_handle, &dl_uri);
        httpd_register_uri_handler(web_server_handle, &view_uri);
        httpd_register_uri_handler(web_server_handle, &mng_uri);
        httpd_register_uri_handler(web_server_handle, &map_asset_uri);
        httpd_register_uri_handler(web_server_handle, &favicon_uri);
        httpd_register_uri_handler(web_server_handle, &storage_uri);
        httpd_register_uri_handler(web_server_handle, &upload_targets_uri);
    }
}

void wifi_manager_task(void *arg) {
    bool logical_wifi_state = false;
    TickType_t pause_tick = 0, resume_tick = 0;
    TickType_t last_reminder_tick = 0; 
    bool last_physical_state = (gpio_get_level(WIFI_SWITCH_PIN) == 0);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200)); 
        bool physical_switch_is_on = (gpio_get_level(WIFI_SWITCH_PIN) == 0);
        TickType_t current_time = xTaskGetTickCount();

        if (physical_switch_is_on && !last_physical_state) {
            printf("\n((( WI-FI SWITCH ACTIVE. Waiting 10 seconds to power up antenna... )))\n\n");
        } else if (!physical_switch_is_on && last_physical_state) {
            printf("\n((( WI-FI SWITCH PAUSED. Waiting 10 seconds to power down antenna... )))\n\n");
        }
        last_physical_state = physical_switch_is_on;

        if (!physical_switch_is_on) {
            resume_tick = 0;
            if (logical_wifi_state) {
                if (pause_tick == 0) pause_tick = current_time;
                if ((current_time - pause_tick) >= pdMS_TO_TICKS(5000)) {
                    logical_wifi_state = false;
                    printf("((( Disabling Wi-Fi and Web Server to save battery. )))\n");
                    if (web_server_handle) { 
                        httpd_stop(web_server_handle); 
                        web_server_handle = NULL; 
                    }
                    stop_dns_server();
                    esp_wifi_stop(); 
                }
            }
        } else {
            pause_tick = 0;
            if (!logical_wifi_state) {
                if (resume_tick == 0) resume_tick = current_time;
                if ((current_time - resume_tick) >= pdMS_TO_TICKS(10000)) {
                    logical_wifi_state = true;
                    last_reminder_tick = current_time; 
                    
                    esp_wifi_start();
                    start_webserver();
                    start_dns_server();
                    
                    printf("\n======================================================================\n");
                    printf("  NOAA DASHBOARD ACTIVE\n");
                    printf("  1. Connect your device to Wi-Fi Network : %s\n", WIFI_AP_SSID);
                    printf("  2. Open any web browser and navigate to : http://aomldropsonde.com\n");
                    printf("     (Or use direct IP Address            : http://192.168.4.1)\n");
                    printf("======================================================================\n\n");
                }
            } else {
                if ((current_time - last_reminder_tick) >= pdMS_TO_TICKS(30000)) {
                    printf("\n======================================================================\n");
                    printf("  NOAA DASHBOARD ACTIVE - REMINDER\n");
                    printf("  1. Connect your device to Wi-Fi Network : %s\n", WIFI_AP_SSID);
                    printf("  2. Open any web browser and navigate to : http://aomldropsonde.com\n");
                    printf("     (Or use direct IP Address            : http://192.168.4.1)\n");
                    printf("======================================================================\n\n");
                    last_reminder_tick = current_time;
                }
            }
        }
    }
}
