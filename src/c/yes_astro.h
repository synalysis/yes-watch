#pragma once

#include <pebble.h>
#include "yes_types.h"

// Minutes east of UTC: watch system timezone when set, else loc fallback.
int32_t yes_tz_offset_min(const GeoLoc *loc);

// Current local calendar time (system timezone when set).
bool yes_local_tm_now(const GeoLoc *loc, struct tm *out_tm, int *out_minutes_since_midnight);

// Local date stamp (yyyymmdd) for a location.
int ymd_for_loc_now(const GeoLoc *loc, int *out_y, int *out_m, int *out_d);

bool get_location_local_tm(const GeoLoc *loc, struct tm *out_tm, int *out_minutes_since_midnight);

SunTimes calc_sunrise_sunset_local(int year, int month_1_12, int day_1_31,
                                   double lat_deg, double lon_deg,
                                   int32_t tz_offset_min);


