#include "yes_astro.h"

#include <stdlib.h>

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

// We avoid libm trig (sin/cos/atan/acos/asin) here because the Basalt emulator toolchain
// has shown app faults in libm argument reduction. Pebble provides safe fixed-point trig
// via sin_lookup/cos_lookup.
static inline int32_t deg_e6_to_trig(int32_t deg_e6) {
  // TRIG_MAX_ANGLE == 360 degrees
  return (int32_t)((int64_t)TRIG_MAX_ANGLE * (int64_t)deg_e6 / (360LL * 1000000LL));
}

static inline int32_t rad_e6_to_trig(int32_t rad_e6) {
  // 2*pi radians == TRIG_MAX_ANGLE; 2*pi*1e6 ~= 6283185
  return (int32_t)((int64_t)TRIG_MAX_ANGLE * (int64_t)rad_e6 / 6283185LL);
}

static inline int32_t trig_sin(int32_t a) { return sin_lookup(a); } // scaled by TRIG_MAX_RATIO
static inline int32_t trig_cos(int32_t a) { return cos_lookup(a); } // scaled by TRIG_MAX_RATIO

static int day_of_year(int year, int month_1_12, int day_1_31) {
  static const int days_before_month[] = { 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  int doy = days_before_month[month_1_12] + day_1_31;
  const bool leap = ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
  if (leap && month_1_12 > 2) {
    doy += 1;
  }
  return doy;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

int ymd_for_loc_now(const GeoLoc *loc, int *out_y, int *out_m, int *out_d) {
  if (!loc || !loc->valid) return 0;
  const time_t now_utc = time(NULL);
  const time_t shifted = now_utc + (time_t)loc->tz_offset_min * 60;
  struct tm *tm_p = gmtime(&shifted);
  if (!tm_p) return 0;
  const int y = tm_p->tm_year + 1900;
  const int m = tm_p->tm_mon + 1;
  const int d = tm_p->tm_mday;
  if (out_y) *out_y = y;
  if (out_m) *out_m = m;
  if (out_d) *out_d = d;
  return y * 10000 + m * 100 + d;
}

bool get_location_local_tm(const GeoLoc *loc, struct tm *out_tm, int *out_minutes_since_midnight) {
  if (!loc || !loc->valid) return false;

  time_t now_utc = time(NULL);
  time_t shifted = now_utc + (time_t)loc->tz_offset_min * 60;
  struct tm *tm_loc = gmtime(&shifted);
  if (!tm_loc) return false;

  *out_tm = *tm_loc;
  if (out_minutes_since_midnight) {
    *out_minutes_since_midnight = tm_loc->tm_hour * 60 + tm_loc->tm_min;
  }
  return true;
}

// Compute sin(altitude) of sun at a given minute using NOAA "equation of time" approximation
// (sin/cos series in gamma). All math is fixed-point using Pebble trig lookup.
// Returns sin(alt) scaled by TRIG_MAX_RATIO.
static int32_t sun_sin_alt_scaled(int N, int minute_of_day, int32_t lat_e6, int32_t lon_e6, int32_t tz_offset_min) {
  // gamma = 2*pi/365 * (N-1 + (minute-720)/1440)
  const int64_t a = (int64_t)TRIG_MAX_ANGLE * (int64_t)(N - 1) / 365LL;
  const int64_t b = (int64_t)TRIG_MAX_ANGLE * (int64_t)(minute_of_day - 720) / (365LL * 1440LL);
  const int32_t gamma = (int32_t)(a + b);

  const int32_t sin1 = trig_sin(gamma);
  const int32_t cos1 = trig_cos(gamma);
  const int32_t sin2 = trig_sin(gamma * 2);
  const int32_t cos2 = trig_cos(gamma * 2);
  const int32_t sin3 = trig_sin(gamma * 3);
  const int32_t cos3 = trig_cos(gamma * 3);

  // eqtime (seconds) = 13750800 * (0.000075 + 0.001868 cosγ - 0.032077 sinγ - 0.014615 cos2γ - 0.040849 sin2γ)
  // We evaluate the bracket term in 1e6 scale, using trig scaled by TRIG_MAX_RATIO.
  int64_t sum_e6 = 75;
  sum_e6 += (int64_t)1868  * cos1 / TRIG_MAX_RATIO;
  sum_e6 += (int64_t)-32077 * sin1 / TRIG_MAX_RATIO;
  sum_e6 += (int64_t)-14615 * cos2 / TRIG_MAX_RATIO;
  sum_e6 += (int64_t)-40849 * sin2 / TRIG_MAX_RATIO;
  const int32_t eqtime_sec = (int32_t)((int64_t)13750800 * sum_e6 / 1000000LL);

  // decl (rad, 1e6 scale):
  // 0.006918 - 0.399912 cosγ + 0.070257 sinγ - 0.006758 cos2γ + 0.000907 sin2γ - 0.002697 cos3γ + 0.00148 sin3γ
  int64_t decl_e6 = 6918;
  decl_e6 += (int64_t)-399912 * cos1 / TRIG_MAX_RATIO;
  decl_e6 += (int64_t) 70257 * sin1 / TRIG_MAX_RATIO;
  decl_e6 += (int64_t) -6758 * cos2 / TRIG_MAX_RATIO;
  decl_e6 += (int64_t)   907 * sin2 / TRIG_MAX_RATIO;
  decl_e6 += (int64_t) -2697 * cos3 / TRIG_MAX_RATIO;
  decl_e6 += (int64_t)  1480 * sin3 / TRIG_MAX_RATIO;
  const int32_t decl_trig = rad_e6_to_trig((int32_t)decl_e6);

  // True solar time (seconds):
  // tst_min = minutes + eqtime_min + 4*lon - tz_offset_min
  // => tst_sec = minutes*60 + eqtime_sec + 240*lon_deg - tz_offset_min*60
  const int64_t lon_term_sec = (int64_t)240 * (int64_t)lon_e6 / 1000000LL;
  int64_t tst_sec = (int64_t)minute_of_day * 60 + (int64_t)eqtime_sec + lon_term_sec - (int64_t)tz_offset_min * 60;
  tst_sec %= 86400;
  if (tst_sec < 0) tst_sec += 86400;

  // Hour angle (deg): ha_deg = tst_sec/240 - 180, then to TRIG:
  // ha_trig = TRIG_MAX_ANGLE * (tst_sec - 43200) / 86400
  const int32_t ha_trig = (int32_t)((int64_t)TRIG_MAX_ANGLE * (tst_sec - 43200) / 86400LL);

  const int32_t lat_trig = deg_e6_to_trig(lat_e6);
  const int32_t sin_lat = trig_sin(lat_trig);
  const int32_t cos_lat = trig_cos(lat_trig);
  const int32_t sin_dec = trig_sin(decl_trig);
  const int32_t cos_dec = trig_cos(decl_trig);
  const int32_t cos_ha  = trig_cos(ha_trig);

  // sin(alt) = sinφ sinδ + cosφ cosδ cosH
  int64_t term1 = (int64_t)sin_lat * (int64_t)sin_dec / TRIG_MAX_RATIO;
  int64_t term2 = (int64_t)cos_lat * (int64_t)cos_dec / TRIG_MAX_RATIO;
  term2 = term2 * (int64_t)cos_ha / TRIG_MAX_RATIO;
  int64_t s = term1 + term2;
  if (s > INT32_MAX) s = INT32_MAX;
  if (s < INT32_MIN) s = INT32_MIN;
  return (int32_t)s;
}

SunTimes calc_sunrise_sunset_local(int year, int month_1_12, int day_1_31,
                                   double lat_deg, double lon_deg,
                                   int32_t tz_offset_min) {
  SunTimes out = (SunTimes){ .valid = true, .always_day = false, .always_night = false, .sunrise_min = 0, .sunset_min = 0 };

  // Convert degrees (double) to e6 integers (avoid lround/libm).
  const int32_t lat_e6 = (int32_t)(lat_deg * 1000000.0 + (lat_deg >= 0 ? 0.5 : -0.5));
  const int32_t lon_e6 = (int32_t)(lon_deg * 1000000.0 + (lon_deg >= 0 ? 0.5 : -0.5));
  const int N = day_of_year(year, month_1_12, day_1_31);

  // Sunrise/sunset convention: sun center at -0.833 degrees altitude (refraction + radius)
  const int32_t h0_trig = deg_e6_to_trig(-833000);
  const int32_t sin_h0 = trig_sin(h0_trig);

  const int step = 10; // minutes
  int rise = -1, set = -1;
  int above_count = 0;

  int32_t prev_s = sun_sin_alt_scaled(N, 0, lat_e6, lon_e6, tz_offset_min);
  bool prev_above = (prev_s > sin_h0);
  if (prev_above) above_count++;

  for (int m = step; m <= 1440; m += step) {
    const int mm = (m == 1440) ? 1439 : m;
    const int32_t s = sun_sin_alt_scaled(N, mm, lat_e6, lon_e6, tz_offset_min);
    const bool above = (s > sin_h0);
    if (above) above_count++;

    if (above != prev_above) {
      int lo = m - step;
      int hi = m;
      for (int i = 0; i < 10; i++) {
        const int mid = (lo + hi) / 2;
        const int32_t sm = sun_sin_alt_scaled(N, mid, lat_e6, lon_e6, tz_offset_min);
        const bool ab = (sm > sin_h0);
        if (ab == prev_above) lo = mid; else hi = mid;
      }
      if (!prev_above && above) {
        if (rise < 0) rise = hi;
      } else if (prev_above && !above) {
        if (set < 0) set = hi;
      }
    }

    prev_above = above;
  }

  if (rise < 0 && set < 0) {
    const int samples = (1440 / step);
    if (above_count > samples / 2) out.always_day = true;
    else out.always_night = true;
    return out;
  }

  if (rise < 0) rise = 0;
  if (set < 0) set = 0;
  out.sunrise_min = clamp_i32(rise, 0, 1439);
  out.sunset_min  = clamp_i32(set, 0, 1439);
  return out;
}

