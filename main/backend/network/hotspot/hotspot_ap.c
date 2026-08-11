/**
 * hotspot_ap.c — WiFi SoftAP + NAPT internet-sharing lifecycle
 *
 * See hotspot_ap.h for the full design — in particular why this needs
 * zero changes to app_main.c's existing WiFi STA init (the
 * esp_wifi_set_mode(STA)<->esp_wifi_set_mode(APSTA) promotion trick).
 *
 * DNS/DHCP setup sequence (esp_netif_dhcps_stop -> set IP -> enable DNS
 * offer flag -> esp_netif_dhcps_start -> set DNS servers) matches the
 * proven, already-working esp32s3_4g_hotspotWorkingClaude reference
 * project's wifi_manager.c exactly (docs 14/15) — not reinvented.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"   // wifi_sta_mac_ip_list_t / esp_wifi_ap_get_sta_list_with_ip() — NOT declared by esp_wifi.h itself, a separate header
#include "esp_mac.h"
#include "lwip/ip4_addr.h"
#include "lwip/dns.h"
#include "config.h"
#include "hotspot_nvs.h"
#include "hotspot_ap.h"

static const char *TAG = "hotspot_ap";

static esp_netif_t *s_ap_netif   = NULL;   // created once, reused across stop/start cycles
static bool         s_ap_created = false;
static bool         s_running    = false;
static bool         s_napt_on    = false;

// MAC<->AID tracking — esp_wifi_deauth_sta() (the only "kick" primitive
// ESP-IDF exposes) takes an Association ID, NOT a MAC address, and
// wifi_sta_info_t (from esp_wifi_ap_get_sta_list()) does not carry the
// AID either — only the STACONNECTED/STADISCONNECTED *events* carry
// both together (wifi_event_ap_staconnected_t.aid, verified against the
// real IDF 5.4 header, esp_wifi_types_generic.h). So this small table is
// populated/cleared from those events and is the only way to correlate
// "kick by MAC" (what a human types) to "kick by AID" (what the API
// needs).
typedef struct {
    uint8_t mac[6];
    uint8_t aid;
    bool    in_use;
} _mac_aid_entry_t;
#define MAC_AID_TABLE_SIZE 10   // matches ESP_WIFI_MAX_CONN_NUM's practical ceiling
static _mac_aid_entry_t s_mac_aid[MAC_AID_TABLE_SIZE];

static void _mac_aid_add(const uint8_t mac[6], uint8_t aid) {
    for (int i = 0; i < MAC_AID_TABLE_SIZE; i++) {
        if (!s_mac_aid[i].in_use) {
            memcpy(s_mac_aid[i].mac, mac, 6);
            s_mac_aid[i].aid = aid;
            s_mac_aid[i].in_use = true;
            return;
        }
    }
    ESP_LOGW(TAG, "MAC/AID table full (%d) — kick-by-MAC won't find this client until it reconnects", MAC_AID_TABLE_SIZE);
}

static void _mac_aid_remove(const uint8_t mac[6]) {
    for (int i = 0; i < MAC_AID_TABLE_SIZE; i++) {
        if (s_mac_aid[i].in_use && memcmp(s_mac_aid[i].mac, mac, 6) == 0) {
            s_mac_aid[i].in_use = false;
            return;
        }
    }
}

static bool _mac_aid_lookup(const uint8_t mac[6], uint8_t *out_aid) {
    for (int i = 0; i < MAC_AID_TABLE_SIZE; i++) {
        if (s_mac_aid[i].in_use && memcmp(s_mac_aid[i].mac, mac, 6) == 0) {
            *out_aid = s_mac_aid[i].aid;
            return true;
        }
    }
    return false;
}

static void _on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(e->mac));
        ESP_LOGI(TAG, "Hotspot client connected: %s (aid=%d)", mac_str, e->aid);
        _mac_aid_add(e->mac, e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(e->mac));
        ESP_LOGI(TAG, "Hotspot client disconnected: %s (reason=%d)", mac_str, e->reason);
        _mac_aid_remove(e->mac);
    }
}

// ── DNS helpers — same shape as the proven reference project's own ──
static void _enable_dhcp_dns_offer(esp_netif_t *netif) {
    uint8_t dns_offer = 1;
    esp_err_t ret = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                            &dns_offer, sizeof(uint8_t));
    if (ret != ESP_OK) ESP_LOGW(TAG, "DHCP DNS-offer flag: %s", esp_err_to_name(ret));
}

static void _set_dns_servers(esp_netif_t *netif, const char *primary_str, const char *secondary_str) {
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;

    dns.ip.u_addr.ip4.addr = ipaddr_addr(primary_str);
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);

    dns.ip.u_addr.ip4.addr = ipaddr_addr(secondary_str);
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns);

    ip_addr_t lwip_p, lwip_s;
    ipaddr_aton(primary_str, &lwip_p);
    ipaddr_aton(secondary_str, &lwip_s);
    dns_setserver(0, &lwip_p);
    dns_setserver(1, &lwip_s);

    ESP_LOGI(TAG, "Hotspot DNS: %s (primary), %s (secondary)", primary_str, secondary_str);
}

esp_err_t hotspot_ap_init(void) {
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, _on_ap_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, _on_ap_event, NULL);
    ESP_LOGI(TAG, "Hotspot module ready (AP event handlers registered)");

    // doc 169 — auto-start reflects the PERSISTED "enabled" preference
    // (hotspot_nvs.c), not a build-time-only flag, so a hotspot you
    // turned on stays on across a power cycle instead of reverting to
    // off every boot. "hotspot on"/"hotspot off" are what change this
    // going forward — see hotspot_nvs_set_enabled()'s own comment.
    hotspot_config_t cfg;
    hotspot_nvs_get(&cfg);
    if (cfg.enabled) {
        ESP_LOGI(TAG, "Hotspot was ON last time (persisted) — starting automatically");
        return hotspot_ap_start();
    }
    ESP_LOGI(TAG, "Hotspot off — 'hotspot on' or the GUI's toggle to start it");
    return ESP_OK;
}

esp_err_t hotspot_ap_start(void) {
    if (s_running) {
        ESP_LOGI(TAG, "hotspot already running — no-op");
        return ESP_OK;
    }

    hotspot_config_t cfg;
    hotspot_nvs_get(&cfg);

    if (!s_ap_created) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap() failed");
            return ESP_FAIL;
        }
        s_ap_created = true;
    }

    esp_netif_dhcps_stop(s_ap_netif);

    esp_netif_ip_info_t ip_info = {0};
    ip4addr_aton(HOTSPOT_AP_IP, (ip4_addr_t *)&ip_info.ip);
    ip4addr_aton(HOTSPOT_AP_IP, (ip4_addr_t *)&ip_info.gw);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);

    _enable_dhcp_dns_offer(s_ap_netif);
    esp_netif_dhcps_start(s_ap_netif);
    _set_dns_servers(s_ap_netif, HOTSPOT_DNS_PRIMARY, HOTSPOT_DNS_SECONDARY);

    // Promote STA -> APSTA at runtime — see hotspot_ap.h's header
    // comment for why this is safe and why it's the whole reason this
    // module needs no changes to app_main.c's existing STA init.
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.ap.ssid, cfg.ssid, sizeof(wifi_cfg.ap.ssid));
    wifi_cfg.ap.ssid_len = strlen(cfg.ssid);
    strlcpy((char *)wifi_cfg.ap.password, cfg.password, sizeof(wifi_cfg.ap.password));
    wifi_cfg.ap.channel        = cfg.channel;
    wifi_cfg.ap.max_connection = cfg.max_clients;
    wifi_cfg.ap.ssid_hidden    = cfg.hidden ? 1 : 0;
    wifi_cfg.ap.authmode       = (strlen(cfg.password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s — check password length (8-63 chars for WPA2)", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);   // undo the promotion — don't leave APSTA half-configured
        return err;
    }

    s_running = true;
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "HOTSPOT STARTED — ssid=\"%s\" channel=%d ip=%s max_clients=%d",
             cfg.ssid, cfg.channel, HOTSPOT_AP_IP, cfg.max_clients);
    ESP_LOGI(TAG, "  (NAT not yet enabled — net_manager.c enables it once an uplink is ready)");
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return ESP_OK;
}

esp_err_t hotspot_ap_stop(void) {
    if (!s_running) {
        ESP_LOGI(TAG, "hotspot already stopped — no-op");
        return ESP_OK;
    }
    hotspot_ap_disable_napt();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);   // demote — AP interface goes away, STA untouched
    s_running = false;
    ESP_LOGI(TAG, "HOTSPOT STOPPED");
    return err;
}

bool hotspot_ap_is_running(void) { return s_running; }
esp_netif_t *hotspot_ap_get_netif(void) { return s_ap_netif; }

esp_err_t hotspot_ap_enable_napt(esp_netif_t *uplink_netif) {
    if (!s_running || !s_ap_netif) {
        ESP_LOGI(TAG, "enable_napt: hotspot not running — nothing to route yet");
        return ESP_ERR_INVALID_STATE;
    }
    if (!uplink_netif) {
        ESP_LOGW(TAG, "enable_napt: no uplink netif given");
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_set_default_netif(uplink_netif);
    esp_err_t ret = esp_netif_napt_enable(s_ap_netif);
    if (ret == ESP_OK) {
        s_napt_on = true;
        ESP_LOGI(TAG, "NAPT enabled — hotspot clients now routed through the current uplink");
    } else {
        ESP_LOGE(TAG, "NAPT enable failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void hotspot_ap_disable_napt(void) {
    if (!s_napt_on || !s_ap_netif) return;
    esp_netif_napt_disable(s_ap_netif);
    s_napt_on = false;
    ESP_LOGI(TAG, "NAPT disabled");
}

void hotspot_ap_update_dns(esp_netif_t *uplink_netif) {
    if (!s_ap_netif || !uplink_netif) return;

    esp_netif_dns_info_t uplink_dns = {0};
    if (esp_netif_get_dns_info(uplink_netif, ESP_NETIF_DNS_MAIN, &uplink_dns) != ESP_OK
        || uplink_dns.ip.u_addr.ip4.addr == 0
        || uplink_dns.ip.u_addr.ip4.addr == 0xFFFFFFFF) {
        return;   // uplink has no usable DNS of its own yet — keep the static fallback
    }

    char uplink_dns_str[16];
    snprintf(uplink_dns_str, sizeof(uplink_dns_str), IPSTR, IP2STR(&uplink_dns.ip.u_addr.ip4));
    ESP_LOGI(TAG, "Uplink DNS %s available — prepending to hotspot DHCP", uplink_dns_str);
    _set_dns_servers(s_ap_netif, uplink_dns_str, HOTSPOT_DNS_PRIMARY);
}

int hotspot_ap_get_client_count(void) {
    if (!s_running) return 0;
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
    return list.num;
}

int hotspot_ap_get_clients(hotspot_client_t *out, int max_count) {
    if (!out || max_count <= 0 || !s_running) return 0;

    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) return 0;

    wifi_sta_mac_ip_list_t ip_list = {0};
    bool have_ip = (esp_wifi_ap_get_sta_list_with_ip(&sta_list, &ip_list) == ESP_OK);

    int n = 0;
    for (int i = 0; i < sta_list.num && n < max_count; i++) {
        memcpy(out[n].mac, sta_list.sta[i].mac, 6);
        out[n].rssi = sta_list.sta[i].rssi;
        out[n].ip.addr = 0;
        if (have_ip) {
            for (int j = 0; j < ip_list.num; j++) {
                if (memcmp(ip_list.sta[j].mac, sta_list.sta[i].mac, 6) == 0) {
                    out[n].ip = ip_list.sta[j].ip;
                    break;
                }
            }
        }
        n++;
    }
    return n;
}

esp_err_t hotspot_ap_kick_client(const uint8_t mac[6]) {
    uint8_t aid;
    if (!_mac_aid_lookup(mac, &aid)) {
        ESP_LOGW(TAG, "kick: MAC " MACSTR " not currently associated (or connected before this boot)", MAC2STR(mac));
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_wifi_deauth_sta(aid);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Kicked client " MACSTR " (aid=%d)", MAC2STR(mac), aid);
    } else {
        ESP_LOGE(TAG, "Kick failed for " MACSTR ": %s", MAC2STR(mac), esp_err_to_name(err));
    }
    return err;
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS
// ═══════════════════════════════════════════════════════════════
static void _show_help(void) {
    printf("\n  hotspot on / off                    Start/stop the AP\n");
    printf("  hotspot status                       SSID/channel/clients/NAPT state\n");
    printf("  hotspot ssid <name>                  Change SSID (restarts AP if running)\n");
    printf("  hotspot passwd <old> <new>            Change password — old required\n");
    printf("  hotspot clients                       Connected devices: MAC + IP + RSSI\n");
    printf("  hotspot kick <AA:BB:CC:DD:EE:FF>       Disconnect one client\n");
    printf("  hotspot reset-credentials <passcode>   Factory-reset SSID/password\n");
    printf("  hotspot help\n\n");
}

static bool _parse_mac(const char *s, uint8_t mac[6]) {
    unsigned int b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    return true;
}

static void _restart_ap_if_running(void) {
    if (s_running) {
        ESP_LOGI(TAG, "Restarting AP to apply the change...");
        hotspot_ap_stop();
        hotspot_ap_start();
    }
}

bool hotspot_ap_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "hotspot", 7) != 0) return false;

    const char *p = line + 7;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "help") == 0) {
        _show_help();
    } else if (strcmp(p, "on") == 0) {
        hotspot_ap_start();
        hotspot_nvs_set_enabled(true);    // doc 169 — persists so this survives a power cycle
    } else if (strcmp(p, "off") == 0) {
        hotspot_ap_stop();
        hotspot_nvs_set_enabled(false);
    } else if (strcmp(p, "status") == 0) {
        hotspot_config_t cfg;
        hotspot_nvs_get(&cfg);
        ESP_LOGI(TAG, "══════════════════════════════════════");
        ESP_LOGI(TAG, "HOTSPOT STATUS");
        ESP_LOGI(TAG, "  Running:      %s", s_running ? "yes" : "no");
        ESP_LOGI(TAG, "  SSID:         %s%s", cfg.ssid, cfg.hidden ? " (hidden)" : "");
        ESP_LOGI(TAG, "  Channel:      %d", cfg.channel);
        ESP_LOGI(TAG, "  Max clients:  %d", cfg.max_clients);
        ESP_LOGI(TAG, "  IP:           %s", HOTSPOT_AP_IP);
        ESP_LOGI(TAG, "  Clients now:  %d", hotspot_ap_get_client_count());
        ESP_LOGI(TAG, "  NAPT:         %s", s_napt_on ? "enabled (internet sharing active)" : "disabled (no uplink routed yet)");
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
    } else if (strncmp(p, "ssid", 4) == 0) {
        const char *name = p + 4;
        while (*name == ' ') name++;
        if (*name == '\0') { printf("Usage: hotspot ssid <name>\n"); return true; }
        if (hotspot_nvs_set_ssid(name) == ESP_OK) _restart_ap_if_running();
    } else if (strncmp(p, "passwd", 6) == 0) {
        char old_pw[65] = {0}, new_pw[65] = {0};
        if (sscanf(p + 6, "%64s %64s", old_pw, new_pw) != 2) {
            printf("Usage: hotspot passwd <old> <new>\n");
            return true;
        }
        if (hotspot_nvs_change_password(old_pw, new_pw) == ESP_OK) {
            _restart_ap_if_running();
        } else {
            printf("Password change refused — check the current password and that the new one is 8-63 characters.\n");
        }
    } else if (strcmp(p, "clients") == 0) {
        hotspot_client_t clients[MAC_AID_TABLE_SIZE];
        int n = hotspot_ap_get_clients(clients, MAC_AID_TABLE_SIZE);
        printf("\n%-20s %-16s %s\n", "MAC", "IP", "RSSI");
        for (int i = 0; i < n; i++) {
            char ip_str[16] = "(pending)";
            if (clients[i].ip.addr != 0) snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&clients[i].ip));
            printf("%02X:%02X:%02X:%02X:%02X:%02X   %-16s %d dBm\n",
                   clients[i].mac[0], clients[i].mac[1], clients[i].mac[2],
                   clients[i].mac[3], clients[i].mac[4], clients[i].mac[5], ip_str, clients[i].rssi);
        }
        printf("(%d client(s))\n\n", n);
    } else if (strncmp(p, "kick", 4) == 0) {
        const char *mac_str = p + 4;
        while (*mac_str == ' ') mac_str++;
        uint8_t mac[6];
        if (!_parse_mac(mac_str, mac)) {
            printf("Usage: hotspot kick <AA:BB:CC:DD:EE:FF>\n");
        } else {
            hotspot_ap_kick_client(mac);
        }
    } else if (strncmp(p, "reset-credentials", 18) == 0) {
        const char *passcode = p + 18;
        while (*passcode == ' ') passcode++;
        if (strcmp(passcode, FACTORY_RESET_PASSCODE) != 0) {
            ESP_LOGW(TAG, "reset-credentials: wrong passcode — refused");
        } else {
            hotspot_nvs_reset_to_defaults();
            _restart_ap_if_running();
        }
    } else {
        printf("Unknown 'hotspot' subcommand. Type 'hotspot help'.\n");
    }
    return true;
}
