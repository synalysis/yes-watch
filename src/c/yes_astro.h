#pragma once

#include <pebble.h>
#include "yes_types.h"

// Local date stamp (yyyymmdd) for a location using its tz offset minutes.
int ymd_for_loc_now(const GeoLoc *loc, int *out_y, int *out_m, int *out_d);

// Get "local" time by shifting UTC with tz offset (no DST rules beyond the offset provided).
bool get_location_local_tm(const GeoLoc *loc, struct tm *out_tm, int *out_minutes_since_midnight);

SunTimes calc_sunrise_sunset_local(int year, int month_1_12, int day_1_31,
                                   double lat_deg, double lon_deg,
                                   int32_t tz_offset_min);


