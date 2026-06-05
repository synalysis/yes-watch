#include "yes_i18n.h"

static int s_lang = YES_LANG_EN;

typedef struct {
  const char *texts[13];
  const char *weekdays[7];
  const char *compass[8];
} YesLocale;

static const YesLocale LOCALES[] = {
  [YES_LANG_EN] = {
    .texts = {
      "Loading...", "Waiting for data", "Getting location", "Computing sun", "Waiting for moon",
      "Tap to exit debug", "SUN DAY", "SUN NITE", "MOON UP", "MOON DN", "Age", "Moon", "in"
    },
    .weekdays = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" },
    .compass = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" },
  },
  [YES_LANG_DE] = {
    .texts = {
      "Laden...", "Warte auf Daten", "Suche Position", "Berechne Sonne", "Warte auf Mond",
      "Tippen beendet Debug", "SON TAG", "SON NACHT", "MOND OB", "MOND UN", "Alter", "Mond", "in"
    },
    .weekdays = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa" },
    .compass = { "N", "NO", "O", "SO", "S", "SW", "W", "NW" },
  },
  [YES_LANG_FR] = {
    .texts = {
      "Chargement...", "Attente données", "Position...", "Calcul soleil", "Attente lune",
      "Touchez pour quitter", "SOL JOUR", "SOL NUIT", "LUNE HA", "LUNE BA", "Age", "Lune", "dans"
    },
    .weekdays = { "Di", "Lu", "Ma", "Me", "Je", "Ve", "Sa" },
    .compass = { "N", "NE", "E", "SE", "S", "SO", "O", "NO" },
  },
  [YES_LANG_ES] = {
    .texts = {
      "Cargando...", "Esperando datos", "Buscando ubic.", "Calc. sol", "Esperando luna",
      "Toque para salir", "SOL DIA", "SOL NOCHE", "LUNA AR", "LUNA AB", "Edad", "Luna", "en"
    },
    .weekdays = { "Do", "Lu", "Ma", "Mi", "Ju", "Vi", "Sa" },
    .compass = { "N", "NE", "E", "SE", "S", "SO", "O", "NO" },
  },
  [YES_LANG_PT] = {
    .texts = {
      "Carregando...", "Aguardando dados", "Obtendo local", "Calc. sol", "Aguardando lua",
      "Toque para sair", "SOL DIA", "SOL NOITE", "LUA AC", "LUA AB", "Idade", "Lua", "em"
    },
    .weekdays = { "Do", "Se", "Te", "Qu", "Qi", "Se", "Sa" },
    .compass = { "N", "NE", "E", "SE", "S", "SO", "O", "NO" },
  },
  [YES_LANG_IT] = {
    .texts = {
      "Caricamento...", "Attesa dati", "Cerco posiz.", "Calc. sole", "Attesa luna",
      "Tocca per uscire", "SOL GIO", "SOL NOT", "LUNA SU", "LUNA GI", "Età", "Luna", "tra"
    },
    .weekdays = { "Do", "Lu", "Ma", "Me", "Gi", "Ve", "Sa" },
    .compass = { "N", "NE", "E", "SE", "S", "SO", "O", "NO" },
  },
};

int yes_i18n_normalize_language(int lang) {
  if (lang < YES_LANG_EN || lang > YES_LANG_IT) return YES_LANG_EN;
  return lang;
}

void yes_i18n_set_language(int lang) {
  s_lang = yes_i18n_normalize_language(lang);
}

int yes_i18n_get_language(void) {
  return s_lang;
}

const char *yes_i18n_text(YesTextKey key) {
  const int k = (int)key;
  if (k < (int)YES_TEXT_LOADING || k > (int)YES_TEXT_IN) return "";
  const char *s = LOCALES[s_lang].texts[key];
  return s ? s : LOCALES[YES_LANG_EN].texts[key];
}

const char *yes_i18n_weekday_short(int wday) {
  if (wday < 0 || wday > 6) return "";
  const char *s = LOCALES[s_lang].weekdays[wday];
  return s ? s : LOCALES[YES_LANG_EN].weekdays[wday];
}

const char *yes_i18n_compass(int idx) {
  idx &= 7;
  const char *s = LOCALES[s_lang].compass[idx];
  return s ? s : LOCALES[YES_LANG_EN].compass[idx];
}
