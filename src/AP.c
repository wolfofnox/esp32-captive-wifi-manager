#include "AP.h"

#include "sdkconfig.h"

#include "Server-mgr.h"
#include "Runtime-handlers.h"
#include "SD-mgr.h"
#include "Captive.h"
#include "Wifi.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"

#include "mdns.h"
#include "lwip/inet.h"
#include "esp_netif.h"

static const char *TAG = "Wifi: AP";

/**
 * @brief Initialize WiFi in AP mode.
 *
 * This is a stub and will be implemented later.
 */
esp_err_t wifi_init_ap() {
    esp_log_level_set(TAG, CONFIG_LOG_LEVEL_WIFI);

    ESP_LOGI(TAG, "Starting WiFi in access point mode...");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Failed to set WiFi mode");
    wifi_config_t wifi_cfg;
    get_ap_wifi_config(&wifi_cfg);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg), TAG, "Failed to set WiFi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi");
    
    int8_t max_tx_power;
    ESP_RETURN_ON_ERROR(esp_wifi_get_max_tx_power(&max_tx_power), TAG, "Failed to get max TX power");
    ESP_LOGI(TAG, "Max TX power is %d, setting to 44 (11dBm) for AP mode", max_tx_power);
    ESP_RETURN_ON_ERROR(esp_wifi_set_max_tx_power(44), TAG, "Failed to set max TX power"); // 44 = 11dBm (~5-10m range)

    // Configure AP IP address
    esp_netif_ip_info_t ip_info = {0};
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(wifi_get_ap_netif()), TAG, "Failed to stop DHCP server");  // Stop DHCP SERVER
    bool use_static_ip;
    esp_ip4_addr_t ip_addr;
    get_static_ip_config(&use_static_ip, &ip_addr);
    if (use_static_ip) {
        uint32_t new_ip = ntohl(ip_addr.addr);
        ip_info.ip.addr = ip_addr.addr;
        ip_info.gw.addr = htonl((new_ip & 0xFFFFFF00)|0x01);    // x.x.x.1
        ip_info.netmask.addr = htonl((255 << 24) | (255 << 16) | (255 << 8) | 0);   // 255.255.255.0
    } else {
        // Default AP IP: 192.168.4.1
        ip_info.ip.addr = htonl((192 << 24) | (168 << 16) | (4 << 8) | 1);
        ip_info.gw.addr = ip_info.ip.addr;
        ip_info.netmask.addr = htonl((255 << 24) | (255 << 16) | (255 << 8) | 0);
    }
    
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(wifi_get_ap_netif(), &ip_info), TAG, "Failed to set AP IP info");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(wifi_get_ap_netif()), TAG, "Failed to start DHCP server");  // Start DHCP SERVER
    
    // Log IP address
    char ip_addr_str[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr_str, 16);
    ESP_LOGI(TAG, "Set up AP with IP: %s", ip_addr_str);

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