#include "yes_draw.h"

#include <math.h>
#include <string.h>

#include "yes_astro.h"

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

// Reusable path for the 24h hand (tapered arrow)
static GPath *s_hand_path;
static GPoint s_hand_points[5];

// Scale a "baseline Basalt" pixel value (for 144x168 => face_r ~72) to current face radius.
static int16_t scale_px(int16_t base_px, int16_t face_r) {
  const int16_t BASE_R = 72;
  if (face_r <= 0) return base_px;
  int32_t v = (int32_t)base_px * (int32_t)face_r;
  v = (v + (BASE_R / 2)) / BASE_R; // round
  if (v < 1) v = 1;
  if (v > 32767) v = 32767;
  return (int16_t)v;
}

void yes_draw_init(void) {
  if (!s_hand_path) {
    const GPathInfo hand_info = {
      .num_points = (uint32_t)(sizeof(s_hand_points) / sizeof(s_hand_points[0])),
      .points = s_hand_points,
    };
    s_hand_path = gpath_create(&hand_info);
  }
}

void yes_draw_deinit(void) {
  if (s_hand_path) {
    gpath_destroy(s_hand_path);
    s_hand_path = NULL;
  }
}

static int32_t angle_from_local_minutes_24h(int minutes_since_midnight) {
  const int m = (minutes_since_midnight % 1440 + 1440) % 1440;
  const int delta = m - 720; // minutes from noon
  int32_t angle = (int32_t)lround((double)delta * (double)TRIG_MAX_ANGLE / 1440.0);
  angle %= TRIG_MAX_ANGLE;
  if (angle < 0) angle += TRIG_MAX_ANGLE;
  return angle;
}

static bool angle_in_sweep(int32_t a, int32_t start, int32_t end) {
  a %= TRIG_MAX_ANGLE; start %= TRIG_MAX_ANGLE; end %= TRIG_MAX_ANGLE;
  if (a < 0) a += TRIG_MAX_ANGLE;
  if (start < 0) start += TRIG_MAX_ANGLE;
  if (end < 0) end += TRIG_MAX_ANGLE;

  if (start == end) return true;
  if (start < end) return (a >= start && a <= end);
  return (a >= start || a <= end);
}

static void format_deg2_from_e6(char *out, size_t out_sz, int32_t e6) {
  int32_t v = e6;
  const char sign = (v < 0) ? '-' : '+';
  if (v < 0) v = -v;
  const int32_t whole = v / 1000000;
  const int32_t frac2 = (v % 1000000) / 10000; // 2 decimals
  snprintf(out, out_sz, "%c%ld.%02ld", sign, (long)whole, (long)frac2);
}

static void fill_radial_wedge(GContext *ctx, GRect bounds, uint16_t inset,
                              int32_t start_angle, int32_t end_angle, GColor color) {
  const GRect disk_rect = GRect(bounds.origin.x + inset,
                                bounds.origin.y + inset,
                                bounds.size.w - (inset * 2),
                                bounds.size.h - (inset * 2));
  const int16_t r = (int16_t)(MIN(disk_rect.size.w, disk_rect.size.h) / 2);
  if (r <= 0) return;
  if (start_angle == end_angle) return;

  int32_t s = start_angle % TRIG_MAX_ANGLE;
  int32_t e = end_angle % TRIG_MAX_ANGLE;
  if (s < 0) s += TRIG_MAX_ANGLE;
  if (e < 0) e += TRIG_MAX_ANGLE;

  graphics_context_set_fill_color(ctx, color);
  const uint16_t thickness = (uint16_t)r; // fill from outside all the way to center

  if (s < e) {
    graphics_fill_radial(ctx, disk_rect, GOvalScaleModeFitCircle, thickness, s, e);
  } else {
    // Wrap across 0°
    graphics_fill_radial(ctx, disk_rect, GOvalScaleModeFitCircle, thickness, s, TRIG_MAX_ANGLE);
    graphics_fill_radial(ctx, disk_rect, GOvalScaleModeFitCircle, thickness, 0, e);
  }
}

// Draw the moon ring "background" as a filled disk. The solar day disk (drawn later) will cut out
// the center, leaving a clean ring area with no seams.
static void draw_ring_base_disk(GContext *ctx, GRect bounds, uint16_t inset, uint16_t thickness, GColor color) {
  const GRect ring_rect = GRect(bounds.origin.x + inset,
                                bounds.origin.y + inset,
                                bounds.size.w - (inset * 2),
                                bounds.size.h - (inset * 2));
  const GPoint c = grect_center_point(&ring_rect);
  const int16_t r = (int16_t)(MIN(ring_rect.size.w, ring_rect.size.h) / 2);
  if (r <= 0 || thickness == 0) return;
  const int16_t r_out = (int16_t)(r + (int16_t)(thickness / 2));
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_circle(ctx, c, r_out);
}

// Prefer Pebble's native arc primitive when available (fewer draw calls, smoother).
static void draw_ring_arc(GContext *ctx, GRect bounds, uint16_t inset, uint16_t thickness,
                          int32_t start_angle, int32_t end_angle, GColor color) {
  const GRect ring_rect = GRect(bounds.origin.x + inset,
                                bounds.origin.y + inset,
                                bounds.size.w - (inset * 2),
                                bounds.size.h - (inset * 2));
  if (thickness == 0) return;
  if (start_angle == end_angle) return;

  int32_t s = start_angle % TRIG_MAX_ANGLE;
  int32_t e = end_angle % TRIG_MAX_ANGLE;
  if (s < 0) s += TRIG_MAX_ANGLE;
  if (e < 0) e += TRIG_MAX_ANGLE;

  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, thickness);

  // Full circle
  if (s == e) {
    graphics_draw_arc(ctx, ring_rect, GOvalScaleModeFitCircle, 0, TRIG_MAX_ANGLE);
    return;
  }

  // Non-wrapping vs wrapping
  if (s < e) {
    graphics_draw_arc(ctx, ring_rect, GOvalScaleModeFitCircle, s, e);
  } else {
    graphics_draw_arc(ctx, ring_rect, GOvalScaleModeFitCircle, s, TRIG_MAX_ANGLE);
    graphics_draw_arc(ctx, ring_rect, GOvalScaleModeFitCircle, 0, e);
  }
}

static void draw_outer_scale(GContext *ctx, GRect bounds, uint16_t moon_inset, uint16_t moon_ring_thickness) {
  const GPoint c = grect_center_point(&bounds);
  const int min_dim = (int)MIN(bounds.size.w, bounds.size.h);
  const int16_t face_r = (int16_t)(min_dim / 2);

  const int16_t ring_outer_r = (int16_t)(face_r - (int16_t)moon_inset);
  const int16_t r_outer = (int16_t)(face_r - 1);

  const int16_t band_inner = (int16_t)(ring_outer_r + (int16_t)moon_ring_thickness + 1);
  const int16_t band_outer = r_outer;

  const int16_t label_h = scale_px(16, face_r);
  const int16_t r_label = (int16_t)(band_outer - scale_px(8, face_r));

  const int16_t long_len = scale_px(10, face_r);
  const int16_t short_len = scale_px(5, face_r);
  const int16_t r_short_start = band_inner;
  const int16_t r_short_end = (int16_t)MIN(band_outer, r_short_start + short_len);
  const int16_t r_long_start = band_inner;
  const int16_t r_long_end = (int16_t)MIN(band_outer, r_long_start + long_len);

  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_text_color(ctx, GColorWhite);
  const GFont font = (min_dim >= 200)
    ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
    : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  for (int i = 1; i < 48; i++) {
    const int m = i * 30;
    if ((m % 60) == 0) continue;
    if ((m % 120) == 0) continue;

    const int32_t a = angle_from_local_minutes_24h(m);
    const int16_t x1 = (int16_t)(c.x + (int32_t)sin_lookup(a) * r_short_start / TRIG_MAX_RATIO);
    const int16_t y1 = (int16_t)(c.y - (int32_t)cos_lookup(a) * r_short_start / TRIG_MAX_RATIO);
    const int16_t x2 = (int16_t)(c.x + (int32_t)sin_lookup(a) * r_short_end / TRIG_MAX_RATIO);
    const int16_t y2 = (int16_t)(c.y - (int32_t)cos_lookup(a) * r_short_end / TRIG_MAX_RATIO);
    graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
  }

  for (int h = 0; h < 24; h++) {
    const int m = h * 60;
    const int32_t a = angle_from_local_minutes_24h(m);

    if ((h % 2) == 0) {
      int label = (h == 0) ? 24 : h;
      char buf[3];
      snprintf(buf, sizeof(buf), "%d", label);

      const int16_t x = (int16_t)(c.x + (int32_t)sin_lookup(a) * r_label / TRIG_MAX_RATIO);
      const int16_t y = (int16_t)(c.y - (int32_t)cos_lookup(a) * r_label / TRIG_MAX_RATIO);

      const GSize sz = graphics_text_layout_get_content_size(
        buf, font, GRect(0, 0, 60, label_h),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter
      );
      const int16_t w = (int16_t)(sz.w + 4);

      int16_t rx = (int16_t)(x - w / 2);
      int16_t ry = (int16_t)(y - label_h / 2);
      if (rx < 0) rx = 0;
      if (ry < 0) ry = 0;
      if (rx + w > bounds.size.w) rx = (int16_t)(bounds.size.w - w);
      if (ry + label_h > bounds.size.h) ry = (int16_t)(bounds.size.h - label_h);
      const GRect r = GRect(rx, ry, w, label_h);
      graphics_draw_text(ctx, buf, font, r, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    } else {
      const int16_t x1 = (int16_t)(c.x + (int32_t)sin_lookup(a) * r_long_start / TRIG_MAX_RATIO);
      const int16_t y1 = (int16_t)(c.y - (int32_t)cos_lookup(a) * r_long_start / TRIG_MAX_RATIO);
      const int16_t x2 = (int16_t)(c.x + (int32_t)sin_lookup(a) * r_long_end / TRIG_MAX_RATIO);
      const int16_t y2 = (int16_t)(c.y - (int32_t)cos_lookup(a) * r_long_end / TRIG_MAX_RATIO);
      graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
    }
  }
}

static double moon_phase_0_1(time_t now_utc) {
  const time_t ref = 947182440;
  const int64_t synodic_sec = 2551443;
  int64_t delta = (int64_t)now_utc - (int64_t)ref;
  int64_t m = delta % synodic_sec;
  if (m < 0) m += synodic_sec;
  return (double)m / (double)synodic_sec;
}

static int16_t isqrt16(int16_t n) {
  if (n <= 0) return 0;
  int16_t x = 0;
  while ((int32_t)(x + 1) * (int32_t)(x + 1) <= (int32_t)n) x++;
  return x;
}

static void draw_moon(GContext *ctx, GPoint center, int radius, double phase) {
  // Snap near endpoints so "full" and "new" look clean and don't show a stray terminator.
  // Also, draw the terminator using scanlines so the shadow never paints outside the moon disk.
  const double eps = 0.015; // ~0.44 days
  if (phase < 0) phase = 0;
  if (phase > 1) phase = 1;

  if (radius <= 0) return;

  if (phase < eps || phase > 1.0 - eps) {
    // New moon: dark disk with bright outline
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, center, radius);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_circle(ctx, center, radius);
    return;
  }

  if (fabs(phase - 0.5) < eps) {
    // Full moon: bright disk
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, center, radius);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_circle(ctx, center, radius);
    return;
  }

  // White base disk
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, center, radius);

  // Shadow mask: same concept as the old "offset circle", but clipped to the moon disk via scanlines.
  const double d = 2.0 * (double)radius * (1.0 - 2.0 * fabs(phase - 0.5)); // 0..2r
  int16_t offset = (int16_t)lround(d);
  if (offset < 0) offset = 0;
  if (offset > 2 * radius) offset = (int16_t)(2 * radius);
  // When offset is ~2r the shadow-mask circle is tangent to the moon disk, which can produce
  // a single dark pixel. Treat this as "full enough" and draw no shadow.
  if (offset >= (int16_t)(2 * radius - 1)) {
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_circle(ctx, center, radius);
    return;
  }
  const bool waxing = (phase < 0.5);
  const int16_t dx = waxing ? (int16_t)(-offset) : offset;

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 1);

  const int16_t r = (int16_t)radius;
  for (int16_t yy = -r; yy <= r; yy++) {
    const int16_t x_disk = isqrt16((int16_t)(r * r - yy * yy));
    int16_t x1 = (int16_t)(dx - x_disk);
    int16_t x2 = (int16_t)(dx + x_disk);
    // Clip to the moon disk extents for this scanline.
    if (x1 < -x_disk) x1 = (int16_t)(-x_disk);
    if (x2 >  x_disk) x2 = x_disk;
    if (x1 <= x2) {
      const GPoint p0 = GPoint((int16_t)(center.x + x1), (int16_t)(center.y + yy));
      const GPoint p1 = GPoint((int16_t)(center.x + x2), (int16_t)(center.y + yy));
      graphics_draw_line(ctx, p0, p1);
    }
  }

  // Outline
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_circle(ctx, center, radius);
}

void yes_draw_canvas(Layer *layer, GContext *ctx,
                     bool debug,
                     bool net_on,
                     bool have_loc,
                     bool have_sun,
                     bool have_moon,
                     bool have_phase,
                     int32_t moon_phase_e6,
                     const GeoLoc *loc,
                     const SunTimes *sun_times,
                     const MoonTimes *moon_times) {
  const GRect bounds = layer_get_bounds(layer);
  const GPoint c = grect_center_point(&bounds);
  const int min_dim = (int)MIN(bounds.size.w, bounds.size.h);
  const int16_t face_r = (int16_t)(min_dim / 2);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (debug) {
    char buf0[32], buf1[32], buf2[32], buf3[32], buf4[32];

    char time_buf[6];
    if (loc && loc->valid) {
      time_t now_utc = time(NULL);
      time_t shifted = now_utc + (time_t)loc->tz_offset_min * 60;
      struct tm *tm_p = gmtime(&shifted);
      if (tm_p) strftime(time_buf, sizeof(time_buf), clock_is_24h_style() ? "%H:%M" : "%I:%M", tm_p);
      else strcpy(time_buf, "--:--");
    } else {
      strcpy(time_buf, "--:--");
    }

    if (loc && loc->valid) {
      const int off = (int)loc->tz_offset_min;
      const int sign = (off < 0) ? -1 : 1;
      const int aoff = off * sign;
      const int hh = aoff / 60;
      const int mm = aoff % 60;
      snprintf(buf0, sizeof(buf0), "DEBUG  %s", time_buf);
      snprintf(buf1, sizeof(buf1), "TZ UTC%c%02d:%02d  NET:%s",
               (sign < 0) ? '-' : '+', hh, mm, net_on ? "ON" : "OFF");
      char lat_s[16], lon_s[16];
      format_deg2_from_e6(lat_s, sizeof(lat_s), loc->lat_e6);
      format_deg2_from_e6(lon_s, sizeof(lon_s), loc->lon_e6);
      snprintf(buf4, sizeof(buf4), "LAT %s  LON %s", lat_s, lon_s);
    } else {
      snprintf(buf0, sizeof(buf0), "DEBUG  %s", time_buf);
      snprintf(buf1, sizeof(buf1), "TZ --  NET:%s", net_on ? "ON" : "OFF");
      snprintf(buf4, sizeof(buf4), "LAT/LON --");
    }

    if (sun_times && sun_times->valid && !sun_times->always_day && !sun_times->always_night) {
      snprintf(buf2, sizeof(buf2), "SR %02d:%02d  SS %02d:%02d",
               sun_times->sunrise_min / 60, sun_times->sunrise_min % 60,
               sun_times->sunset_min / 60,  sun_times->sunset_min % 60);
    } else if (sun_times && sun_times->valid && sun_times->always_day) {
      snprintf(buf2, sizeof(buf2), "SUN: ALWAYS DAY");
    } else if (sun_times && sun_times->valid && sun_times->always_night) {
      snprintf(buf2, sizeof(buf2), "SUN: ALWAYS NIGHT");
    } else {
      snprintf(buf2, sizeof(buf2), "SUN: --");
    }

    if (moon_times && moon_times->valid && !moon_times->always_up && !moon_times->always_down) {
      snprintf(buf3, sizeof(buf3), "MR %02d:%02d  MS %02d:%02d",
               moon_times->moonrise_min / 60, moon_times->moonrise_min % 60,
               moon_times->moonset_min / 60,  moon_times->moonset_min % 60);
    } else if (moon_times && moon_times->valid && moon_times->always_up) {
      snprintf(buf3, sizeof(buf3), "MOON: ALWAYS UP");
    } else if (moon_times && moon_times->valid && moon_times->always_down) {
      snprintf(buf3, sizeof(buf3), "MOON: ALWAYS DOWN");
    } else {
      snprintf(buf3, sizeof(buf3), "MOON: --");
    }

    graphics_context_set_text_color(ctx, GColorWhite);
    const int16_t line_h = (min_dim >= 200) ? scale_px(24, face_r) : scale_px(22, face_r);
    const int16_t top_y = scale_px(8, face_r);
    const GFont f_dbg0 = (min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD)
                                          : fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    const GFont f_dbg = (min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_24)
                                         : fonts_get_system_font(FONT_KEY_GOTHIC_18);

    graphics_draw_text(ctx, buf0, f_dbg0,
                       GRect(0, top_y, bounds.size.w, line_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, buf1, f_dbg,
                       GRect(0, (int16_t)(top_y + line_h + scale_px(4, face_r)), bounds.size.w, line_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, buf4, f_dbg,
                       GRect(0, (int16_t)(top_y + 2 * line_h + scale_px(8, face_r)), bounds.size.w, line_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, buf2, f_dbg,
                       GRect(0, (int16_t)(top_y + 3 * line_h + scale_px(12, face_r)), bounds.size.w, line_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, buf3, f_dbg,
                       GRect(0, (int16_t)(top_y + 4 * line_h + scale_px(16, face_r)), bounds.size.w, line_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    const GFont f_hint = (min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18)
                                          : fonts_get_system_font(FONT_KEY_GOTHIC_14);
    const int16_t hint_h = (min_dim >= 200) ? scale_px(22, face_r) : scale_px(18, face_r);
    graphics_draw_text(ctx, "Tap to exit debug", f_hint,
                       GRect(0, (int16_t)(bounds.size.h - hint_h), bounds.size.w, hint_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }

  // Loading/progress screen: avoid flashing obviously wrong times while data is still arriving.
  if (!(have_loc && have_sun && have_moon)) {
    graphics_context_set_text_color(ctx, GColorWhite);
    const bool big = (min_dim >= 200);
    const GFont f_title = big ? fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD)
                              : fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    const GFont f_body = big ? fonts_get_system_font(FONT_KEY_GOTHIC_24)
                             : fonts_get_system_font(FONT_KEY_GOTHIC_18);
    const GFont f_prog = big ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                             : fonts_get_system_font(FONT_KEY_GOTHIC_14);

    const int16_t title_y = (int16_t)scale_px(18, face_r);
    const int16_t msg_y   = (int16_t)(title_y + scale_px(28, face_r));
    const int16_t prog_y  = (int16_t)(msg_y + scale_px(26, face_r));

    graphics_draw_text(ctx, "Loading\u2026", f_title,
                       GRect(0, title_y, bounds.size.w, scale_px(28, face_r)),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    const char *msg = "Waiting for data";
    if (!have_loc) msg = "Getting location";
    else if (!have_sun) msg = "Computing sun";
    else if (!have_moon) msg = "Waiting for moon";

    graphics_draw_text(ctx, msg, f_body,
                       GRect(0, msg_y, bounds.size.w, scale_px(26, face_r)),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    // Progress: 3 steps (loc, sun, moon)
    int done = (have_loc ? 1 : 0) + (have_sun ? 1 : 0) + (have_moon ? 1 : 0);
    char prog[16];
    snprintf(prog, sizeof(prog), "%d/3", done);
    const int16_t prog_h = scale_px(18, face_r);
    graphics_draw_text(ctx, prog, f_prog,
                       GRect(0, prog_y, bounds.size.w, prog_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    // Simple spinner hand
    const int32_t a = (int32_t)((time(NULL) % 60) * (TRIG_MAX_ANGLE / 60));
    const int16_t r0 = scale_px(10, face_r);
    const int16_t r1 = scale_px(22, face_r);
    // Place spinner slightly lower to avoid touching the status text on small screens.
    const int16_t prog_pad = scale_px(6, face_r);
    const int16_t min_cy = (int16_t)(prog_y + prog_h + prog_pad + r1);
    int16_t cy = (int16_t)(c.y + scale_px(18, face_r));
    if (cy < min_cy) cy = min_cy;
    cy = (int16_t)MIN((int)(bounds.size.h - (r1 + scale_px(6, face_r))), (int)cy);
    const GPoint cc = GPoint(c.x, cy);
    const GPoint p0 = (GPoint){
      .x = (int16_t)(cc.x + (int32_t)sin_lookup(a) * r0 / TRIG_MAX_RATIO),
      .y = (int16_t)(cc.y - (int32_t)cos_lookup(a) * r0 / TRIG_MAX_RATIO),
    };
    const GPoint p1 = (GPoint){
      .x = (int16_t)(cc.x + (int32_t)sin_lookup(a) * r1 / TRIG_MAX_RATIO),
      .y = (int16_t)(cc.y - (int32_t)cos_lookup(a) * r1 / TRIG_MAX_RATIO),
    };
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, p0, p1);
    graphics_draw_circle(ctx, cc, r1);
    return;
  }

#ifdef PBL_COLOR
  const GColor col_solar_night = GColorOxfordBlue;
  const GColor col_solar_day = GColorCeleste;
  const GColor col_moon_base = GColorDarkGray;
  const GColor col_moon_up = GColorWhite;
#else
  const GColor col_solar_night = GColorBlack;
  const GColor col_solar_day = GColorWhite;
  // On B/W displays, use a darker shade for the "moon down" background ring so the "moon up"
  // segment (white) is distinguishable.
  const GColor col_moon_base = GColorDarkGray;
  const GColor col_moon_up = GColorWhite;
#endif

  const uint16_t moon_inset = (uint16_t)scale_px(22, face_r);
#ifdef PBL_COLOR
  const uint16_t moon_base_thickness = (uint16_t)scale_px(5, face_r);
  const uint16_t moon_up_thickness = (uint16_t)scale_px(5, face_r);
#else
  const uint16_t moon_base_thickness = (uint16_t)scale_px(2, face_r);
  const uint16_t moon_up_thickness = (uint16_t)scale_px(5, face_r);
#endif

  // Solar disk inset: inner edge of ring is at (moon_inset + thickness/2)
  const uint16_t solar_inset = (uint16_t)(moon_inset + (moon_up_thickness / 2));
  // Night wedge should be slightly smaller than the day disk.
  // Make it a bit bigger by reducing the extra inset.
  const uint16_t night_inset = (uint16_t)(solar_inset + (uint16_t)scale_px(1, face_r));
  bool top_is_night = false;

  // Paint order as requested:
  // 1) dark moon background as a disk (gets cut out by the solar day disk)
  draw_ring_base_disk(ctx, bounds, moon_inset, moon_base_thickness, col_moon_base);

  // 2) moon-up ring segment as an arc
  if (moon_times && moon_times->valid) {
    if (moon_times->always_up) {
      draw_ring_arc(ctx, bounds, moon_inset, moon_up_thickness, 0, TRIG_MAX_ANGLE, col_moon_up);
    } else if (!moon_times->always_down) {
      const int32_t a_rise = angle_from_local_minutes_24h(moon_times->moonrise_min);
      const int32_t a_set  = angle_from_local_minutes_24h(moon_times->moonset_min);
      draw_ring_arc(ctx, bounds, moon_inset, moon_up_thickness, a_rise, a_set, col_moon_up);
    }
  }

  // 3) solar night disc as a circle
  if (sun_times && sun_times->valid) {
    graphics_context_set_fill_color(ctx, sun_times->always_day ? col_solar_day : col_solar_night);
    graphics_fill_circle(ctx, c, (int16_t)(MIN(bounds.size.w, bounds.size.h) / 2 - solar_inset));

    // 4) day wedge (radius reduced by a little)
    if (sun_times->always_day) {
      top_is_night = false;
    } else if (sun_times->always_night) {
      top_is_night = true;
    } else {
      const int32_t a_sunrise = angle_from_local_minutes_24h(sun_times->sunrise_min);
      const int32_t a_sunset  = angle_from_local_minutes_24h(sun_times->sunset_min);
      fill_radial_wedge(ctx, bounds, night_inset, a_sunrise, a_sunset, col_solar_day);
      top_is_night = !angle_in_sweep(0, a_sunrise, a_sunset);
    }
  }

  draw_outer_scale(ctx, bounds, moon_inset, moon_up_thickness);

  // Moon phase disk
  {
    const int moon_r = scale_px(9, face_r);
    const GPoint moon_c = GPoint(c.x, (int16_t)(c.y + min_dim / 5));
    double phase = moon_phase_0_1(time(NULL)); // fallback
    if (have_phase) {
      int32_t p = moon_phase_e6;
      if (p < 0) p = 0;
      if (p > 1000000) p = 1000000;
      phase = (double)p / 1000000.0;
    }
    draw_moon(ctx, moon_c, moon_r, phase);
  }

  struct tm tm_loc;
  int minutes = 0;
  if (loc && loc->valid) {
    if (!get_location_local_tm(loc, &tm_loc, &minutes)) minutes = 0;
  } else {
    time_t now_utc = time(NULL);
    struct tm *local_tm = localtime(&now_utc);
    minutes = local_tm ? (local_tm->tm_hour * 60 + local_tm->tm_min) : 0;
  }

  const int32_t hand_angle = angle_from_local_minutes_24h(minutes);
  const int16_t solar_r = (int16_t)(face_r - (int16_t)solar_inset - scale_px(2, face_r));
  const int16_t hand_len = (int16_t)MIN(solar_r, (int16_t)(face_r - scale_px(18, face_r)));

  // Yes-watch-like tapered arrow hand (filled polygon + subtle outline)
  if (s_hand_path && hand_len > 16) {
    const int32_t ux = sin_lookup(hand_angle);
    const int32_t uy = -cos_lookup(hand_angle);
    const int32_t px = cos_lookup(hand_angle);
    const int32_t py = sin_lookup(hand_angle);

    const int16_t base_r = scale_px(4, face_r);
    const int16_t head_len = scale_px(10, face_r);
    int16_t neck_r = (int16_t)(hand_len - head_len);
    if (neck_r < (int16_t)(base_r + scale_px(4, face_r))) neck_r = (int16_t)(base_r + scale_px(4, face_r));
    const int16_t tip_r  = hand_len;

#ifdef PBL_COLOR
    const int16_t base_w = scale_px(9, face_r);
    const int16_t neck_w = scale_px(5, face_r);
    const GColor outline = GColorDarkGray;
#else
    const int16_t base_w = scale_px(7, face_r);
    const int16_t neck_w = scale_px(4, face_r);
    const GColor outline = GColorBlack;
#endif

    const GPoint base = (GPoint){
      .x = (int16_t)(c.x + (int32_t)ux * base_r / TRIG_MAX_RATIO),
      .y = (int16_t)(c.y + (int32_t)uy * base_r / TRIG_MAX_RATIO),
    };
    const GPoint neck = (GPoint){
      .x = (int16_t)(c.x + (int32_t)ux * neck_r / TRIG_MAX_RATIO),
      .y = (int16_t)(c.y + (int32_t)uy * neck_r / TRIG_MAX_RATIO),
    };
    const GPoint tip = (GPoint){
      .x = (int16_t)(c.x + (int32_t)ux * tip_r / TRIG_MAX_RATIO),
      .y = (int16_t)(c.y + (int32_t)uy * tip_r / TRIG_MAX_RATIO),
    };

    const int16_t bdx = (int16_t)((int32_t)px * (base_w / 2) / TRIG_MAX_RATIO);
    const int16_t bdy = (int16_t)((int32_t)py * (base_w / 2) / TRIG_MAX_RATIO);
    const int16_t ndx = (int16_t)((int32_t)px * (neck_w / 2) / TRIG_MAX_RATIO);
    const int16_t ndy = (int16_t)((int32_t)py * (neck_w / 2) / TRIG_MAX_RATIO);

    s_hand_points[0] = GPoint(base.x + bdx, base.y + bdy);
    s_hand_points[1] = GPoint(neck.x + ndx, neck.y + ndy);
    s_hand_points[2] = tip;
    s_hand_points[3] = GPoint(neck.x - ndx, neck.y - ndy);
    s_hand_points[4] = GPoint(base.x - bdx, base.y - bdy);

    s_hand_path->num_points = (uint32_t)(sizeof(s_hand_points) / sizeof(s_hand_points[0]));
    s_hand_path->rotation = 0;
    s_hand_path->offset = GPoint(0, 0);

    graphics_context_set_fill_color(ctx, GColorWhite);
    gpath_draw_filled(ctx, s_hand_path);
    graphics_context_set_stroke_color(ctx, outline);
    graphics_context_set_stroke_width(ctx, 1);
    gpath_draw_outline(ctx, s_hand_path);

    const int16_t hub_r = scale_px(6, face_r);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, c, hub_r);
    graphics_context_set_stroke_color(ctx, outline);
    graphics_draw_circle(ctx, c, hub_r);
  } else {
    const int16_t x = c.x + (int16_t)((int32_t)sin_lookup(hand_angle) * hand_len / TRIG_MAX_RATIO);
    const int16_t y = c.y - (int16_t)((int32_t)cos_lookup(hand_angle) * hand_len / TRIG_MAX_RATIO);
    graphics_context_set_stroke_color(ctx, GColorWhite);
#ifdef PBL_COLOR
    graphics_context_set_stroke_width(ctx, 3);
#else
    graphics_context_set_stroke_width(ctx, 2);
#endif
    graphics_draw_line(ctx, c, GPoint(x, y));
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, c, scale_px(4, face_r));
  }

  char time_buf[6];
  char ampm_buf[3];
  ampm_buf[0] = '\0';
  if (loc && loc->valid) {
    time_t now_utc = time(NULL);
    time_t shifted = now_utc + (time_t)loc->tz_offset_min * 60;
    struct tm *tm_p = gmtime(&shifted);
    if (tm_p) {
      const bool is24 = clock_is_24h_style();
      strftime(time_buf, sizeof(time_buf), is24 ? "%H:%M" : "%I:%M", tm_p);
      if (!is24) {
        strftime(ampm_buf, sizeof(ampm_buf), "%p", tm_p);
      }
    } else {
      strcpy(time_buf, "--:--");
    }
  } else {
    strcpy(time_buf, "--:--");
  }

  // Digital time (top half between center and moon ring)
  {
    const int16_t ring_center_r = (int16_t)(face_r - (int16_t)moon_inset);
    const int16_t ring_inner_r = (int16_t)(ring_center_r - (int16_t)(moon_up_thickness / 2));
    const int16_t y_center = (int16_t)(c.y - ring_inner_r / 2);

    // On both color and B/W: black on day, white on night.
    const GColor time_col = top_is_night ? GColorWhite : GColorBlack;
    graphics_context_set_text_color(ctx, time_col);
    const GFont f_time = (min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD)
                                          : fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    const int16_t time_h = (min_dim >= 200) ? scale_px(30, face_r) : scale_px(24, face_r);

    const GRect time_rect = GRect(0, (int16_t)(y_center - time_h / 2), bounds.size.w, time_h);
    graphics_draw_text(ctx, time_buf, f_time,
                       time_rect,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    // Optional AM/PM label (only in 12h mode)
    if (ampm_buf[0]) {
      const GFont f_ampm = (min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                            : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
      const int16_t pad = scale_px(4, face_r);
      const GSize time_sz = graphics_text_layout_get_content_size(
        time_buf, f_time, GRect(0, 0, bounds.size.w, time_h),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter
      );

      int16_t ampm_x = (int16_t)((bounds.size.w - time_sz.w) / 2 + time_sz.w + pad);
      if (ampm_x < 0) ampm_x = 0;
      const int16_t ampm_w = scale_px(28, face_r);
      if (ampm_x + ampm_w > bounds.size.w) ampm_x = (int16_t)(bounds.size.w - ampm_w);

      graphics_draw_text(ctx, ampm_buf, f_ampm,
                         GRect(ampm_x, time_rect.origin.y, ampm_w, time_rect.size.h),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    }
  }

  // No placeholder text here; startup loading screen handles missing inputs.
}


