#include "yes_astro.h"

#include <math.h>
#include <stdlib.h>

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#ifndef DEG2RAD
#define DEG2RAD 0.017453292519943295
#endif

static double norm_deg(double d) {
  d = fmod(d, 360.0);
  if (d < 0.0) d += 360.0;
  return d;
}

static double norm_hours(double h) {
  h = fmod(h, 24.0);
  if (h < 0.0) h += 24.0;
  return h;
}

static int day_of_year(int year, int month_1_12, int day_1_31) {
  static const int days_before_month[] = { 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  int doy = days_before_month[month_1_12] + day_1_31;
  const bool leap = ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
  if (leap && month_1_12 > 2) {
    doy += 1;
  }
  return doy;
}

typedef enum {
  SOLAR_EVENT_OK = 0,
  SOLAR_EVENT_POLAR_DAY,
  SOLAR_EVENT_POLAR_NIGHT,
  SOLAR_EVENT_ERR
} SolarEventResult;

// Same NOAA approximation as pkjs calcSolarEventLocalHours (matches MET Norway closely).
static SolarEventResult calc_solar_event_hours(int N, double lat_deg, double lon_deg, double tz_hours,
                                               bool is_sunrise, double *out_hours) {
  if (!out_hours) return SOLAR_EVENT_ERR;

  const double lng_hour = lon_deg / 15.0;
  const double zenith = 90.833;
  const double t = (double)N + ((is_sunrise ? 6.0 : 18.0) - lng_hour) / 24.0;
  const double M = (0.9856 * t) - 3.289;
  double L = M + (1.916 * sin(M * DEG2RAD)) + (0.020 * sin(2.0 * M * DEG2RAD)) + 282.634;
  L = norm_deg(L);

  double RA = atan(0.91764 * tan(L * DEG2RAD)) / DEG2RAD;
  RA = norm_deg(RA);
  const double Lq = floor(L / 90.0) * 90.0;
  const double RAq = floor(RA / 90.0) * 90.0;
  RA = (RA + (Lq - RAq)) / 15.0;

  const double sin_dec = 0.39782 * sin(L * DEG2RAD);
  const double cos_dec = cos(asin(sin_dec));
  const double cos_h = (cos(zenith * DEG2RAD) - (sin_dec * sin(lat_deg * DEG2RAD))) /
                       (cos_dec * cos(lat_deg * DEG2RAD));

  if (cos_h > 1.0) return SOLAR_EVENT_POLAR_NIGHT;
  if (cos_h < -1.0) return SOLAR_EVENT_POLAR_DAY;

  double H = is_sunrise ? (360.0 - (acos(cos_h) / DEG2RAD)) : (acos(cos_h) / DEG2RAD);
  H /= 15.0;
  const double T = H + RA - (0.06571 * t) - 6.622;
  const double UT = norm_hours(T - lng_hour);
  *out_hours = norm_hours(UT + tz_hours);
  return SOLAR_EVENT_OK;
}

static int minutes_from_hours(double hours) {
  int mins = (int)lround(hours * 60.0);
  mins %= 1440;
  if (mins < 0) mins += 1440;
  return mins;
}

int32_t yes_tz_offset_min(const GeoLoc *loc) {
  if (clock_is_timezone_set()) {
    const time_t now = time(NULL);
    struct tm *tm_p = localtime(&now);
    if (tm_p) return (int32_t)(tm_p->tm_gmtoff / 60);
  }
  if (loc && loc->valid) return loc->tz_offset_min;
  return 0;
}

bool yes_local_tm_now(const GeoLoc *loc, struct tm *out_tm, int *out_minutes_since_midnight) {
  if (!out_tm) return false;

  const time_t now = time(NULL);
  struct tm *tm_p = NULL;
  if (clock_is_timezone_set()) {
    tm_p = localtime(&now);
  } else if (loc && loc->valid) {
    const time_t shifted = now + (time_t)loc->tz_offset_min * 60;
    tm_p = gmtime(&shifted);
  } else {
    tm_p = localtime(&now);
  }
  if (!tm_p) return false;

  *out_tm = *tm_p;
  if (out_minutes_since_midnight) {
    *out_minutes_since_midnight = tm_p->tm_hour * 60 + tm_p->tm_min;
  }
  return true;
}

int ymd_for_loc_now(const GeoLoc *loc, int *out_y, int *out_m, int *out_d) {
  if (!loc || !loc->valid) return 0;
  struct tm tm_loc;
  if (!yes_local_tm_now(loc, &tm_loc, NULL)) return 0;
  const int y = tm_loc.tm_year + 1900;
  const int m = tm_loc.tm_mon + 1;
  const int d = tm_loc.tm_mday;
  if (out_y) *out_y = y;
  if (out_m) *out_m = m;
  if (out_d) *out_d = d;
  return y * 10000 + m * 100 + d;
}

bool get_location_local_tm(const GeoLoc *loc, struct tm *out_tm, int *out_minutes_since_midnight) {
  if (!loc || !loc->valid) return false;
  return yes_local_tm_now(loc, out_tm, out_minutes_since_midnight);
}

bool yes_sun_times_sane(const SunTimes *sun) {
  if (!sun || !sun->valid) return false;
  if (sun->always_day || sun->always_night) return true;
  if (sun->sunrise_min < 0 || sun->sunset_min < 0) return false;
  if (sun->sunrise_min >= sun->sunset_min) return false;
  return true;
}

SunTimes calc_sunrise_sunset_local(int year, int month_1_12, int day_1_31,
                                   double lat_deg, double lon_deg,
                                   int32_t tz_offset_min) {
  SunTimes out = (SunTimes){ .valid = true, .always_day = false, .always_night = false, .sunrise_min = 0, .sunset_min = 0 };

  const double tz_hours = (double)tz_offset_min / 60.0;
  const int N = day_of_year(year, month_1_12, day_1_31);
  double rise_h = 0.0, set_h = 0.0;

  const SolarEventResult rise_r = calc_solar_event_hours(N, lat_deg, lon_deg, tz_hours, true, &rise_h);
  const SolarEventResult set_r = calc_solar_event_hours(N, lat_deg, lon_deg, tz_hours, false, &set_h);

  if (rise_r == SOLAR_EVENT_POLAR_DAY || set_r == SOLAR_EVENT_POLAR_DAY) {
    out.always_day = true;
    return out;
  }
  if (rise_r == SOLAR_EVENT_POLAR_NIGHT || set_r == SOLAR_EVENT_POLAR_NIGHT) {
    out.always_night = true;
    return out;
  }
  if (rise_r != SOLAR_EVENT_OK || set_r != SOLAR_EVENT_OK) {
    out.valid = false;
    return out;
  }

  out.sunrise_min = minutes_from_hours(rise_h);
  out.sunset_min = minutes_from_hours(set_h);
  if (!yes_sun_times_sane(&out)) {
    out.valid = false;
    out.sunrise_min = 0;
    out.sunset_min = 0;
  }
  return out;
}
