#pragma once
// ================================================================
// gps_nmea.h — Shared NMEA sentence parser
//
// Both GPS backends emit standard NMEA sentences — the NEO-6M natively,
// and the A7670E's GNSS engine once AT+CGNSSTST=1 + AT+CGNSSPORTSWITCH
// are sent (docs/TestFunctionalities/esp32s3_board/
// 139_2026-07-25_gnss_agps_implementation_research_and_plan.md §2).
// This is the ONE parser both use — extracted verbatim from what was
// originally gps_backend_neo6m.c's private implementation (already
// tested on real S3 hardware this session), so a parser fix only needs
// to happen in one place and both backends benefit identically.
//
// NMEA SENTENCE FORMAT:
//   $GPGGA,time,lat,N,lon,E,qual,sats,hdop,alt,M,geoid,M,age,ref*cs
//     Field 6: fix quality (0=invalid, 1=GPS, 2=DGPS)
//     Field 7: satellites used, Field 8: HDOP, Field 9: altitude (m)
//   $GPRMC,time,status,lat,N,lon,E,speed,course,date,magvar,varDir,mode*cs
//     Field 2: A/V (valid/void), Field 7: speed (knots), Field 8: course (deg)
//
// $GN-prefixed variants (multi-constellation GPS+GLONASS/BeiDou combo
// receivers, which both the NEO-6M and the A7670E's GNSS commonly are)
// are handled identically to $GP-prefixed ones.
// ================================================================
#include <stdbool.h>
#include "gps_client.h"   // gps_data_t

// Parses a $GPGGA/$GNGGA sentence (position, altitude, satellites, fix
// quality) into *out. Returns true only if a usable fix was found
// (quality > 0 and lat/lon fields present) — callers should only copy
// fields out of *out when this returns true, matching the "parse into a
// temp struct, then copy selected fields onto the persistent struct"
// pattern both backends use (see gps_backend_neo6m.c / gps_backend_gnss.c).
bool gps_nmea_parse_gga(const char *sentence, gps_data_t *out);

// Parses a $GPRMC/$GNRMC sentence (speed, course) — updates *out
// in-place (speed/course fields only) if the sentence reports a valid
// fix (status == 'A'). Returns true if the sentence type matched at all
// (mirrors the original _parse_rmc contract — callers pass their
// persistent struct directly, not a temp).
bool gps_nmea_parse_rmc(const char *sentence, gps_data_t *out);
