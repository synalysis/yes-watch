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

// Display modes (config + rendering)
enum {
  YES_DISPLAY_MODE_NORMAL = 0,
  YES_DISPLAY_MODE_MINIMALISM = 1,
  YES_DISPLAY_MODE_STRICT_MINIMALISM = 2
};

static inline int yes_normalize_display_mode(int mode) {
  if (mode == YES_DISPLAY_MODE_MINIMALISM || mode == YES_DISPLAY_MODE_STRICT_MINIMALISM) {
    return mode;
  }
  return YES_DISPLAY_MODE_NORMAL;
}

// Golden / blue hour visualization
enum {
  YES_TWILIGHT_MODE_NONE = 0,
  YES_TWILIGHT_MODE_LINED = 1,
  YES_TWILIGHT_MODE_FULL = 2
};

static inline int yes_normalize_twilight_mode(int mode) {
  if (mode == YES_TWILIGHT_MODE_LINED || mode == YES_TWILIGHT_MODE_FULL) {
    return mode;
  }
  return YES_TWILIGHT_MODE_NONE;
}
