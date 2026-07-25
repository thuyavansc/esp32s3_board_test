#pragma once
// ================================================================
// api_client.h — Shared authenticated HTTPS + JSON helper
//
// WHAT THIS MODULE DOES:
//   Every TaxiMeter backend module (auth_client, setup_client,
//   duty_client, reference_data, trip_manager) needs to call the same
//   TaxiMeter REST API (base URL: https://mytaxis.softclient.com.au)
//   with the same TLS setup, the same "Authorization: Bearer <token>"
//   header, and the same error handling. This module is that ONE
//   shared implementation, so every call site gets identical,
//   already-tested behavior instead of eight slightly-different copies
//   of HTTP boilerplate.
//
//   Modeled on the HTTPS pattern already proven in this codebase
//   (backend/taximeter/rest_api_storage.c's own fetch path) — same TLS
//   cert bundle, same heap logging around esp_http_client_perform(),
//   same "log clearly, never silently swallow an error" style.
//
// TWO RESPONSE MODES (avoid heap-fragmentation risk on this board):
//   api_client_request()         → response body goes into a caller-
//                                   supplied buffer (heap-free). Use for
//                                   small responses: login, driver
//                                   profile, network/vehicle lookup,
//                                   on/off duty.
//   api_client_request_to_file() → response body streams straight to a
//                                   SPIFFS file, never fully buffered in
//                                   RAM. Use for reference-data fetches
//                                   (tariffs/fixed-rates/special-fares/
//                                   public-holidays), which can be tens
//                                   of KB — too large to safely buffer
//                                   on a board with limited free heap
//                                   during an HTTPS call.
//
// AUTH:
//   Pass use_auth=true to add "Authorization: Bearer <token>" using
//   whatever token session_store currently holds. Every endpoint except
//   Login/Network/Vehicle needs this.
//
// ERROR HANDLING:
//   Returns ESP_OK only when the HTTP transport itself succeeded (a
//   response was received) — this can still be a non-2xx status, which
//   the caller must check via *out_status. Returns ESP_FAIL for
//   transport-level failures (DNS, TLS, timeout, connection refused) —
//   these are logged with a specific reason at ESP_LOGE level so a
//   "why did this fail" question can be answered from the serial log
//   alone.
// ================================================================
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// GET or POST — this project doesn't need PUT/DELETE for the TaxiMeter
// business endpoints.
typedef enum {
    API_METHOD_GET,
    API_METHOD_POST,
} api_method_t;

// ── Buffered request (small responses — no heap streaming) ──────
//
// path        — URL path only, e.g. EP_LOGIN from config.h (host is
//               always TAXIMETER_API_HOST).
// json_body   — request body for POST (NULL for GET or a bodyless POST).
// use_auth    — true to add the Bearer token header.
// out_buf     — caller-owned buffer; response body is copied here,
//               null-terminated, truncated with a logged warning if the
//               response is larger than out_buf_size - 1.
// out_status  — HTTP status code (0 if the transport itself failed).
//
// Returns ESP_OK if a response was received at all (check *out_status
// for success/failure), ESP_FAIL on a transport-level failure.
esp_err_t api_client_request(api_method_t method, const char *path, const char *json_body,
                              bool use_auth, char *out_buf, size_t out_buf_size, int *out_status);

// ── Streamed-to-file request (large responses — reference data) ─
//
// Same as api_client_request(), but the response body is written
// directly to dest_path on SPIFFS as it arrives — never held whole in
// RAM. Caller must have already ensured dest_path's parent directory
// exists. On failure, any partially-written file is removed (don't
// leave 0-byte junk files behind).
esp_err_t api_client_request_to_file(api_method_t method, const char *path, const char *json_body,
                                      bool use_auth, const char *dest_path, int *out_status);

// Logs free heap + largest free contiguous block at the given checkpoint
// name — exposed publicly so every module can log heap state around its
// own HTTPS calls consistently, and so the "mem" serial command
// (backend/taximeter/diag.c) can report the same numbers on demand.
void api_client_log_heap(const char *when);
