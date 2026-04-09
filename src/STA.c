#include "STA.h"

#include "sdkconfig.h"

#include "Server-mgr.h"
#include "Runtime-handlers.h"
#include "SD-mgr.h"
#include "Captive.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"
#include "esp_check.h"

#include "mdns.h"
#include "lwip/inet.h"
#include "esp_netif.h"

static const char *TAG = "Wifi: STA";

extern esp_netif_t *sta_netif;

/**
 * @brief Initialize WiFi in station (client) mode.
 * 
 * Connects to the configured WiFi network, starts the HTTP server,
 * registers handlers, and optionally starts mDNS.
 */
esp_err_t wifi_init_sta() {
    esp_log_level_set(TAG, CONFIG_LOG_LEVEL_WIFI);

    ESP_LOGI(TAG, "Starting WiFi in station mode...");
    
    wifi_config_t wifi_cfg = get_sta_wifi_config();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set WiFi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "Failed to set WiFi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi");
    
    // Set static IP if requested
    
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_dhcpc_stop(sta_netif);
    bool use_static_ip;
    esp_ip4_addr_t ip_addr;
    get_static_ip_config(&use_static_ip, &ip_addr);
    if (use_static_ip) {
        uint32_t new_ip = ntohl(ip_addr.addr);
        ip_info.ip.addr = ip_addr.addr;
        ip_info.gw.addr = htonl((new_ip & 0xFFFFFF00)|0x01);    // x.x.x.1
        ip_info.netmask.addr = htonl((255 << 24) | (255 << 16) | (255 << 8) | 0);   // 255.255.255.0
        esp_netif_set_ip_info(sta_netif, &ip_info);
    } else {
        esp_netif_dhcpc_start(sta_netif);
    }
    
    // Log IP address
    esp_netif_get_ip_info(sta_netif, &ip_info);
    char ip_addr_str[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr_str, 16);
    ESP_LOGD(TAG, "Set up STA with IP: %s", ip_addr_str);

    ESP_LOGD(TAG, "Starting web server on port: %d", server_mgr_get_port());
    ESP_RETURN_ON_ERROR(server_mgr_start(), TAG, "Failed to start web server");

    ESP_RETURN_ON_ERROR(register_runtime_handlers(is_sd_card_present()), TAG, "Failed to register runtime handlers");

    bool use_mDNS;
    char mDNS_hostname[32];
    char service_name[32];
    get_mdns_config(&use_mDNS, mDNS_hostname, sizeof(mDNS_hostname), service_name, sizeof(service_name));
    // Start mDNS if enabled
    if (use_mDNS) {
        ESP_RETURN_ON_ERROR(mdns_init(), TAG, "Failed to initialize mDNS");
        ESP_RETURN_ON_ERROR(mdns_hostname_set(mDNS_hostname), TAG, "Failed to set mDNS hostname");
        ESP_RETURN_ON_ERROR(mdns_instance_name_set(service_name), TAG, "Failed to set mDNS instance name");
        ESP_LOGI(TAG, "mDNS started: http://%s.local", mDNS_hostname);
        ESP_LOGI(TAG, "mDNS service started: %s", service_name);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    return ESP_OK;
}
