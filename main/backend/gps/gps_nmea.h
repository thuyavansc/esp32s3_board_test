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

// doc 180 §7.1 / doc 182 10.7: a fix report is only trusted if its
// reported HDOP is below this. NMEA receivers commonly emit ~99 or 100
// as a "no computation possible" sentinel even while otherwise reporting
// a nonzero fix quality — this is deliberately generous (fare_calc.c's
// own trust hysteresis already distrusts anything above HDOP 5.0 for
// BILLING purposes, doc 150 §8; this threshold only governs whether
// gps_data_t.has_fix itself is honest for every OTHER consumer — GPS Info
// screens, "gps info"/"gps gnss info", the meter's live-fix indicator).
#define GPS_NMEA_HDOP_INVALID_THRESHOLD  50.0

// Parses a $GPGGA/$GNGGA sentence (position, altitude, satellites, fix
// quality) into *out.
//
// Returns true whenever the line was recognisably a GGA/GNGGA sentence
// AT ALL — including a genuine "no fix" report (quality==0). doc 180
// §7.1 / doc 182 10.7: this is a deliberate change from returning false
// on quality==0 — the old contract let callers skip updating *out
// entirely on a real "I have no fix" report, which is how has_fix ended
// up STICKY (frozen at whatever an earlier real fix set it to, with no
// expiry — a stale position shown as if it were current). Now:
//   - quality==0            -> out->has_fix=false, out->fix_quality=0, returns true
//   - quality>0 but HDOP >= GPS_NMEA_HDOP_INVALID_THRESHOLD -> same (a
//     fix "found" with unusable geometry is not a fix worth trusting)
//   - quality>0, lat/lon unparseable -> returns false (genuinely corrupt
//     line — nothing usable to report either way, not even "no fix")
//   - quality>0, HDOP ok, lat/lon parsed -> out fully populated, has_fix=true
// Callers should only copy fields out of *out when this returns true —
// on a true "no fix" result, that means copying has_fix=false onward,
// not skipping the copy (see gps_backend_neo6m.c / gps_backend_gnss.c).
bool gps_nmea_parse_gga(const char *sentence, gps_data_t *out);

// Parses a $GPRMC/$GNRMC sentence (speed, course) — updates *out
// in-place if the sentence reports a valid fix (status == 'A'): speed
// and course. doc 180 §7.1 point 2 / doc 182 10.7: if status == 'V'
// ("void" — RMC's OWN "this data is not valid" signal, independent of
// GGA's fix-quality field), out->has_fix is cleared too — a real-time
// cross-check on top of GGA's. Returns true if the sentence type
// matched at all (mirrors the original _parse_rmc contract — callers
// pass their persistent struct directly, not a temp).
bool gps_nmea_parse_rmc(const char *sentence, gps_data_t *out);
