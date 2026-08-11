/**
 * net_manager.c — Uplink selection (WiFi <-> Cellular)
 *
 * See net_manager.h for the full design. This is the only module that
 * knows about both cellular_ppp.c and hotspot_ap.c.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "config.h"
#include "cellular/cellular_ppp.h"
#include "hotspot/hotspot_ap.h"
#include "bg_worker.h"
#include "net_manager.h"

static const char *TAG = "net_mgr";
static net_uplink_t s_active = NET_UPLINK_NONE;
static bool s_prefer_cellular_auto = false;   // set true only by net_manager_set_uplink_auto() — see the health-check timer below

static esp_netif_t *_wifi_sta_netif(void) {
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

// doc 164 §2 — lwIP's DNS resolver is a SINGLE GLOBAL table, not
// per-netif, even though esp_netif itself stores a DNS record on each
// netif independently. Whichever interface last pushed its own DNS
// server into that shared table wins system-wide, regardless of which
// netif is currently "default." Real-hardware evidence: cellular's PPP
// link set the resolver to the carrier's own DNS server when it
// connected; once cellular dropped and net_manager fell back to WiFi,
// nothing ever pushed WiFi's own (still perfectly valid, still cached
// on its own netif) DNS server back — every lookup kept failing
// (getaddrinfo() errors) for the rest of that boot even though WiFi
// itself was fully connected and working. Called every time an uplink
// is (re-)selected so the global resolver always matches whichever
// interface traffic is actually flowing through.
static void _apply_dns_for_uplink(esp_netif_t *netif, const char *fallback_dns_str) {
    if (!netif) return;

    esp_netif_dns_info_t dns = {0};
    char dns_str[16];
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK
        && dns.ip.u_addr.ip4.addr != 0 && dns.ip.u_addr.ip4.addr != 0xFFFFFFFF) {
        snprintf(dns_str, sizeof(dns_str), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    } else {
        // No DNS cached on this netif yet (e.g. WiFi connected but never
        // got one via DHCP for some reason) — a public resolver beats
        // silently leaving the previous uplink's now-unreachable one active.
        strlcpy(dns_str, fallback_dns_str, sizeof(dns_str));
    }

    esp_netif_dns_info_t set_dns = {0};
    set_dns.ip.type = ESP_IPADDR_TYPE_V4;
    set_dns.ip.u_addr.ip4.addr = ipaddr_addr(dns_str);
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &set_dns);

    ip_addr_t lwip_dns;
    ipaddr_aton(dns_str, &lwip_dns);
    dns_setserver(0, &lwip_dns);

    ESP_LOGI(TAG, "DNS resolver -> %s", dns_str);
}

net_uplink_t net_manager_get_active_uplink(void) { return s_active; }

esp_err_t net_manager_set_uplink(net_uplink_t uplink) {
    // An explicit, single-target switch cancels any standing "auto"
    // preference — matches phone-like behavior (manually picking WiFi
    // means "stop trying to jump me back to cellular in the background").
    // Only net_manager_set_uplink_auto() re-arms it.
    s_prefer_cellular_auto = false;

    if (uplink == NET_UPLINK_CELLULAR) {
        if (!cellular_ppp_is_connected()) {
            esp_err_t err = cellular_ppp_up(30000);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "set_uplink(CELLULAR): dial failed — staying on current uplink (%s)",
                         s_active == NET_UPLINK_WIFI ? "WiFi" : "none");
                return err;
            }
        }
        esp_netif_t *cell_netif = cellular_ppp_get_netif();
        if (!cell_netif) {
            ESP_LOGE(TAG, "set_uplink(CELLULAR): connected but no netif? — unexpected, staying on current uplink");
            return ESP_ERR_INVALID_STATE;
        }
        esp_netif_set_default_netif(cell_netif);
        _apply_dns_for_uplink(cell_netif, "8.8.8.8");
        if (hotspot_ap_is_running()) {
            hotspot_ap_enable_napt(cell_netif);
            hotspot_ap_update_dns(cell_netif);
        }
        s_active = NET_UPLINK_CELLULAR;
        ESP_LOGI(TAG, "Active uplink -> CELLULAR");
        return ESP_OK;
    }

    if (uplink == NET_UPLINK_WIFI) {
        esp_netif_t *sta_netif = _wifi_sta_netif();
        if (!sta_netif) {
            ESP_LOGE(TAG, "set_uplink(WIFI): no WIFI_STA_DEF netif — WiFi not initialized?");
            return ESP_ERR_INVALID_STATE;
        }
        esp_netif_set_default_netif(sta_netif);
        _apply_dns_for_uplink(sta_netif, "8.8.8.8");
        if (hotspot_ap_is_running()) {
            hotspot_ap_enable_napt(sta_netif);
            hotspot_ap_update_dns(sta_netif);
        }
        s_active = NET_UPLINK_WIFI;
        ESP_LOGI(TAG, "Active uplink -> WIFI");
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t net_manager_set_uplink_auto(void) {
    esp_err_t err = net_manager_set_uplink(NET_UPLINK_CELLULAR);
    // set_uplink() above just cleared this (it clears on every explicit
    // call) — re-set it here so the health-check timer keeps trying to
    // (re)acquire cellular in the background for as long as "auto" mode
    // is the standing preference, including after this specific attempt.
    s_prefer_cellular_auto = true;
    if (err == ESP_OK) return ESP_OK;
    ESP_LOGI(TAG, "auto: cellular unavailable — falling back to WiFi");
    return net_manager_set_uplink(NET_UPLINK_WIFI);
}

// ── Health check: doc 162 §4 ──────────────────────────────────────────
// The vendor's PPP layer can silently drop the link (see cellular_ppp.h's
// "PPP DIAL RECOVERY" note) without net_manager.c ever being told — once
// s_active latched onto CELLULAR it stayed there even after
// cellular_ppp_is_connected() went false, leaving esp_netif's default
// netif pointed at a dead interface. Confirmed on real hardware: "net
// test" failed with "Host is unreachable" while WiFi sat there connected
// and unused the whole time. This periodic check is the fix.
#define UPLINK_HEALTH_CHECK_PERIOD_S   15
// A dial (+ this turn's automatic modem-stack recovery cycle, see
// cellular_ppp.c) can itself take up to ~65s — retrying every health
// -check tick would hammer the modem with back-to-back recovery cycles,
// so auto-mode retries are throttled independently of the check period.
#define UPLINK_AUTO_RETRY_COOLDOWN_S   300

static esp_timer_handle_t s_health_timer = NULL;
static int64_t s_last_auto_retry_us = 0;

static bool _retry_cellular_job(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Auto mode: retrying cellular uplink in the background...");
    return net_manager_set_uplink_auto() == ESP_OK;
}

static void _health_check_cb(void *arg) {
    (void)arg;

    if (s_active == NET_UPLINK_CELLULAR && !cellular_ppp_is_connected()) {
        ESP_LOGW(TAG, "Health check: active uplink was CELLULAR but it silently dropped — "
                 "falling back to WiFi automatically (doc 162 §4)");
        net_manager_set_uplink(NET_UPLINK_WIFI);
        // Re-arm auto mode (the explicit set_uplink() call above just
        // cleared it) so the retry branch below keeps trying to win
        // cellular back later instead of leaving it stuck on WiFi.
        s_prefer_cellular_auto = true;
        return;
    }

    if (s_prefer_cellular_auto && s_active != NET_UPLINK_CELLULAR) {
        int64_t now = esp_timer_get_time();
        if (now - s_last_auto_retry_us >= (int64_t)UPLINK_AUTO_RETRY_COOLDOWN_S * 1000000LL) {
            s_last_auto_retry_us = now;
            if (!bg_worker_submit_fn(_retry_cellular_job, NULL, NULL, NULL)) {
                ESP_LOGW(TAG, "Health check: background worker busy — will retry cellular at the next cooldown window");
            }
        }
    }
}

void net_manager_get_status(net_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active_uplink      = s_active;
    out->wifi_connected     = (_wifi_sta_netif() != NULL) && esp_netif_is_netif_up(_wifi_sta_netif());
    out->cellular_connected = cellular_ppp_is_connected();
    out->hotspot_running    = hotspot_ap_is_running();
    // No direct "is NAPT on" getter from ESP-IDF — net_manager itself is
    // the only place NAPT gets enabled/disabled in this project, so its
    // own last-known state (mirrored via hotspot_ap_is_running() +
    // whichever uplink is active) is authoritative enough for status
    // reporting purposes; a dedicated flag isn't worth adding here.
    out->napt_active = out->hotspot_running && (out->wifi_connected || out->cellular_connected);
}

void net_manager_print_status(void) {
    net_status_t st;
    net_manager_get_status(&st);

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "UPLINK STATUS");
    ESP_LOGI(TAG, "──────────────────────────────────────");
    ESP_LOGI(TAG, "  Active uplink:     %s",
             st.active_uplink == NET_UPLINK_CELLULAR ? "CELLULAR" :
             st.active_uplink == NET_UPLINK_WIFI ? "WIFI" : "NONE");
    ESP_LOGI(TAG, "  WiFi connected:    %s", st.wifi_connected ? "yes" : "no");
    ESP_LOGI(TAG, "  Cellular connected:%s", st.cellular_connected ? " yes" : " no");
    ESP_LOGI(TAG, "  Hotspot running:   %s", st.hotspot_running ? "yes" : "no");
    ESP_LOGI(TAG, "  NAPT (sharing):    %s", st.napt_active ? "active" : "inactive");
    ESP_LOGI(TAG, "  Try: net uplink wifi|cellular|auto");
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

// ── Boot-time background dial-up (doc 155 §7.2's non-blocking design) ──
static bool _init_job(void *arg) {
    (void)arg;
    if (NET_UPLINK_PREFER_CELLULAR) {
        ESP_LOGI(TAG, "NET_UPLINK_PREFER_CELLULAR=1 — attempting cellular uplink in the background...");
        if (net_manager_set_uplink(NET_UPLINK_CELLULAR) == ESP_OK) return true;
        ESP_LOGW(TAG, "  Cellular unavailable at boot — staying on WiFi (already active). "
                 "Try 'cell up' or 'net uplink cellular' once signal/SIM are confirmed.");
    }
    return true;
}

esp_err_t net_manager_init(void) {
    // WiFi-STA is already connected (app_main.c's own blocking init ran
    // before this) — make it the immediate default so the system has
    // SOME working uplink from the first instant, zero wait.
    esp_netif_t *sta_netif = _wifi_sta_netif();
    if (sta_netif) {
        esp_netif_set_default_netif(sta_netif);
        s_active = NET_UPLINK_WIFI;
    }

    if (!bg_worker_submit_fn(_init_job, NULL, NULL, NULL)) {
        ESP_LOGW(TAG, "net_manager_init: background worker busy — cellular uplink preference will "
                 "apply on the next 'net uplink auto' or 'cell up'");
    }

    const esp_timer_create_args_t health_timer_args = {
        .callback = _health_check_cb,
        .name     = "net_health",
    };
    if (esp_timer_create(&health_timer_args, &s_health_timer) == ESP_OK) {
        esp_timer_start_periodic(s_health_timer, (uint64_t)UPLINK_HEALTH_CHECK_PERIOD_S * 1000000ULL);
    } else {
        ESP_LOGW(TAG, "net_manager_init: failed to create uplink health-check timer — "
                 "automatic WiFi fallback on a dropped cellular link will NOT happen");
    }
    return ESP_OK;
}
