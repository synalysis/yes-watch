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

  // Fill clockwise from start->end (our angle convention matches Pebble's: 0 at top, increasing clockwise).
  // Handle wrap across 0° explicitly.
  if (s < e) {
    graphics_fill_radial(ctx, disk_rect, GOvalScaleModeFitCircle, thickness, s, e);
  } else {
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

#ifndef PBL_ROUND
static void draw_tide_icon(GContext *ctx, GPoint origin, int16_t w, int16_t h, GColor col) {
  // Ocean wave glyph: two stacked sine-like waves.
  if (w < 8) w = 8;
  if (h < 4) h = 4;
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 1);

  const int16_t amp = (int16_t)((h / 4) > 1 ? (h / 4) : 1);
  const int16_t y0 = (int16_t)(origin.y + h / 3);
  const int16_t y1 = (int16_t)(origin.y + (h * 2) / 3);

  // Sample a sine wave across width.
  const int samples = (w >= 18) ? 12 : 8;
  for (int wave = 0; wave < 2; wave++) {
    const int16_t y_base = (wave == 0) ? y0 : y1;
    GPoint prev = GPoint(origin.x, y_base);
    for (int i = 1; i <= samples; i++) {
      const int16_t x = (int16_t)(origin.x + (int32_t)w * i / samples);
      // One full sine period across the icon width.
      const int32_t a = (int32_t)((int64_t)TRIG_MAX_ANGLE * i / samples);
      const int16_t dy = (int16_t)((int32_t)sin_lookup(a) * amp / TRIG_MAX_RATIO);
      const GPoint cur = GPoint(x, (int16_t)(y_base - dy));
      graphics_draw_line(ctx, prev, cur);
      prev = cur;
    }
  }
}

static void draw_tide_clock(GContext *ctx, GRect bounds, int16_t face_r, int16_t corner_pad,
                            bool have_tide, int32_t tide_last_unix, int32_t tide_next_unix, bool tide_next_is_high,
                            int16_t tide_level_x10,
                            bool tide_level_is_ft,
                            GColor color_base, GColor color_prog, GColor color_text) {
  if (!have_tide) return;
  if (tide_last_unix <= 0 || tide_next_unix <= 0) return;
  if (tide_next_unix <= tide_last_unix) return;

  const time_t now = time(NULL);
  const int32_t span = (int32_t)(tide_next_unix - tide_last_unix);
  int32_t t = (int32_t)(now - (time_t)tide_last_unix);
  if (t < 0) t = 0;
  if (t > span) t = span;

  const int16_t r_path = scale_px(10, face_r);
  const uint16_t stroke = (uint16_t)scale_px(2, face_r);
  const int16_t r_out = (int16_t)(r_path + (int16_t)(stroke / 2));
  const int16_t content_w = (int16_t)(2 * r_out);
  const int16_t icon_w = scale_px(12, face_r);
  const int16_t icon_h = scale_px(8, face_r);
  const int16_t gap = scale_px(3, face_r);
  // Fixed layout: [icon][gap][content box of width content_w] anchored to bottom-right.
  const int16_t right = (int16_t)(bounds.origin.x + bounds.size.w - corner_pad);
  const int16_t bottom = (int16_t)(bounds.origin.y + bounds.size.h - corner_pad);
  const int16_t content_x0 = (int16_t)(right - content_w);
  const int16_t content_y0 = (int16_t)(bottom - content_w);
  const int16_t icon_x0 = (int16_t)(content_x0 - gap - icon_w);
  const int16_t icon_y0 = (int16_t)(bottom - icon_h);
  draw_tide_icon(ctx, GPoint(icon_x0, icon_y0), icon_w, icon_h, color_text);

  const int16_t cx = (int16_t)(content_x0 + r_out);
  const int16_t cy = (int16_t)(content_y0 + r_out);
  const GRect rect = GRect(cx - r_path, cy - r_path, (int16_t)(2 * r_path), (int16_t)(2 * r_path));

  // Cycle 3 views every 5 seconds:
  // 0) progress ring, 1) minutes to next H/L, 2) current level + trend arrow.
  const int mode = (int)((now / 5) % 3);

  graphics_context_set_text_color(ctx, color_text);
  const GFont f_small = (MIN(bounds.size.w, bounds.size.h) >= 200)
    ? fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD)
    : fonts_get_system_font(FONT_KEY_GOTHIC_09);
  const char *label = tide_next_is_high ? "H" : "L";

  const bool rising = tide_next_is_high;

  if (mode == 0) {
    // Ring view: draw base ring + progress + label + trend arrow.
    graphics_context_set_stroke_width(ctx, stroke);
    graphics_context_set_stroke_color(ctx, color_base);
    graphics_draw_arc(ctx, rect, GOvalScaleModeFitCircle, 0, TRIG_MAX_ANGLE);

    // Label at top (small)
    graphics_draw_text(ctx, label, f_small,
                       GRect(cx - r_path, (int16_t)(cy - r_path), (int16_t)(2 * r_path), (int16_t)((r_path / 2) + 1)),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    // Progress arc
    const int32_t end = (int32_t)((int64_t)TRIG_MAX_ANGLE * (int64_t)t / (int64_t)span);
    if (end > 0) {
      graphics_context_set_stroke_color(ctx, color_prog);
      graphics_draw_arc(ctx, rect, GOvalScaleModeFitCircle, 0, end);
    }
    // Trend arrow (bottom). Keep it comfortably away from the H/L label above,
    // but still clear of the ring stroke.
    int16_t s = (int16_t)(r_path / 3);
    if (s < 3) s = 3;
    const int16_t ay = (int16_t)(cy + r_path - s - 3);
    const GPoint top = GPoint(cx, (int16_t)(ay - s));
    const GPoint bot = GPoint(cx, (int16_t)(ay + s));
    const GPoint left = GPoint((int16_t)(cx - s), ay);
    const GPoint right = GPoint((int16_t)(cx + s), ay);
    graphics_context_set_stroke_color(ctx, color_text);
    graphics_context_set_stroke_width(ctx, 1);
    if (rising) {
      graphics_draw_line(ctx, top, left);
      graphics_draw_line(ctx, top, right);
    } else {
      graphics_draw_line(ctx, bot, left);
      graphics_draw_line(ctx, bot, right);
    }
    return;
  }

  // Text-only views (no circle): show a tide icon at left and text at right, bottom-aligned.
  char buf[12];
  buf[0] = '\0';

  if (mode == 1) {
    // Option A: minutes to next extreme (include H/L for context).
    int32_t mins = (int32_t)((tide_next_unix - (int32_t)now + 30) / 60);
    if (mins < 0) mins = 0;
    if (mins > 999) mins = 999;
    const int32_t hh = mins / 60;
    const int32_t mm = mins % 60;
    // Two-line layout: label above time.
    char time_buf[8];
    snprintf(time_buf, sizeof(time_buf), "%ld:%02ld", (long)hh, (long)mm);

    const GFont f_lbl = (MIN(bounds.size.w, bounds.size.h) >= 200)
      ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
    const GFont f_time = (MIN(bounds.size.w, bounds.size.h) >= 200)
      ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

    const int16_t block_w = (int16_t)(right - content_x0);
    const int16_t lbl_h = scale_px(12, face_r);
    const int16_t time_h = scale_px(14, face_r);
    const int16_t top_y = (int16_t)(bottom - (lbl_h + time_h));
    graphics_draw_text(ctx, label, f_lbl,
                       GRect(content_x0, top_y, block_w, lbl_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    graphics_draw_text(ctx, time_buf, f_time,
                       GRect(content_x0, (int16_t)(top_y + lbl_h - scale_px(1, face_r)), block_w, time_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    return;
  }

  // mode == 2: level view (two-line, with same "L/H" semantics as countdown)
  {
    int v = (int)tide_level_x10;
    const bool neg = (v < 0);
    if (v < 0) v = -v;
    const int ip = v / 10;
    const int fp = v % 10;
    const char *unit = tide_level_is_ft ? "ft" : "m";
    snprintf(buf, sizeof(buf), neg ? "-%d.%d%s" : "%d.%d%s", ip, fp, unit);

    const int16_t block_w = (int16_t)(right - content_x0);

    const GFont f_lbl = (MIN(bounds.size.w, bounds.size.h) >= 200)
      ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
    const GFont f_val = (MIN(bounds.size.w, bounds.size.h) >= 200)
      ? fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD)
      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

    // Bottom-align the value using actual rendered height (matches weather temp style).
    const GSize val_sz = graphics_text_layout_get_content_size(
      buf, f_val, GRect(0, 0, block_w, scale_px(24, face_r)),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight
    );
    const int16_t val_h = (int16_t)val_sz.h;
    const int16_t val_y = (int16_t)(bottom - val_h);

    const GSize lbl_sz = graphics_text_layout_get_content_size(
      label, f_lbl, GRect(0, 0, block_w, scale_px(24, face_r)),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight
    );
    const int16_t lbl_h = (int16_t)lbl_sz.h;
    const int16_t lbl_y = (int16_t)(val_y - lbl_h + scale_px(1, face_r));

    // Trend arrow (triangle) in the label line (left), and L/H on the right.
    int16_t s = (int16_t)(r_path / 4);
    if (s < 3) s = 3;
    const int16_t ax = (int16_t)(content_x0 + s + 1);
    const int16_t ay = (int16_t)(lbl_y + lbl_h / 2);
    const GPoint atop = GPoint(ax, (int16_t)(ay - s));
    const GPoint abot = GPoint(ax, (int16_t)(ay + s));
    const GPoint aleft = GPoint((int16_t)(ax - s), ay);
    const GPoint aright = GPoint((int16_t)(ax + s), ay);
    graphics_context_set_stroke_color(ctx, color_text);
    graphics_context_set_stroke_width(ctx, 1);
    if (rising) {
      graphics_draw_line(ctx, atop, aleft);
      graphics_draw_line(ctx, atop, aright);
    } else {
      graphics_draw_line(ctx, abot, aleft);
      graphics_draw_line(ctx, abot, aright);
    }

    graphics_draw_text(ctx, label, f_lbl,
                       GRect(content_x0, lbl_y, block_w, lbl_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    graphics_draw_text(ctx, buf, f_val,
                       GRect(content_x0, val_y, block_w, val_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    return;
  }
}

static void draw_steps_icon(GContext *ctx, GPoint center, int16_t s, GColor col) {
  // Two literal footprints: heel + sole + toes.
  // This reads better than abstract shapes at tiny sizes.
  if (s < 12) s = 12;
  graphics_context_set_fill_color(ctx, col);

  const int16_t heel_r = (int16_t)((s / 6) > 1 ? (s / 6) : 1);
  const int16_t toe_r = (int16_t)((s / 10) > 1 ? (s / 10) : 1);
  const int16_t sole_w = (int16_t)(s * 2 / 5);
  const int16_t sole_h = (int16_t)(s * 3 / 5);
  const uint16_t cr = (uint16_t)((sole_w / 2) > 1 ? (sole_w / 2) : 1);

  // Helper: draw one footprint
  const int16_t dx = (int16_t)(s / 5);
  const int16_t dy = (int16_t)(s / 8);

  for (int i = 0; i < 2; i++) {
    const int16_t sx = (i == 0) ? (int16_t)(-dx) : dx;
    const int16_t sy = (i == 0) ? dy : (int16_t)(-dy);
    const GPoint c0 = GPoint((int16_t)(center.x + sx), (int16_t)(center.y + sy));

    // Heel
    const GPoint heel = GPoint(c0.x, (int16_t)(c0.y + sole_h / 2 - heel_r));
    graphics_fill_circle(ctx, heel, (uint16_t)heel_r);

    // Sole (rounded rectangle)
    const GRect sole = GRect((int16_t)(c0.x - sole_w / 2),
                             (int16_t)(c0.y - sole_h / 2 + toe_r * 2),
                             sole_w, sole_h);
    graphics_fill_rect(ctx, sole, cr, GCornersAll);

    // Toes (3 circles near the top, slightly diagonal)
    const int16_t ty = (int16_t)(sole.origin.y + toe_r);
    const int16_t tx = sole.origin.x;
    graphics_fill_circle(ctx, GPoint((int16_t)(tx + toe_r * 2), ty), (uint16_t)toe_r);
    graphics_fill_circle(ctx, GPoint((int16_t)(tx + toe_r * 4), (int16_t)(ty + toe_r)), (uint16_t)toe_r);
    graphics_fill_circle(ctx, GPoint((int16_t)(tx + toe_r * 6), (int16_t)(ty + toe_r * 2)), (uint16_t)toe_r);
  }
}

static void draw_weather_icon(GContext *ctx, GPoint c, int16_t s, uint8_t code, bool is_day, GColor col) {
  (void)is_day;
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_fill_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 1);

  // Very small icon set based on WMO weather_code categories.
  // 0: clear, 1-3: partly cloudy, 45/48: fog, 51-67: drizzle/rain, 71-77: snow, 80-99: showers/thunder.
  if (code == 0) {
    graphics_draw_circle(ctx, c, (uint16_t)(s / 3));
    // Rays
    for (int i = 0; i < 8; i++) {
      const int32_t a = (int32_t)(i * (TRIG_MAX_ANGLE / 8));
      const int16_t r0 = (int16_t)(s / 2);
      const int16_t r1 = (int16_t)(s / 2 + s / 6);
      const GPoint p0 = GPoint((int16_t)(c.x + (int32_t)sin_lookup(a) * r0 / TRIG_MAX_RATIO),
                               (int16_t)(c.y - (int32_t)cos_lookup(a) * r0 / TRIG_MAX_RATIO));
      const GPoint p1 = GPoint((int16_t)(c.x + (int32_t)sin_lookup(a) * r1 / TRIG_MAX_RATIO),
                               (int16_t)(c.y - (int32_t)cos_lookup(a) * r1 / TRIG_MAX_RATIO));
      graphics_draw_line(ctx, p0, p1);
    }
    return;
  }

  // Cloud base
  const int16_t w = s;
  const int16_t h = (int16_t)(s * 2 / 3);
  const GRect r = GRect(c.x - w / 2, c.y - h / 2, w, h);
  graphics_draw_round_rect(ctx, r, (uint16_t)(h / 2));

  if (code <= 3) return; // partly cloudy: just cloud outline

  if (code == 45 || code == 48) {
    // Fog: 2 lines
    graphics_draw_line(ctx, GPoint(r.origin.x, (int16_t)(r.origin.y + h + 1)),
                       GPoint((int16_t)(r.origin.x + w), (int16_t)(r.origin.y + h + 1)));
    graphics_draw_line(ctx, GPoint(r.origin.x, (int16_t)(r.origin.y + h + 4)),
                       GPoint((int16_t)(r.origin.x + w), (int16_t)(r.origin.y + h + 4)));
    return;
  }

  // Rain / showers / thunder: draw 2-3 drops as short slanted lines.
  const int16_t y0 = (int16_t)(r.origin.y + h + 1);
  graphics_draw_line(ctx, GPoint((int16_t)(c.x - s / 4), y0), GPoint((int16_t)(c.x - s / 6), (int16_t)(y0 + s / 4)));
  graphics_draw_line(ctx, GPoint(c.x, y0), GPoint((int16_t)(c.x + s / 12), (int16_t)(y0 + s / 4)));
  graphics_draw_line(ctx, GPoint((int16_t)(c.x + s / 4), y0), GPoint((int16_t)(c.x + s / 3), (int16_t)(y0 + s / 4)));
}

static int16_t weather_icon_extra_bottom(uint8_t code, int16_t s) {
  // Our drawn weather icon can extend below its nominal (s/2) bounding box due to rain/fog lines.
  if (code == 0) return 0;
  if (code <= 3) return 0;
  if (code == 45 || code == 48) {
    const int16_t extra = (int16_t)(4 - (s / 6));
    return (extra > 0) ? extra : 0;
  }
  return (int16_t)((s / 12) + 1);
}
#endif // !PBL_ROUND

#ifndef PBL_ROUND
typedef struct {
  GContext *ctx;
  GRect bounds;
  int16_t face_r;
  int16_t corner_pad;
  GColor color_txt;
  GColor color_base;
  GColor color_prog;

  // Shared state needed by corner complications
  bool have_tide;
  int32_t tide_last_unix;
  int32_t tide_next_unix;
  bool tide_next_is_high;
  int16_t tide_level_x10;
  bool tide_level_is_ft;

  bool alt_valid;
  int32_t alt_m;
  bool alt_is_ft;

  bool have_weather;
  int16_t weather_temp_c10;
  uint8_t weather_code;
  bool weather_is_day;
  bool weather_is_f;
  int16_t weather_wind_spd_x10;
  int16_t weather_wind_dir_deg;
  int16_t weather_precip_x10;
  int16_t weather_uv_x10;
  int16_t weather_pressure_hpa_x10;

  bool battery_alert;
  uint8_t battery_percent;

  bool have_phase;
  int32_t moon_phase_e6;

  const GeoLoc *loc;
  const SunTimes *sun_times;
  const MoonTimes *moon_times;

  int min_dim;
} CornerCtx;

typedef bool (*CornerAvailFn)(const CornerCtx *c);
typedef void (*CornerDrawFn)(const CornerCtx *c);

// Generic "slot" helper: if any exclusive comp is available, show the first such comp.
// Otherwise, cycle through all available comps every 5 seconds.
typedef struct {
  CornerAvailFn avail;
  CornerDrawFn draw;
  bool exclusive;
} SlotComp;

static int slot_pick_index(const SlotComp *comps, int count, const CornerCtx *c, time_t now) {
  // Exclusive-first
  for (int i = 0; i < count; i++) {
    if (comps[i].exclusive && (!comps[i].avail || comps[i].avail(c))) return i;
  }
  // Cycle among available
  int idxs[12];
  int n = 0;
  for (int i = 0; i < count && n < (int)(sizeof(idxs) / sizeof(idxs[0])); i++) {
    if (!comps[i].avail || comps[i].avail(c)) idxs[n++] = i;
  }
  if (n <= 0) return -1;
  const int k = (int)((now / 5) % (time_t)n);
  return idxs[k];
}

// --- Bottom-right complication implementations (tide absent) ---
static bool br_avail_alt(const CornerCtx *c) { return c->alt_valid; }
static bool br_avail_sun(const CornerCtx *c) { return c->sun_times && c->sun_times->valid; }
static bool br_avail_moon(const CornerCtx *c) { return c->moon_times && c->moon_times->valid; }
static bool br_avail_age(const CornerCtx *c) { return c->have_phase; }

static int br_compute_now_min(const CornerCtx *c) {
  int now_min = -1;
  if (c->loc && c->loc->valid) {
    time_t now_utc = time(NULL);
    time_t shifted = now_utc + (time_t)c->loc->tz_offset_min * 60;
    struct tm *tm_p = gmtime(&shifted);
    if (tm_p) now_min = tm_p->tm_hour * 60 + tm_p->tm_min;
  }
  if (now_min < 0) {
    time_t now = time(NULL);
    struct tm *tm_p = localtime(&now);
    if (tm_p) now_min = tm_p->tm_hour * 60 + tm_p->tm_min;
  }
  return now_min;
}

static void br_draw_alt(const CornerCtx *c) {
  const int16_t pad = c->corner_pad;
  const int16_t right = (int16_t)(c->bounds.origin.x + c->bounds.size.w - pad);
  const int16_t bottom = (int16_t)(c->bounds.origin.y + c->bounds.size.h - pad);
  const int16_t icon_w = scale_px(12, c->face_r);
  const int16_t icon_h = scale_px(10, c->face_r);
  const int16_t gap = scale_px(3, c->face_r);
  const int16_t icon_x0 = (int16_t)(right - icon_w - gap - scale_px(36, c->face_r));
  const int16_t icon_y0 = (int16_t)(bottom - icon_h);
  graphics_context_set_stroke_color(c->ctx, c->color_txt);
  graphics_context_set_stroke_width(c->ctx, 1);
  const GPoint a = GPoint(icon_x0, (int16_t)(icon_y0 + icon_h));
  const GPoint b = GPoint((int16_t)(icon_x0 + icon_w / 2), icon_y0);
  const GPoint c2 = GPoint((int16_t)(icon_x0 + icon_w), (int16_t)(icon_y0 + icon_h));
  graphics_draw_line(c->ctx, a, b);
  graphics_draw_line(c->ctx, b, c2);
  const GPoint a2 = GPoint((int16_t)(icon_x0 + icon_w / 3), (int16_t)(icon_y0 + icon_h));
  const GPoint b2 = GPoint((int16_t)(icon_x0 + (icon_w * 2) / 3), (int16_t)(icon_y0 + icon_h / 3));
  const GPoint c3 = GPoint((int16_t)(icon_x0 + icon_w), (int16_t)(icon_y0 + icon_h));
  graphics_draw_line(c->ctx, a2, b2);
  graphics_draw_line(c->ctx, b2, c3);

  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  int32_t v = c->alt_m;
  const bool neg = (v < 0);
  if (v < 0) v = -v;
  int32_t disp = v;
  const char *unit = "m";
  if (c->alt_is_ft) {
    disp = (int32_t)((v * 3281 + 500) / 1000);
    unit = "ft";
  }
  char abuf[16];
  snprintf(abuf, sizeof(abuf), "%s%ld%s", (neg ? "-" : ""), (long)disp, unit);

  const int16_t text_x = (int16_t)(icon_x0 + icon_w + gap);
  const int16_t text_w = (int16_t)(right - text_x);
  const GSize tsz = graphics_text_layout_get_content_size(
    abuf, f, GRect(0, 0, text_w, scale_px(24, c->face_r)),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentRight
  );
  const int16_t th = (int16_t)tsz.h;
  const int16_t ty = (int16_t)(bottom - th);
  graphics_context_set_text_color(c->ctx, c->color_txt);
  graphics_draw_text(c->ctx, abuf, f,
                     GRect(text_x, ty, text_w, th),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

static void br_draw_sun_cd(const CornerCtx *c) {
  const int now_min = br_compute_now_min(c);
  const int16_t pad = c->corner_pad;
  const int16_t right = (int16_t)(c->bounds.origin.x + c->bounds.size.w - pad);
  const int16_t bottom = (int16_t)(c->bounds.origin.y + c->bounds.size.h - pad);
  const int16_t w = (int16_t)(c->bounds.size.w / 2);
  const int16_t x0 = (int16_t)(right - w);
  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  char buf[24];
  buf[0] = '\0';

  if (c->sun_times->always_day) {
    snprintf(buf, sizeof(buf), "SUN DAY");
  } else if (c->sun_times->always_night) {
    snprintf(buf, sizeof(buf), "SUN NITE");
  } else if (now_min >= 0) {
    const int sr = c->sun_times->sunrise_min;
    const int ss = c->sun_times->sunset_min;
    const bool to_sunrise = (now_min < sr) || (now_min >= ss);
    int dmin = 0;
    const char *lab = "SR";
    if (to_sunrise) {
      lab = "SR";
      if (now_min < sr) dmin = sr - now_min;
      else dmin = (1440 - now_min) + sr;
    } else {
      lab = "SS";
      dmin = ss - now_min;
    }
    if (dmin < 0) dmin = 0;
    const int hh = dmin / 60;
    const int mm = dmin % 60;
    snprintf(buf, sizeof(buf), "%s in %d:%02d", lab, hh, mm);
  }

  const GSize tsz = graphics_text_layout_get_content_size(
    buf, f, GRect(0, 0, w, scale_px(24, c->face_r)),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentRight
  );
  const int16_t th = (int16_t)tsz.h;
  const int16_t ty = (int16_t)(bottom - th);
  graphics_context_set_text_color(c->ctx, c->color_txt);
  graphics_draw_text(c->ctx, buf, f,
                     GRect(x0, ty, w, th),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

static void br_draw_moon_cd(const CornerCtx *c) {
  const int now_min = br_compute_now_min(c);
  const int16_t pad = c->corner_pad;
  const int16_t right = (int16_t)(c->bounds.origin.x + c->bounds.size.w - pad);
  const int16_t bottom = (int16_t)(c->bounds.origin.y + c->bounds.size.h - pad);
  const int16_t w = (int16_t)(c->bounds.size.w / 2);
  const int16_t x0 = (int16_t)(right - w);
  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  char buf[24];
  buf[0] = '\0';

  if (c->moon_times->always_up) {
    snprintf(buf, sizeof(buf), "MOON UP");
  } else if (c->moon_times->always_down) {
    snprintf(buf, sizeof(buf), "MOON DN");
  } else if (now_min >= 0) {
    const int mr = c->moon_times->moonrise_min;
    const int ms = c->moon_times->moonset_min;
    int d_mr = (mr >= now_min) ? (mr - now_min) : ((1440 - now_min) + mr);
    int d_ms = (ms >= now_min) ? (ms - now_min) : ((1440 - now_min) + ms);
    const bool next_is_mr = (d_mr <= d_ms);
    const int dmin = next_is_mr ? d_mr : d_ms;
    const int hh = dmin / 60;
    const int mm = dmin % 60;
    snprintf(buf, sizeof(buf), "%s in %d:%02d", next_is_mr ? "MR" : "MS", hh, mm);
  }

  const GSize tsz = graphics_text_layout_get_content_size(
    buf, f, GRect(0, 0, w, scale_px(24, c->face_r)),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentRight
  );
  const int16_t th = (int16_t)tsz.h;
  const int16_t ty = (int16_t)(bottom - th);
  graphics_context_set_text_color(c->ctx, c->color_txt);
  graphics_draw_text(c->ctx, buf, f,
                     GRect(x0, ty, w, th),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

static void br_draw_moon_age(const CornerCtx *c) {
  const int16_t pad = c->corner_pad;
  const int16_t right = (int16_t)(c->bounds.origin.x + c->bounds.size.w - pad);
  const int16_t bottom = (int16_t)(c->bounds.origin.y + c->bounds.size.h - pad);
  const int16_t w = (int16_t)(c->bounds.size.w / 2);
  const int16_t x0 = (int16_t)(right - w);
  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  const int32_t days_x10000 = (int32_t)(((int64_t)c->moon_phase_e6 * 295306LL + 500000LL) / 1000000LL);
  const int32_t days_x10 = (days_x10000 + 500) / 1000;
  const int d = (int)(days_x10 / 10);
  const int frac = (int)(days_x10 % 10);
  char buf[24];
  snprintf(buf, sizeof(buf), "Age %d.%dd", d, frac);

  const GSize tsz = graphics_text_layout_get_content_size(
    buf, f, GRect(0, 0, w, scale_px(24, c->face_r)),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentRight
  );
  const int16_t th = (int16_t)tsz.h;
  const int16_t ty = (int16_t)(bottom - th);
  graphics_context_set_text_color(c->ctx, c->color_txt);
  graphics_draw_text(c->ctx, buf, f,
                     GRect(x0, ty, w, th),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

static bool br_avail_tide(const CornerCtx *c) { return c->have_tide; }
static void br_draw_tide(const CornerCtx *c) {
  draw_tide_clock(c->ctx, c->bounds, c->face_r, c->corner_pad,
                  c->have_tide, c->tide_last_unix, c->tide_next_unix, c->tide_next_is_high,
                  c->tide_level_x10, c->tide_level_is_ft,
                  c->color_base, c->color_prog, c->color_txt);
}

static bool tl_avail_bt(const CornerCtx *c) { (void)c; return !bluetooth_connection_service_peek(); }
static void tl_draw_bt(const CornerCtx *c) {
  const int16_t pad = c->corner_pad;
  const int16_t h = (c->min_dim >= 200) ? scale_px(24, c->face_r) : scale_px(20, c->face_r);
  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  graphics_context_set_text_color(c->ctx, c->color_txt);
  graphics_draw_text(c->ctx, "BT", f,
                     GRect(pad, pad, c->bounds.size.w / 2, h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static bool tl_avail_batt(const CornerCtx *c) { return bluetooth_connection_service_peek() && c->battery_alert; }
static bool tl_avail_steps(const CornerCtx *c) { return bluetooth_connection_service_peek(); }

static void tl_draw_batt(const CornerCtx *c) {
  const int16_t pad = c->corner_pad;
  const int16_t h = (c->min_dim >= 200) ? scale_px(24, c->face_r) : scale_px(20, c->face_r);
  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%", (int)c->battery_percent);
  graphics_context_set_text_color(c->ctx, c->color_txt);
  graphics_draw_text(c->ctx, buf, f,
                     GRect((int16_t)(pad + scale_px(18, c->face_r)), pad, c->bounds.size.w / 2, h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void tl_draw_steps(const CornerCtx *c) {
  const int16_t pad = c->corner_pad;
  const int16_t h = (c->min_dim >= 200) ? scale_px(24, c->face_r) : scale_px(20, c->face_r);
  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  // Health calls can be surprisingly expensive on some platforms; cache at most once per minute.
  static int s_steps_cached = -1;
  static time_t s_steps_cached_at = 0;
  const time_t now = time(NULL);
  if (s_steps_cached_at == 0 || (now - s_steps_cached_at) >= 60) {
    int steps = -1;
    const time_t t0 = time_start_of_today();
    const HealthServiceAccessibilityMask m = health_service_metric_accessible(HealthMetricStepCount, t0, now);
    if ((m & HealthServiceAccessibilityMaskAvailable) != 0) {
      steps = (int)health_service_sum_today(HealthMetricStepCount);
    }
    s_steps_cached = steps;
    s_steps_cached_at = now;
  }
  const int steps = s_steps_cached;
  char buf[16];
  if (steps < 0) {
    strcpy(buf, "--");
  } else if (steps >= 10000) {
    snprintf(buf, sizeof(buf), "%dk", steps / 1000);
  } else {
    snprintf(buf, sizeof(buf), "%d", steps);
  }
  graphics_context_set_text_color(c->ctx, c->color_txt);
  const int16_t icon_s = scale_px(14, c->face_r);
  draw_steps_icon(c->ctx, GPoint((int16_t)(pad + icon_s / 2), (int16_t)(pad + h / 2)), icon_s, c->color_txt);
  graphics_draw_text(c->ctx, buf, f,
                     GRect((int16_t)(pad + scale_px(18, c->face_r)), pad, c->bounds.size.w / 2, h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

// --- Weather slot (bottom-left) ---
static bool wx_avail_temp(const CornerCtx *c) { return c->have_weather; }
static bool wx_avail_wind(const CornerCtx *c) { return c->have_weather && c->weather_wind_spd_x10 != 0; }
static bool wx_avail_precip(const CornerCtx *c) { return c->have_weather && c->weather_precip_x10 != 0; }
static bool wx_avail_uv(const CornerCtx *c) { return c->have_weather && c->weather_uv_x10 > 0; }
static bool wx_avail_p(const CornerCtx *c) { return c->have_weather && c->weather_pressure_hpa_x10 != 0; }

static void wx_draw_common(const CornerCtx *c, const char *text, GFont f_use) {
  const int16_t pad = c->corner_pad;
  const int16_t icon_s = scale_px(16, c->face_r);
  const int16_t icon_bottom = (int16_t)(c->bounds.size.h - c->corner_pad);
  const int16_t extra = weather_icon_extra_bottom(c->weather_code, icon_s);
  const int16_t cy = (int16_t)(icon_bottom - (icon_s / 2) - extra);
  const GPoint ic = GPoint((int16_t)(pad + icon_s / 2), cy);
  draw_weather_icon(c->ctx, ic, icon_s, c->weather_code, c->weather_is_day, c->color_txt);

  const int16_t h = (c->min_dim >= 200) ? scale_px(24, c->face_r) : scale_px(20, c->face_r);
  const int16_t text_x = (int16_t)(pad + icon_s + scale_px(4, c->face_r));
  const int16_t text_w = (int16_t)(c->bounds.size.w / 2);
  const GSize text_sz = graphics_text_layout_get_content_size(
    text, f_use, GRect(0, 0, text_w, h),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft
  );
  int16_t text_h = (int16_t)text_sz.h;
  if (text_h < 1) text_h = h;
  int16_t text_top = (int16_t)(icon_bottom - text_h);
  if (text_top < 0) text_top = 0;
  if (text_top > (int16_t)(c->bounds.size.h - text_h)) text_top = (int16_t)(c->bounds.size.h - text_h);

  graphics_context_set_text_color(c->ctx, c->color_txt);
  graphics_draw_text(c->ctx, text, f_use,
                     GRect(text_x, text_top, text_w, text_h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void wx_draw_temp(const CornerCtx *c) {
  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  int t_disp = (int)c->weather_temp_c10;
  if (c->weather_is_f) {
    const int32_t num = (int32_t)t_disp * 9;
    const int32_t div = (num >= 0) ? ((num + 2) / 5) : ((num - 2) / 5);
    t_disp = (int)(div + 320);
  }
  const int t_abs = t_disp < 0 ? -t_disp : t_disp;
  const int t_int = t_abs / 10;
  char buf[16];
  snprintf(buf, sizeof(buf), "%s%d\u00B0", (t_disp < 0 ? "-" : ""), t_int);
  wx_draw_common(c, buf, f);
}

static void wx_draw_wind(const CornerCtx *c) {
  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  const int spd_x10 = (int)c->weather_wind_spd_x10;
  const int spd_abs = (spd_x10 < 0) ? -spd_x10 : spd_x10;
  const int spd_int = (spd_abs + 5) / 10;
  const int dir = (int)c->weather_wind_dir_deg;
  const int idx = (int)(((dir % 360) + 22) / 45) & 7;
  const char *card[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  char buf[16];
  snprintf(buf, sizeof(buf), "%s %d%s", card[idx], spd_int, c->weather_is_f ? "mph" : "m/s");
  wx_draw_common(c, buf, f);
}

static void wx_draw_precip(const CornerCtx *c) {
  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  const int pr_x10 = (int)c->weather_precip_x10;
  const int pr_abs = (pr_x10 < 0) ? -pr_x10 : pr_x10;
  const int pr_int = pr_abs / 10;
  const int pr_frac = pr_abs % 10;
  char buf[16];
  snprintf(buf, sizeof(buf), "%s%d.%d%s", (pr_x10 < 0 ? "-" : ""), pr_int, pr_frac, c->weather_is_f ? "in" : "mm");
  wx_draw_common(c, buf, f);
}

static void wx_draw_uv(const CornerCtx *c) {
  const GFont f_small = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD)
                                            : fonts_get_system_font(FONT_KEY_GOTHIC_09);
  const int uv_i = ((int)c->weather_uv_x10 + 5) / 10;
  char buf[16];
  snprintf(buf, sizeof(buf), "UV %d", uv_i);
  wx_draw_common(c, buf, f_small);
}

static void wx_draw_pressure(const CornerCtx *c) {
  const GFont f = (c->min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  const int p_i = ((int)c->weather_pressure_hpa_x10 + 5) / 10;
  char buf[16];
  snprintf(buf, sizeof(buf), "%dhPa", p_i);
  wx_draw_common(c, buf, f);
}
#endif

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
                     const SunTimes *sun_times,
                     const MoonTimes *moon_times) {
  const GRect bounds = layer_get_bounds(layer);
  const GPoint c = grect_center_point(&bounds);
  const int min_dim = (int)MIN(bounds.size.w, bounds.size.h);
  const int16_t face_r = (int16_t)(min_dim / 2);
  const int16_t corner_pad = scale_px(6, face_r);
#ifdef PBL_ROUND
  (void)corner_pad;
#endif

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (debug) {
    char buf0[32], buf1[32], buf2[32], buf3[32], buf4[32], buf5[32];

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

    if (have_tide && tide_last_unix > 0 && tide_next_unix > tide_last_unix) {
      const int mins = (int)((tide_next_unix - (int32_t)time(NULL)) / 60);
      snprintf(buf5, sizeof(buf5), "TIDE next %s in %dm", tide_next_is_high ? "H" : "L", mins);
    } else {
      snprintf(buf5, sizeof(buf5), "TIDE: --");
    }

    graphics_context_set_text_color(ctx, GColorWhite);
    const GFont f_dbg0 = (min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD)
                                          : fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    const GFont f_dbg = (min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_24)
                                         : fonts_get_system_font(FONT_KEY_GOTHIC_18);

    const GFont f_hint = (min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18)
                                          : fonts_get_system_font(FONT_KEY_GOTHIC_14);
    const int16_t hint_h = (min_dim >= 200) ? scale_px(22, face_r) : scale_px(18, face_r);

    // Layout: stack lines using actual font heights with minimal gaps so everything fits above the hint.
    const int16_t gap = scale_px(1, face_r);
    const int16_t top_y_default = scale_px(6, face_r);
    const int16_t max_y = (int16_t)(bounds.size.h - hint_h);

    const int16_t h0 = (int16_t)graphics_text_layout_get_content_size(
      buf0, f_dbg0, GRect(0, 0, bounds.size.w, bounds.size.h),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter
    ).h;
    const int16_t h = (int16_t)graphics_text_layout_get_content_size(
      "Ag", f_dbg, GRect(0, 0, bounds.size.w, bounds.size.h),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter
    ).h;

    // Total height for 6 lines with 5 gaps.
    int16_t total = (int16_t)(h0 + 5 * h + 5 * gap);
    int16_t top_y = top_y_default;
    int16_t use_gap = gap;
    const int16_t avail = (int16_t)(max_y - top_y_default);
    if (avail > 0 && total > avail) {
      // Tighten up if needed.
      use_gap = 0;
      top_y = 0;
      total = (int16_t)(h0 + 5 * h);
    }

    int16_t y = top_y;
    graphics_draw_text(ctx, buf0, f_dbg0,
                       GRect(0, y, bounds.size.w, h0),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    y = (int16_t)(y + h0 + use_gap);
    graphics_draw_text(ctx, buf1, f_dbg,
                       GRect(0, y, bounds.size.w, h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    y = (int16_t)(y + h + use_gap);
    graphics_draw_text(ctx, buf4, f_dbg,
                       GRect(0, y, bounds.size.w, h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    y = (int16_t)(y + h + use_gap);
    graphics_draw_text(ctx, buf2, f_dbg,
                       GRect(0, y, bounds.size.w, h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    y = (int16_t)(y + h + use_gap);
    graphics_draw_text(ctx, buf3, f_dbg,
                       GRect(0, y, bounds.size.w, h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    y = (int16_t)(y + h + use_gap);
    // Clamp last line to stay above hint.
    int16_t y5 = y;
    if ((int16_t)(y5 + h) > max_y) y5 = (int16_t)(max_y - h);
    graphics_draw_text(ctx, buf5, f_dbg,
                       GRect(0, y5, bounds.size.w, h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

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

  // Corner: top-left is now drawn in the overlay layer (`yes_draw_corners`) so that 5s
  // alternation doesn't force a full watchface redraw.

  // Corner: top-right date (constant)
  {
#ifdef PBL_ROUND
    // Skip corners on round watches to avoid wasted draw calls.
#else
    const int16_t pad = corner_pad;
    const int16_t h = (min_dim >= 200) ? scale_px(24, face_r) : scale_px(20, face_r);
    const GFont f = (min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                                     : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
    const GFont f2 = (min_dim >= 200) ? fonts_get_system_font(FONT_KEY_GOTHIC_14)
                                      : fonts_get_system_font(FONT_KEY_GOTHIC_09);
    char date_buf[16];
    char year_buf[8];
    date_buf[0] = '\0';
    year_buf[0] = '\0';
    if (loc && loc->valid) {
      time_t now_utc = time(NULL);
      time_t shifted = now_utc + (time_t)loc->tz_offset_min * 60;
      struct tm *tm_p = gmtime(&shifted);
      if (tm_p) {
        strftime(date_buf, sizeof(date_buf), "%b %e", tm_p);
        strftime(year_buf, sizeof(year_buf), "%Y", tm_p);
      }
    }
    if (!date_buf[0]) {
      time_t now = time(NULL);
      struct tm *tm_p = localtime(&now);
      if (tm_p) {
        strftime(date_buf, sizeof(date_buf), "%b %e", tm_p);
        strftime(year_buf, sizeof(year_buf), "%Y", tm_p);
      }
      else strcpy(date_buf, "--");
    }
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, date_buf, f,
                       GRect(bounds.size.w / 2, pad, bounds.size.w / 2 - pad, h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    if (year_buf[0]) {
      const int16_t yh = (min_dim >= 200) ? scale_px(16, face_r) : scale_px(14, face_r);
      // Nudge the year up closer to the date so it reads as a single grouped widget.
      const int16_t y = (int16_t)(pad + h - scale_px(8, face_r));
      graphics_draw_text(ctx, year_buf, f2,
                         GRect(bounds.size.w / 2, y, bounds.size.w / 2 - pad, yh),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    }
#endif
  }

  // Corner: bottom-left weather is now drawn in the overlay layer (`yes_draw_corners`) so that
  // 5s alternation doesn't force a full watchface redraw.

  // Bottom-right complications are now drawn in the overlay layer (`yes_draw_corners`) so that
  // 5s alternation doesn't force a full watchface redraw.

  // No placeholder text here; startup loading screen handles missing inputs.
}

#ifndef PBL_ROUND
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
                      const SunTimes *sun_times,
                      const MoonTimes *moon_times) {
  (void)layer;
  // Mirror the old behavior: no corners on debug or loading screen.
  if (debug) return;
  if (!(have_loc && have_sun && have_moon)) return;

  const GRect bounds = layer_get_bounds(layer);
  const int min_dim = (int)MIN(bounds.size.w, bounds.size.h);
  const int16_t face_r = (int16_t)(min_dim / 2);
  const int16_t corner_pad = scale_px(6, face_r);

  // Construct a shared context for slot implementations.
  CornerCtx cc = (CornerCtx){
    .ctx = ctx,
    .bounds = bounds,
    .face_r = face_r,
    .corner_pad = corner_pad,
    .color_txt = GColorWhite,
    .color_base = GColorDarkGray,
    .color_prog = GColorWhite,
    .have_tide = have_tide,
    .tide_last_unix = tide_last_unix,
    .tide_next_unix = tide_next_unix,
    .tide_next_is_high = tide_next_is_high,
    .tide_level_x10 = tide_level_x10,
    .tide_level_is_ft = tide_level_is_ft,
    .alt_valid = alt_valid,
    .alt_m = alt_m,
    .alt_is_ft = alt_is_ft,
    .have_weather = have_weather,
    .weather_temp_c10 = weather_temp_c10,
    .weather_code = weather_code,
    .weather_is_day = weather_is_day,
    .weather_is_f = weather_is_f,
    .weather_wind_spd_x10 = weather_wind_spd_x10,
    .weather_wind_dir_deg = weather_wind_dir_deg,
    .weather_precip_x10 = weather_precip_x10,
    .weather_uv_x10 = weather_uv_x10,
    .weather_pressure_hpa_x10 = weather_pressure_hpa_x10,
    .battery_alert = battery_alert,
    .battery_percent = battery_percent,
    .have_phase = have_phase,
    .moon_phase_e6 = moon_phase_e6,
    .loc = loc,
    .sun_times = sun_times,
    .moon_times = moon_times,
    .min_dim = min_dim,
  };

  // Top-left slot
  {
    const SlotComp comps[] = {
      { tl_avail_bt,    tl_draw_bt,    true  },
      { tl_avail_batt,  tl_draw_batt,  false },
      { tl_avail_steps, tl_draw_steps, false },
    };
    const int which = slot_pick_index(comps, (int)(sizeof(comps) / sizeof(comps[0])), &cc, time(NULL));
    if (which >= 0) comps[which].draw(&cc);
  }

  // Top-right date remains in face layer (static); keep corners layer lightweight.

  // Bottom-left weather slot
  if (have_weather) {
    const SlotComp comps[] = {
      { wx_avail_temp,   wx_draw_temp,     false },
      { wx_avail_wind,   wx_draw_wind,     false },
      { wx_avail_precip, wx_draw_precip,   false },
      { wx_avail_uv,     wx_draw_uv,       false },
      { wx_avail_p,      wx_draw_pressure, false },
    };
    const int which = slot_pick_index(comps, (int)(sizeof(comps) / sizeof(comps[0])), &cc, time(NULL));
    if (which >= 0) comps[which].draw(&cc);
  }

  // Bottom-right slot
  {
    const SlotComp comps[] = {
      { br_avail_tide, br_draw_tide,     true  },
      { br_avail_alt,  br_draw_alt,      false },
      { br_avail_sun,  br_draw_sun_cd,   false },
      { br_avail_moon, br_draw_moon_cd,  false },
      { br_avail_age,  br_draw_moon_age, false },
    };
    const int which = slot_pick_index(comps, (int)(sizeof(comps) / sizeof(comps[0])), &cc, time(NULL));
    if (which >= 0) comps[which].draw(&cc);
  }
}
#else
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
                      const SunTimes *sun_times,
                      const MoonTimes *moon_times) {
  (void)layer; (void)ctx; (void)debug; (void)have_loc; (void)have_sun; (void)have_moon;
  (void)have_tide; (void)tide_last_unix; (void)tide_next_unix; (void)tide_next_is_high; (void)tide_level_x10; (void)tide_level_is_ft;
  (void)alt_valid; (void)alt_m; (void)alt_is_ft;
  (void)battery_alert; (void)battery_percent;
  (void)have_weather; (void)weather_temp_c10; (void)weather_code; (void)weather_is_day; (void)weather_is_f;
  (void)weather_wind_spd_x10; (void)weather_wind_dir_deg; (void)weather_precip_x10; (void)weather_uv_x10; (void)weather_pressure_hpa_x10;
  (void)have_phase; (void)moon_phase_e6;
  (void)loc; (void)sun_times; (void)moon_times;
}
#endif


