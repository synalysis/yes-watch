#pragma once

typedef enum {
  YES_LANG_EN = 0,
  YES_LANG_DE = 1,
  YES_LANG_FR = 2,
  YES_LANG_ES = 3,
  YES_LANG_PT = 4,
  YES_LANG_IT = 5,
} YesLang;

typedef enum {
  YES_TEXT_LOADING = 0,
  YES_TEXT_WAITING_DATA,
  YES_TEXT_GETTING_LOCATION,
  YES_TEXT_COMPUTING_SUN,
  YES_TEXT_WAITING_MOON,
  YES_TEXT_TAP_EXIT_DEBUG,
  YES_TEXT_SUN_DAY,
  YES_TEXT_SUN_NIGHT,
  YES_TEXT_MOON_UP,
  YES_TEXT_MOON_DOWN,
  YES_TEXT_AGE,
  YES_TEXT_MOON,
  YES_TEXT_IN,
} YesTextKey;

int yes_i18n_normalize_language(int lang);
void yes_i18n_set_language(int lang);
int yes_i18n_get_language(void);
const char *yes_i18n_text(YesTextKey key);
const char *yes_i18n_weekday_short(int wday);
const char *yes_i18n_compass(int idx);
