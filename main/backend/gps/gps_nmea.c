/**
 * gps_nmea.c — Shared NMEA sentence parser
 *
 * See gps_nmea.h. Extracted verbatim from gps_backend_neo6m.c's
 * original private _parse_gga()/_parse_rmc()/_nmea_to_deg() — the exact
 * logic already tested against a real NEO-6M on real S3 hardware this
 * session, unchanged, just made shared so gps_backend_gnss.c can reuse
 * it for the A7670E's identical NMEA output.
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "gps_nmea.h"

// ═══════════════════════════════════════════════════════════════
//  NMEA → Decimal Degrees Conversion
//
//  NMEA format: ddmm.mmmm  (degrees concatenated with minutes)
//    Example: 3344.1234 = 33 degrees + 44.1234 minutes
//    Result:  33 + 44.1234/60 = 33.735390 decimal degrees
// ═══════════════════════════════════════════════════════════════
static double _nmea_to_deg(const char *coord, char dir) {
    if (!coord || strlen(coord) < 4) return 0;
    double raw = atof(coord) / 100.0;
    double deg = floor(raw);
    double min = (raw - deg) * 100.0;
    double val = deg + min / 60.0;
    if (dir == 'S' || dir == 'W') val = -val;
    return val;
}

bool gps_nmea_parse_gga(const char *sentence, gps_data_t *out) {
    if (strncmp(sentence, "$GPGGA", 6) != 0 &&
        strncmp(sentence, "$GNGGA", 6) != 0) return false;

    char tbuf[16] = {0}, lat[16] = {0}, ns = 'N', lon[16] = {0}, ew = 'E';
    int q = 0, sats = 0;
    float hdop = 0, alt = 0;

    int n = sscanf(sentence, "$GPGGA,%15[^,],%15[^,],%c,%15[^,],%c,%d,%d,%f,%f,M",
                   tbuf, lat, &ns, lon, &ew, &q, &sats, &hdop, &alt);
    if (n < 9) {
        // Try GNGGA (multi-constellation)
        n = sscanf(sentence, "$GNGGA,%15[^,],%15[^,],%c,%15[^,],%c,%d,%d,%f,%f,M",
                   tbuf, lat, &ns, lon, &ew, &q, &sats, &hdop, &alt);
    }

    // doc 180 §7.1 / doc 182 10.7: q==0 is the receiver's own explicit
    // "no fix right now" report — an empty lat/lon field earlier makes
    // sscanf stop matching before it reaches q, leaving q at its
    // initialized 0 (see the declaration above), so this correctly
    // catches both "q was parsed as literally 0" and "the sentence was
    // too empty to parse that far" — both mean the same thing: no fix.
    // Relayed as REAL current data (has_fix=false), not silently
    // dropped — see gps_nmea.h's full explanation of why this matters.
    if (q == 0) {
        out->has_fix     = false;
        out->fix_quality = 0;
        return true;
    }

    if (strlen(lat) < 4 || strlen(lon) < 4) return false;   // q>0 but position fields unusable — genuinely corrupt line, nothing usable to report

    // doc 180 §7.1 / doc 182 10.7: a fix was "found" but with unusable
    // geometry (HDOP near/at NMEA's ~99-100 "no computation" sentinel) —
    // don't report it as a trustworthy has_fix=true.
    if (hdop >= GPS_NMEA_HDOP_INVALID_THRESHOLD) {
        out->has_fix     = false;
        out->fix_quality = 0;
        return true;
    }

    out->lat         = _nmea_to_deg(lat, ns);
    out->lon         = _nmea_to_deg(lon, ew);
    out->alt         = alt;
    out->satellites  = sats;
    out->hdop        = hdop;
    out->fix_quality = q;
    out->has_fix     = true;
    return true;
}

bool gps_nmea_parse_rmc(const char *sentence, gps_data_t *out) {
    if (strncmp(sentence, "$GPRMC", 6) != 0 &&
        strncmp(sentence, "$GNRMC", 6) != 0) return false;

    char status = 'V';
    float speed_knots = 0, course = 0;

    // sentence+6 skips the already-verified 6-char "$GPRMC"/"$GNRMC"
    // prefix (see the strncmp check above) so the same format string
    // works for both variants — $GNRMC is very common on combo
    // GPS+GLONASS/BeiDou receivers, which both the NEO-6M and the
    // A7670E's GNSS engine commonly are.
    sscanf(sentence + 6, ",%*[^,],%c,%*[^,],%*c,%*[^,],%*c,%f,%f",
           &status, &speed_knots, &course);

    if (status == 'A') {
        out->speed  = speed_knots * 1.852;   // knots → km/h
        out->course = course;
    } else if (status == 'V') {
        // doc 180 §7.1 point 2 / doc 182 10.7: RMC's own "data not
        // valid" signal — a real-time cross-check independent of GGA's
        // fix-quality field. Doesn't touch speed/course (last known
        // values are harmless to leave — fare_calc only reads them
        // gated on has_fix anyway).
        out->has_fix = false;
    }
    return true;
}
