#pragma once

#include <pebble.h>
#include "yes_types.h"

void yes_draw_init(void);
void yes_draw_deinit(void);

// Draw either the watchface or the debug screen depending on `debug`.
void yes_draw_canvas(Layer *layer, GContext *ctx,
                     bool debug,
                     bool net_on,
                     bool have_loc,
                     bool have_sun,
                     bool have_moon,
                     bool have_phase,
                     int32_t moon_phase_e6,
                     const GeoLoc *loc,
                     const SunTimes *sun,
                     const MoonTimes *moon);


