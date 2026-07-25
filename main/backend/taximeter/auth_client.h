#pragma once
// ================================================================
// auth_client.h — Login, logout, token expiry
//
// WHAT THIS MODULE DOES:
//   POST taxis-api/api/Authentications/Login  → stores access/refresh
//     tokens + driver/vehicle identity in session_store
//   GET  taxis-api/api/Driver                 → fills in driver name
//     (a second call, right after login)
//   POST taxis-api/api/Authentications/Logout
//
//   Access-token expiry is NOT sent by the server as a separate field —
//   it's decoded client-side from the JWT's own "exp" claim, using
//   mbedtls_base64_decode + cJSON, both already linked into this
//   project. Refresh-token expiry IS sent as an ISO 8601 date string
//   and is parsed to epoch seconds here.
//
// SERIAL COMMANDS:
//   auth login [username] [password]  → login (defaults to
//                                         config.h's AUTH_TEST_USERNAME/
//                                         AUTH_TEST_PASSWORD if omitted)
//   auth logout                        → logout
//   auth info                          → show token validity + expiry
//   auth help                          → command list
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

// Logs in, stores tokens + driver/vehicle identity, then fetches the
// driver profile for the display name. Requires setup_client to have
// already resolved a network + vehicle (session_store's network_id/
// vehicle_no).
esp_err_t auth_client_login(const char *username, const char *password);

esp_err_t auth_client_logout(void);

// True if the stored access token is present and not expired.
bool auth_client_is_logged_in(void);

// Serial command handler ("auth ...")
bool auth_client_process_command(const char *line);
