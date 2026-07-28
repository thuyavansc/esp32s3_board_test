/**
 * net_diag.c — WiFi status + real internet-reachability check (see
 * net_diag.h for the full design rationale).
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "config.h"
#include "net_diag.h"
#include "network/net_manager.h"   // Phase 1 (doc 155/158) — "net uplink/status/mem" forward here.
                                    // Kept as ONE "net" prefix owner (this file) rather than a
                                    // second competing dispatcher — see net_manager.h's own comment.

static const char *TAG = "net_diag";

// The exact technique Android/ChromeOS/Chrome itself use to tell
// "associated to an AP" apart from "actually has a working path to the
// internet": a tiny, stable, ad-free Google endpoint that returns
// HTTP 204 with an EMPTY body only when the request genuinely reaches
// the real internet (DNS resolves, TCP connects, TLS handshakes, no
// captive portal intercepting it and returning its own login page).
#define NET_TEST_URL "https://www.google.com/generate_204"

static void _print_help(void) {
    printf("Net Commands: net info | net test | net uplink wifi|cellular|auto | net status | net mem | net help\n");
}

// Phase 1 (doc 155/158) — network-stack-specific memory footprint,
// distinct from diag.c's general "mem" command: this ONLY breaks out
// what net_manager/cellular_ppp/hotspot_ap's own state is (they don't
// hold large buffers themselves — USB host/lwIP/WiFi driver internals
// account for nearly everything), so this is really "did enabling the
// network stack cost us what we expected" — best read side-by-side
// with a "mem" reading taken before Phase 1 was enabled.
static void _cmd_net_mem(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "NETWORK STACK MEMORY (informational — see 'mem' for the full picture)");
    ESP_LOGI(TAG, "──────────────────────────────────────");
    ESP_LOGI(TAG, "  Internal SRAM free:      %6u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ESP_LOGI(TAG, "  Internal SRAM largest:   %6u KB  (USB host driver needs a sizeable contiguous block)",
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    ESP_LOGI(TAG, "  Compare against a 'mem' reading taken BEFORE cellular/hotspot were enabled");
    ESP_LOGI(TAG, "  (docs/TestFunctionalities/esp32s3_board/internet--hotsport-sms/");
    ESP_LOGI(TAG, "   160_..._ram_buffer_size_list.md) to see exactly what Phase 1 cost.");
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

static void _cmd_info(void) {
#if NETWORK_NEEDED
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "NETWORK STATUS");
    ESP_LOGI(TAG, "──────────────────────────────────────");

    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(TAG, "  Device MAC (STA):  %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "  Connected SSID:    %s", ap.ssid);
        ESP_LOGI(TAG, "  AP BSSID:          %02X:%02X:%02X:%02X:%02X:%02X",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        ESP_LOGI(TAG, "  RSSI:              %d dBm", ap.rssi);
        ESP_LOGI(TAG, "  Channel:           %d", ap.primary);
    } else {
        ESP_LOGW(TAG, "  Not associated to any AP right now (esp_wifi_sta_get_ap_info failed)");
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            ESP_LOGI(TAG, "  Local IP:          " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "  Netmask:           " IPSTR, IP2STR(&ip_info.netmask));
            ESP_LOGI(TAG, "  Gateway:           " IPSTR, IP2STR(&ip_info.gw));
        } else {
            ESP_LOGW(TAG, "  No IP yet (not connected)");
        }

        esp_netif_dns_info_t dns = {0};
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            ESP_LOGI(TAG, "  DNS server:        " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
    } else {
        ESP_LOGW(TAG, "  No WIFI_STA_DEF netif — WiFi station not initialized");
    }

    ESP_LOGI(TAG, "══════════════════════════════════════\n");
#else
    ESP_LOGW(TAG, "WiFi is disabled in this build (ENABLE_WIFI=0 in config.h) — nothing to report.");
#endif
}

static void _cmd_test(void) {
#if NETWORK_NEEDED
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "INTERNET CONNECTIVITY TEST");
    ESP_LOGI(TAG, "  GET %s", NET_TEST_URL);
    ESP_LOGI(TAG, "  (Same trick phones/laptops use to tell \"connected to WiFi\"");
    ESP_LOGI(TAG, "   apart from \"WiFi actually reaches the internet\" — a real HTTPS");
    ESP_LOGI(TAG, "   request to a public endpoint, not just a link/ARP check.)");
    ESP_LOGI(TAG, "──────────────────────────────────────");

    esp_http_client_config_t config = {
        .url = NET_TEST_URL,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int64_t content_len = esp_http_client_get_content_length(client);
        // generate_204 returns exactly HTTP 204 with an empty body when
        // the internet path genuinely works — anything else (including
        // a 200 with an HTML body) usually means a captive portal or
        // proxy intercepted the request instead of it reaching Google.
        bool ok = (status == 204);
        ESP_LOGI(TAG, "  Result:            %s", ok ? "\xE2\x9C\x93 INTERNET OK" : "\xE2\x9A\xA0 UNEXPECTED RESPONSE (possible captive portal / proxy)");
        ESP_LOGI(TAG, "  HTTP status:       %d", status);
        ESP_LOGI(TAG, "  Content length:    %lld bytes", (long long)content_len);
        ESP_LOGI(TAG, "  Round-trip time:   %lld ms", (long long)elapsed_ms);
    } else {
        ESP_LOGE(TAG, "  Result:            \xE2\x9C\x97 NO INTERNET (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "  DNS failed, TCP connect failed, or TLS handshake failed — run");
        ESP_LOGE(TAG, "  'net info' first to confirm WiFi is even associated with a valid");
        ESP_LOGE(TAG, "  IP before assuming this is a deeper problem.");
    }

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
#else
    ESP_LOGW(TAG, "WiFi is disabled in this build (ENABLE_WIFI=0 in config.h) — cannot test internet.");
#endif
}

bool net_diag_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "net", 3) != 0) return false;

    const char *p = line + 3;
    while (*p == ' ') p++;

    if (strcmp(p, "info") == 0) { _cmd_info(); return true; }
    if (strcmp(p, "test") == 0) { _cmd_test(); return true; }
    if (strcmp(p, "help") == 0 || *p == '\0') { _print_help(); return true; }
    if (strcmp(p, "status") == 0) { net_manager_print_status(); return true; }
    if (strcmp(p, "mem") == 0) { _cmd_net_mem(); return true; }

    if (strncmp(p, "uplink", 6) == 0) {
        const char *arg = p + 6;
        while (*arg == ' ') arg++;
        // NOTE: net_manager_set_uplink(CELLULAR) can block up to ~30s
        // dialling — this runs on the "serial_cmd" task, which is fine
        // for a TYPED command (the user is already waiting for a
        // response), unlike an automatic boot-time call which routes
        // through bg_worker instead (net_manager.c's own init job).
        if (strcmp(arg, "wifi") == 0) {
            net_manager_set_uplink(NET_UPLINK_WIFI);
        } else if (strcmp(arg, "cellular") == 0) {
            net_manager_set_uplink(NET_UPLINK_CELLULAR);
        } else if (strcmp(arg, "auto") == 0) {
            net_manager_set_uplink_auto();
        } else {
            printf("Usage: net uplink wifi|cellular|auto\n");
        }
        return true;
    }

    ESP_LOGW(TAG, "Unknown 'net' command: '%s'. Try: info | test | uplink | status | mem | help", p);
    return true;
}
