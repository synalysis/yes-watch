#pragma once

#include <pebble.h>

typedef struct {
  int32_t lat_e6;
  int32_t lon_e6;
  int32_t tz_offset_min;
  bool valid;
} GeoLoc;

typedef struct {
  bool valid;
  bool always_day;
  bool always_night;
  int sunrise_min; // local minutes since midnight
  int sunset_min;  // local minutes since midnight
} SunTimes;

typedef struct {
  bool valid;
  bool always_up;
  bool always_down;
  int moonrise_min; // local minutes since midnight
  int moonset_min;  // local minutes since midnight
} MoonTimes;


