#include "sdkconfig.h"

#include "Runtime-handlers.h"

#include "Server-mgr.h"
#include "Captive.h"
#include "Flags.h"
#include "SD-mgr.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"
#include "esp_check.h"
#include "errno.h"
#include "time.h"
#include <sys/stat.h>
#include <ctype.h>
#include "lwip/inet.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>  
#include <string.h>  
#include <stdlib.h>  
#include <sys/stat.h>  
#include <ctype.h>  
#include <sys/socket.h>  
#include <netinet/in.h>  
#include "lwip/inet.h"  
#include "freertos/FreeRTOS.h"  
#include "freertos/task.h"  

/** @brief Maximum number of client IPs to track for captive portal redirect */
#define MAX_REDIRECTED_IPS 10

/** @brief Array tracking IPs that have already been redirected to prevent redirect loops */
static uint32_t redirected_ips[MAX_REDIRECTED_IPS];

/** @brief Number of IPs currently tracked in redirected_ips array */
static int redirected_count = 0;

static const char *TAG = "Wifi: Runtime-handlers";

extern esp_netif_t *ap_netif, *sta_netif;

/**
 * @brief HTTP 404 error handler.
 * 
 * @param req HTTP request handle
 * @param error Error code
 * @return ESP_FAIL to indicate error
 */
esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t error) {
    #if CONFIG_WIFI_SHOW_SPA_404_HINT
    char text[300];
    #else
    char text[128];
    #endif
    size_t len = 0;
    len += snprintf(text + len, sizeof(text) - len, "404 Not Found\n\n");
    len += snprintf(text + len, sizeof(text) - len, "URI: %s\n", req->uri);
    len += snprintf(text + len, sizeof(text) - len, "Method: %s\n", (req->method == HTTP_GET) ? "GET" : "POST");
    len += snprintf(text + len, sizeof(text) - len, "Arguments:\n");
    #if CONFIG_WIFI_SHOW_SPA_404_HINT
    len += snprintf(text + len, sizeof(text) - len, "\nHint: If using an SPA framework (React, Vue, Angular, etc.) and see this error when routing directly, try setting the menuconfig option WIFI_SD_FILE_SERVING_MODE to SPA mode.");
    #endif
    char query[128];
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        httpd_req_get_url_query_str(req, query, query_len);
        len += snprintf(text + len, sizeof(text) - len, "%s\n", query);
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    ESP_LOGW(TAG, "%s", text);
    return ESP_FAIL;
}

/**
 * @brief HTTP GET handler for index.html (redirects to root).
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success
 */
esp_err_t index_html_get_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);  // Response body can be empty
    ESP_LOGD(TAG, "Redirecting to /");
    return ESP_OK;
}

/**
 * @deprecated
 * @brief HTTP handler for returning current WiFi connection status as JSON.
 * 
 * Provides information on whether connected, current IP address, SSID, and AP mode status.
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success
 */
esp_err_t wifi_status_json_handler(httpd_req_t *req) {
    char json[256];
    EventBits_t bits = wifi_flags_get_bits();
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(sta_netif, &ip_info);
    bool connected = (bits & CONNECTED_BIT) != 0;
    char ip_str[IP4ADDR_STRLEN_MAX];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, IP4ADDR_STRLEN_MAX);
    snprintf(json, sizeof(json), "{\"connected\": %s, \"ip\": \"%s\", \"ssid\": \"%s\", \"in_ap_mode\": %s, \"ap_ssid\": \"%s\"}",
             connected ? "true" : "false",
             ip_str,
             connected ? (const char*)get_sta_wifi_config().sta.ssid : "",
             (bits & AP_MODE_BIT) ? "true" : "false",
             (bits & AP_MODE_BIT) ? (const char*)get_ap_wifi_config().ap.ssid : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    ESP_LOGV(TAG, "WiFi status JSON sent: %s", json);
    return ESP_OK;
}

/**
 * @brief Check if a client IP has already been redirected to the captive portal.
 * 
 * @param ip Client IP address to check
 * @return true if IP has been redirected previously, false otherwise
 */
static bool is_ip_redirected(uint32_t ip) {
    for (int i = 0; i < redirected_count; i++) {
        if (redirected_ips[i] == ip) return true;
    }
    return false;
}

/**
 * @brief Mark a client IP as having been redirected to the captive portal.
 * 
 * Adds the IP to the redirected IPs list to prevent redirect loops.
 * 
 * @param ip Client IP address to mark
 */
static void mark_ip_redirected(uint32_t ip) {
    if (redirected_count < MAX_REDIRECTED_IPS) {
        redirected_ips[redirected_count++] = ip;
    }
}

// Restart from a background task so the HTTP server can finish sending
// the response and close the connection gracefully before reboot.
static void restart_delayed_task(void *pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    vTaskDelete(NULL);
}

/**
 * @brief HTTP GET handler for /restart endpoint (reboots device).
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success
 */
esp_err_t restart_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_send(req, "Restarting...", HTTPD_RESP_USE_STRLEN);

    BaseType_t r = xTaskCreate(restart_delayed_task, "restart_delayed", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (r != pdPASS) {
        // If task creation failed, fall back to delaying in-place (best-effort)
        esp_restart();
    }

    return ESP_OK;
}

/**
 * @brief HTTP GET handler when SD card is not present. Sends a 503 Service Unavailable response with instructions.
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success
 */
esp_err_t no_sd_card_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h2>SD card not detected</h2>\n<p>Please insert an SD card and <a href=\"/restart\">restart</a> the device</p>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


/* Small MIME mapping - extend as needed */
static const char *mime_from_path(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++; // skip '.'
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) return "text/html";
    if (strcasecmp(ext, "css") == 0) return "text/css";
    if (strcasecmp(ext, "js") == 0) return "application/javascript";
    if (strcasecmp(ext, "json") == 0) return "application/json";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, "map") == 0) return "application/octet-stream";
    if (strcasecmp(ext, "woff") == 0) return "font/woff";
    if (strcasecmp(ext, "woff2") == 0) return "font/woff2";
    if (strcasecmp(ext, "ttf") == 0) return "font/ttf";
    if (strcasecmp(ext, "mp4") == 0) return "video/mp4";
    return "application/octet-stream";
}

/* Heuristic: treat path as immutable if it is under /static/ or filename contains a long hex token before ext */
static bool is_immutable_path(const char *path)
{
    if (!path) return false;
    // path-based heuristic
    if (strstr(path, "/static/") == path || strstr(path, "/assets/") == path) return true;

    // filename hash heuristic: filename like main.1a2b3c4d.js
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    // look backwards for a '.' or '-' separating token before extension
    const char *hash = NULL;
    for (const char *p = name; p < dot; ++p) {
        if (*p == '.' || *p == '-') hash = p;
    }
    if (!hash) return false;
    size_t token_len = (size_t)(dot - hash - 1);
    if (token_len >= 8 && token_len <= 64) {
        // check if token is hex-like
        size_t hex_count = 0;
        for (const char *p = hash + 1; p < dot; ++p) {
            if (isxdigit((unsigned char)*p)) hex_count++;
        }
        if (hex_count == token_len) return true;
    }
    return false;
}

/* days_from_civil algorithm:
   Returns number of days since 1970-01-01 (Unix epoch) for given y,m,d.
   y: full year (e.g., 2026)
   m: 1..12
   d: 1..31
*/
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468; // 719468 = days from civil(0,3,1) to 1970-01-01
}

/* portable_timegm:
   Convert struct tm in UTC to time_t (seconds since 1970-01-01 UTC).
   tm->tm_year is years since 1900, tm->tm_mon is 0..11, tm->tm_mday is 1..31.
*/
time_t portable_timegm(struct tm *tm)
{
    if (!tm) return (time_t)-1;

    int64_t year = (int64_t)tm->tm_year + 1900;
    unsigned month = (unsigned)tm->tm_mon + 1; // make 1..12
    unsigned day = (unsigned)tm->tm_mday;

    int64_t days = days_from_civil(year, month, day);
    int64_t secs = days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
    return (time_t)secs;
}

/* Format RFC1123 date into buf (buf must be >= 32 bytes) */
static void format_rfc1123(time_t t, char *buf, size_t len)
{
    struct tm tm;
    gmtime_r(&t, &tm); // convert to UTC
    strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

/* Try to parse RFC1123 timestamp to time_t. Returns true on success. Best-effort parsing. */
static bool parse_rfc1123(const char *s, time_t *out)
{
    if (!s || !out) return false;
#if defined(__GNUC__) || defined(__GLIBC__) || defined(_POSIX_TIMERS)
    struct tm tm = {0};
    char *res = strptime(s, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (!res) {
        // try without weekday
        res = strptime(s, "%d %b %Y %H:%M:%S GMT", &tm);
        if (!res) return false;
    }
    // timegm is a GNU extension; try it first, otherwise fall back to mktime with TZ adjustment (best-effort)
#if defined(HAVE_TIMEGM) || defined(__USE_MISC) || defined(_BSD_SOURCE) || defined(_GNU_SOURCE)
    time_t t = portable_timegm(&tm);
    *out = t;
    return true;
#else
    // fallback: assume server local timezone is UTC (may be wrong on some systems)
    tm.tm_isdst = 0;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return false;
    *out = t;
    return true;
#endif
#else
    (void)s; (void)out;
    return false;
#endif
}

/* Build a weak ETag string from mtime and size (buf must be big enough) */
static void build_weak_etag(char *buf, size_t len, time_t mtime, uint64_t size)
{
    // W/"<mtime>-<size>"
    snprintf(buf, len, "W/\"%lx-%lx\"", (unsigned long)mtime, (unsigned long)size);
}

/**
 * @brief Helper function to serve a file from SD card with appropriate caching headers.
 * 
 * @param req HTTP request handle
 * @param filepath Path to the file on SD card to serve
 * @param serve_mode Mode for serving the file (revalidate, no-cache, immutable)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t send_sd_file(httpd_req_t *req, const char *filepath)
{
    if (!req || !filepath) return ESP_ERR_INVALID_ARG;

    char fullpath[512];
    // ensure filepath begins with '/'
    const char *rel = filepath;
    if (rel[0] != '/') {
        // temporary buffer to build path
        snprintf(fullpath, sizeof(fullpath), "%s/%s", get_sd_card_mount_point(), rel);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%s%s", get_sd_card_mount_point(), rel);
    }

    struct stat st;
    if (stat(fullpath, &st) != 0) {
        ESP_LOGW(TAG, "stat failed for %s: %s", fullpath, strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }

    time_t mtime = st.st_mtime;
    uint64_t fsize = (uint64_t)st.st_size;

    /* Build ETag and Last-Modified */
    char etag[64];
    build_weak_etag(etag, sizeof(etag), mtime, fsize);

    char last_modified[64];
    format_rfc1123(mtime, last_modified, sizeof(last_modified));

    /* Read request conditional headers */
    bool not_modified = false;

    ssize_t inm_len = httpd_req_get_hdr_value_len(req, "If-None-Match");
    if (inm_len > 0) {
        char *inm = malloc(inm_len + 1);
        if (inm) {
            if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, inm_len + 1) == ESP_OK) {
                // simple substring match; handles weak/strong lists crudely
                if (strstr(inm, etag) != NULL) {
                    not_modified = true;
                }
            }
            free(inm);
        }
    }

    if (!not_modified) {
        ssize_t ims_len = httpd_req_get_hdr_value_len(req, "If-Modified-Since");
        if (ims_len > 0) {
            char *ims = malloc(ims_len + 1);
            if (ims) {
                if (httpd_req_get_hdr_value_str(req, "If-Modified-Since", ims, ims_len + 1) == ESP_OK) {
                    time_t when;
                    if (parse_rfc1123(ims, &when)) {
                        if (when >= mtime) {
                            not_modified = true;
                        }
                    }
                }
                free(ims);
            }
        }
    }

    /* Decide Cache-Control based on compile-time mode and immutability heuristic */
    const char *cache_control = NULL;
#ifdef CONFIG_WIFI_SD_FILE_SERVING_MODE_SPA
    bool immutable = is_immutable_path(filepath);
    if (immutable) {
        cache_control = "public, max-age=31536000, immutable";
    } else {
        // index and other HTML (short/no-cache), others short
        const char *ct = mime_from_path(filepath);
        if (ct && strcmp(ct, "text/html") == 0) {
            cache_control = "no-cache, must-revalidate";
        } else {
            cache_control = "public, max-age=60";
        }
    }
#else
    /* STATIC mode: default short cache, but long for immutable heuristics */
    bool immutable = is_immutable_path(filepath);
    if (immutable) {
        cache_control = "public, max-age=31536000, immutable";
    } else {
        cache_control = "public, max-age=60";
    }
#endif

    /* Set headers that should always be present */
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Last-Modified", last_modified);
    httpd_resp_set_hdr(req, "Cache-Control", cache_control);

    /* If not modified, reply 304 */
    if (not_modified) {
        ESP_LOGD(TAG, "Not modified: %s", fullpath);
        httpd_resp_set_status(req, "304 Not Modified");
        // No body for 304
        return httpd_resp_send(req, NULL, 0);
    }

    /* Otherwise, stream the file */
    const char *mime = mime_from_path(filepath);
    if (mime) {
        httpd_resp_set_type(req, mime);
    }

    // HEAD should return headers only
    if (req->method == HTTP_HEAD) {
        return httpd_resp_send(req, NULL, 0);
    }

    FILE *f = fopen(fullpath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s (%s)", fullpath, strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }

    // Stream in small chunks
    const size_t buf_size = 1024;
    char *buf = malloc(buf_size);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "OOM allocating stream buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t r;
    esp_err_t res = ESP_OK;
    while ((r = fread(buf, 1, buf_size, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            ESP_LOGW(TAG, "Client closed during send");
            res = ESP_FAIL;
            break;
        }
    }

    // finish chunked response
    if (res == ESP_OK) {
        if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to finalize chunked response");
            res = ESP_FAIL;
        }
    }

    free(buf);
    fclose(f);
    return res;
}

/**
 * @brief HTTP handler for serving files from the SD card.
 * 
 * This handler serves files from the SD card mounted at /sdcard. It performs:
 * - Captive portal detection URL handling (redirects first request, returns 204 for subsequent)
 * - Directory index handling (serves index.html for directories)
 * - File extension-based content type detection
 * - Automatic .html extension appending for extensionless paths
 * 
 * Supported file types include HTML, CSS, JS, JSON, images, fonts, video, and more.
 * 
 * @param req HTTP request handle
 * @return ESP_OK on successful file serving
 * @return ESP_FAIL if file not found or cannot be opened
 */
esp_err_t sd_file_handler(httpd_req_t *req) {
    ESP_LOGV(TAG, "SD file handler invoked for URI: %s", req->uri);
    #if FALSE // This seemed to have no effect, temporarily removed, to be looked into later
    // Handle captive portal detection URLs from various operating systems
    // Android: /generate_204, /gen_204
    // iOS: /hotspot-detect.html
    // Windows: /ncsi.txt, /connecttest.txt
    // Generic: /success.txt, /redirect, /204
    if (!strcmp(req->uri, "/generate_204") ||
        !strcmp(req->uri, "/gen_204") ||
        !strcmp(req->uri, "/ncsi.txt") ||
        !strcmp(req->uri, "/connecttest.txt") ||
        !strcmp(req->uri, "/hotspot-detect.html") ||
        !strcmp(req->uri, "/success.txt") ||
        !strcmp(req->uri, "/redirect") ||
        !strcmp(req->uri, "/204") ||
        !strcmp(req->uri, "/ipv6check")) {
        
        ESP_LOGV(TAG, "Captive portal detection request: %s", req->uri);
        
        // Extract client IP address from socket for redirect tracking
        uint32_t client_ip = 0;
        int sockfd = httpd_req_to_sockfd(req);
        
        if (sockfd >= 0) {
            struct sockaddr_storage addr;
            socklen_t addr_len = sizeof(addr);
            
            if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) == 0) {
                if (addr.ss_family == AF_INET) {
                    // IPv4
                    struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
                    client_ip = addr_in->sin_addr.s_addr;
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &(addr_in->sin_addr), ip_str, INET_ADDRSTRLEN);
                    ESP_LOGV(TAG, "Client IPv4 obtained: %s (0x%08X)", ip_str, (unsigned int)client_ip);
                } else if (addr.ss_family == AF_INET6) {
                    // IPv6 - use hash of last 4 bytes as identifier
                    struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)&addr;
                    memcpy(&client_ip, &addr_in6->sin6_addr.s6_addr[12], 4);
                    char ip_str[INET6_ADDRSTRLEN];
                    inet_ntop(AF_INET6, &(addr_in6->sin6_addr), ip_str, INET6_ADDRSTRLEN);
                    ESP_LOGV(TAG, "Client IPv6 obtained: %s (hash: 0x%08X)", ip_str, (unsigned int)client_ip);
                } else {
                    ESP_LOGW(TAG, "Unknown address family: %d", addr.ss_family);
                }
            } else {
                ESP_LOGW(TAG, "getpeername failed: errno=%d (%s)", errno, strerror(errno));
            }
        } else {
            ESP_LOGW(TAG, "Invalid socket fd: %d", sockfd);
        }
        
        // Determine response based on request type and redirect tracking
        // For Microsoft NCSI (Windows) - return the exact expected content
        if (!strcmp(req->uri, "/ncsi.txt") || !strcmp(req->uri, "/connecttest.txt")) {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Microsoft NCSI", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        
        // First request from this IP: redirect to portal to open popup/browser
        // Subsequent requests from same IP: return 204 to indicate internet connectivity
        if ((client_ip != 0 && !is_ip_redirected(client_ip))||!strcmp(req->uri, "/redirect")) {
            mark_ip_redirected(client_ip);
            
            char location[64];
            bool use_mDNS;
            char mDNS_hostname[32];
            char service_name[32];
            get_mdns_config(&use_mDNS, mDNS_hostname, sizeof(mDNS_hostname), service_name, sizeof(service_name));            
            if (use_mDNS) {
                snprintf(location, sizeof(location), "http://%s.local/", mDNS_hostname);
            } else {
                esp_netif_ip_info_t ip_info;
                esp_netif_get_ip_info(ap_netif, &ip_info);
                char ip_addr[16];
                inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
                snprintf(location, sizeof(location), "http://%s/", ip_addr);
            }
            
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", location);
            httpd_resp_send(req, NULL, 0);
            ESP_LOGI(TAG, "First captive detection, redirecting to %s", location);
            return ESP_OK;
        }
        
        // Subsequent requests: return 204
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    #endif

    #if CONFIG_WIFI_SD_FILE_SERVING_MODE_SPA
    bool is_navigation_request = true;
    const char *last_slash = strrchr(req->uri, '/');
    const char *last_dot = strrchr(req->uri, '.');
    if (last_dot && (!last_slash || last_dot > last_slash)) {
        ESP_LOGD(TAG, "Found an asset extension in URI: %s", req->uri);
        is_navigation_request = false;
    }
    ssize_t accept_len = httpd_req_get_hdr_value_len(req, "Accept");
    if (accept_len > 0) {
        char *accept = malloc(accept_len + 1);
        if (httpd_req_get_hdr_value_str(req, "Accept", accept, accept_len + 1) == ESP_OK) {
            // If Accept explicitly excludes HTML (no "text/html" and no "*/*"), not a navigation request
            if (strstr(accept, "text/html") == NULL && strstr(accept, "*/*") == NULL) {
                ESP_LOGD(TAG, "Accept header does not include text/html: %s", accept);
                is_navigation_request = false;
            }
        }
        free(accept);
    }
    if (is_navigation_request) {
        ESP_LOGD(TAG, "Handling as navigation request, serving index.html for URI: %s", req->uri);
        
        esp_err_t r = send_sd_file(req, "/index.html");
        if (r != ESP_OK) {
            ESP_LOGD(TAG, "Served index.html for navigation request");
            return not_found_handler(req, HTTPD_404_NOT_FOUND);
        }
        return ESP_OK;
    }
    #endif


    #if CONFIG_WIFI_SD_FILE_SERVING_MODE_STATIC
    // Construct full filesystem path from URI
    char filepath[530];
    snprintf(filepath, sizeof(filepath), "%s%s", get_sd_card_mount_point(), req->uri);

    // Handle directory requests by appending index.html
    struct stat st;
    size_t len = strlen(filepath);
    if (stat(filepath, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (len > 0 && filepath[len - 1] == '/') {
            snprintf(filepath + len, sizeof(filepath) - len, "index.html");
        } else {
            snprintf(filepath + len, sizeof(filepath) - len, "/index.html");
        }
    } else if (!strchr(req->uri, '.') && stat(filepath, &st) != 0) {
        // If URI has no extension and file doesn't exist, try adding .html
        snprintf(filepath + len, sizeof(filepath) - len, ".html");
    }
    #endif

    esp_err_t r = send_sd_file(req, req->uri);
    if (r == ESP_OK) {
        ESP_LOGD(TAG, "Served file: %s", req->uri);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed to serve file: %s, error: %d", req->uri, r);
        return not_found_handler(req, HTTPD_404_NOT_FOUND);
    }
}

esp_err_t register_runtime_handlers(bool sd_card_present) {
    esp_log_level_set(TAG, CONFIG_LOG_LEVEL_WIFI);

    ESP_LOGD(TAG, "Registering runtime HTTP handlers (SD card present: %s)", sd_card_present ? "yes" : "no");
    
    ESP_RETURN_ON_ERROR(server_mgr_register_err_handler(HTTPD_404_NOT_FOUND, not_found_handler), TAG, "Failed to register 404 handler");

    // Register captive portal HTTP handlers (on /captive_portal for STA mode)
    ESP_RETURN_ON_ERROR(register_captive_portal_handlers(), TAG, "Failed to register captive portal handlers");

    httpd_uri_t index_html_uri = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = index_html_get_handler
    };
    ESP_RETURN_ON_ERROR(server_mgr_register_handler(&index_html_uri), TAG, "Failed to register /index.html handler");

    httpd_uri_t wifi_status_json_uri = {
        .uri = "/wifi-status.json",
        .method = HTTP_GET,
        .handler = wifi_status_json_handler,
    };
    ESP_RETURN_ON_ERROR(server_mgr_register_handler(&wifi_status_json_uri), TAG, "Failed to register /wifi-status.json handler");

    httpd_uri_t restart_uri = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = restart_handler
    };
    ESP_RETURN_ON_ERROR(server_mgr_register_handler(&restart_uri), TAG, "Failed to register /restart handler");

    if (sd_card_present) {
        // Register custom handlers
        ESP_RETURN_ON_ERROR(register_custom_http_handlers(), TAG, "Failed to register custom HTTP handlers");

        httpd_uri_t sd_file_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = sd_file_handler
        };
        ESP_RETURN_ON_ERROR(server_mgr_register_handler(&sd_file_uri), TAG, "Failed to register /* handler");
        
        #if CONFIG_WIFI_SD_FILE_SERVING_MODE_SPA
        httpd_uri_t spa_sd_file_uri = {
            .uri = "/*",
            .method = HTTP_HEAD,
            .handler = sd_file_handler
        };
        ESP_RETURN_ON_ERROR(server_mgr_register_handler(&spa_sd_file_uri), TAG, "Failed to register /* HEAD handler");
        #endif

    } else {
        // need to run wildcard handler even if no SD card to have captive redirect in AP mode
        httpd_uri_t no_sd_card_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = no_sd_card_handler
        };
        ESP_RETURN_ON_ERROR(server_mgr_register_handler(&no_sd_card_uri), TAG, "Failed to register /* handler for no SD card");
    }
    return ESP_OK;
}