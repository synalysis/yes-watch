#pragma once

#include <pebble.h>
#include "yes_types.h"

void yes_draw_init(void);
void yes_draw_deinit(void);

// Draw the full watchface (including debug/loading screens). Intended for the main layer.
void yes_draw_face(Layer *layer, GContext *ctx,
                     bool debug,
                     bool net_on,
                     bool have_loc,
                     bool have_sun,
                     bool have_moon,
                     bool have_tide,
                     int32_t tide_last_unix,
                     int32_t tide_next_unix,
                     bool tide_next_is_high,
                     int16_t tide_level_x10,
                     bool tide_level_is_ft,
                     bool alt_valid,
                     int32_t alt_m,
                     bool alt_is_ft,
                     bool battery_alert,
                     uint8_t battery_percent,
                     bool have_weather,
                     int16_t weather_temp_c10,
                     uint8_t weather_code,
                     bool weather_is_day,
                     bool weather_is_f,
                     int16_t weather_wind_spd_x10,
                     int16_t weather_wind_dir_deg,
                     int16_t weather_precip_x10,
                     int16_t weather_uv_x10,
                     int16_t weather_pressure_hpa_x10,
                     bool have_phase,
                     int32_t moon_phase_e6,
                     const GeoLoc *loc,
                     const SunTimes *sun,
                     const MoonTimes *moon);

// Draw only corner complications (no background clearing). Intended for a lightweight overlay layer.
void yes_draw_corners(Layer *layer, GContext *ctx,
                      bool debug,
                      bool have_loc,
                      bool have_sun,
                      bool have_moon,
                      bool have_tide,
                      int32_t tide_last_unix,
                      int32_t tide_next_unix,
                      bool tide_next_is_high,
                      int16_t tide_level_x10,
                      bool tide_level_is_ft,
                      bool alt_valid,
                      int32_t alt_m,
                      bool alt_is_ft,
                      bool battery_alert,
                      uint8_t battery_percent,
                      bool have_weather,
                      int16_t weather_temp_c10,
                      uint8_t weather_code,
                      bool weather_is_day,
                      bool weather_is_f,
                      int16_t weather_wind_spd_x10,
                      int16_t weather_wind_dir_deg,
                      int16_t weather_precip_x10,
                      int16_t weather_uv_x10,
                      int16_t weather_pressure_hpa_x10,
                      bool have_phase,
                      int32_t moon_phase_e6,
                      const GeoLoc *loc,
                      const SunTimes *sun,
                      const MoonTimes *moon);


