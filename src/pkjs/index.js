/* global Pebble */

/**
 * Minimal location companion:
 * - Gets phone GPS
 * - Sends lat/lon (E6), timezone offset minutes, unix timestamp
 * - Responds to watch requests
 */

// IMPORTANT: Pebble-tool generates numeric IDs starting at 10000+ for message keys.
// In pkjs, we should use the *string key names* so the tool can map them correctly.
const KEYS = {
  REQUEST_LOC: 'KEY_REQUEST_LOC',
  LAT_E6: 'KEY_LAT_E6',
  LON_E6: 'KEY_LON_E6',
  TZ_OFFSET_MIN: 'KEY_TZ_OFFSET_MIN',
  LOC_UNIX: 'KEY_LOC_UNIX',

  HOME_SUN_STATE: 'KEY_HOME_SUN_STATE',
  HOME_SUNRISE_MIN: 'KEY_HOME_SUNRISE_MIN',
  HOME_SUNSET_MIN: 'KEY_HOME_SUNSET_MIN',
  HOME_MOON_STATE: 'KEY_HOME_MOON_STATE',
  HOME_MOONRISE_MIN: 'KEY_HOME_MOONRISE_MIN',
  HOME_MOONSET_MIN: 'KEY_HOME_MOONSET_MIN',

  MOON_PHASE_E6: 'KEY_MOON_PHASE_E6',

  TIDE_HAVE: 'KEY_TIDE_HAVE',
  TIDE_LAST_UNIX: 'KEY_TIDE_LAST_UNIX',
  TIDE_NEXT_UNIX: 'KEY_TIDE_NEXT_UNIX',
  TIDE_NEXT_IS_HIGH: 'KEY_TIDE_NEXT_IS_HIGH',
  TIDE_LEVEL_X10: 'KEY_TIDE_LEVEL_X10',
  TIDE_LEVEL_IS_FT: 'KEY_TIDE_LEVEL_IS_FT',

  ALT_VALID: 'KEY_ALT_VALID',
  ALT_M: 'KEY_ALT_M',
  ALT_IS_FT: 'KEY_ALT_IS_FT',

  WEATHER_TEMP_C10: 'KEY_WEATHER_TEMP_C10',
  WEATHER_CODE: 'KEY_WEATHER_CODE',
  WEATHER_IS_DAY: 'KEY_WEATHER_IS_DAY',
  WEATHER_IS_F: 'KEY_WEATHER_IS_F',

  WEATHER_WIND_SPD_X10: 'KEY_WEATHER_WIND_SPD_X10',
  WEATHER_WIND_DIR_DEG: 'KEY_WEATHER_WIND_DIR_DEG',
  WEATHER_PRECIP_X10: 'KEY_WEATHER_PRECIP_X10',
  WEATHER_UV_X10: 'KEY_WEATHER_UV_X10',
  WEATHER_PRESSURE_HPA_X10: 'KEY_WEATHER_PRESSURE_HPA_X10',

  USE_INTERNET_FALLBACK: 'KEY_USE_INTERNET_FALLBACK'
};

function log() {
  try {
    // eslint-disable-next-line no-console
    console.log.apply(console, arguments);
  } catch (e) {}
}

// --- AppMessage queue (avoid burst sending that can wedge pypkjs/QEMU) ---
const State = {
  lastSentAtMs: 0,
  lastLocRequestAtMs: 0,
  lastLoc: null,      // {latE6, lonE6, tzOffsetMin}
  lastLocSent: null,  // {latE6, lonE6, tzOffsetMin, ymd} persisted
  lastAstroYmd: 0,

  tideStations: null, // [{id, lat, lng}] cached in-memory
  tideStationsFetchedAtMs: 0,
  lastTideSentAtMs: 0,
  lastTideSuccessAtMs: 0,
  lastTideAttemptAtMs: 0,
  lastTideLoc: null, // {latE6, lonE6}

  lastWeatherSentAtMs: 0,
  lastWeatherSuccessAtMs: 0,
  lastWeatherAttemptAtMs: 0,
  lastWeatherLoc: null, // {latE6, lonE6}

  isSending: false,
  msgQueue: []
};

const TIDE_NEAR_COAST_THRESHOLD_M = 50000; // 50 km
const TIDE_STATION_CACHE_MS = 7 * 24 * 60 * 60 * 1000; // refresh weekly
const TIDE_REFRESH_MS = 60 * 60 * 1000; // refresh predictions hourly
const TIDE_STATION_FETCH_TIMEOUT_MS = 60000; // large one-time download; cache afterwards
const TIDE_RETRY_MS = 2 * 60 * 1000; // retry quickly on failure

const WEATHER_REFRESH_MS = 30 * 60 * 1000; // weather doesn't need to be frequent
const WEATHER_RETRY_MS = 2 * 60 * 1000; // retry quickly on failure

function loadTideStationsFromStorage() {
  try {
    const raw = localStorage.getItem('noaaTideStations');
    const at = parseInt(localStorage.getItem('noaaTideStationsAtMs') || '0', 10);
    if (!raw || !isFinite(at) || at <= 0) return null;
    const arr = JSON.parse(raw);
    if (!arr || !arr.length) return null;
    State.tideStations = arr;
    State.tideStationsFetchedAtMs = at;
    return arr;
  } catch (e) {
    return null;
  }
}

function saveTideStationsToStorage(stations) {
  try {
    localStorage.setItem('noaaTideStations', JSON.stringify(stations));
    localStorage.setItem('noaaTideStationsAtMs', String(Date.now()));
  } catch (e) {}
}

function safeParseUnixFromNoaaGmtTime(t) {
  // NOAA returns e.g. "2026-01-08 09:24" when time_zone=gmt
  const s = String(t || '');
  const m = /^(\d{4})-(\d{2})-(\d{2})\s+(\d{2}):(\d{2})/.exec(s);
  if (!m) return null;
  const iso = m[1] + '-' + m[2] + '-' + m[3] + 'T' + m[4] + ':' + m[5] + ':00Z';
  const ms = Date.parse(iso);
  if (!isFinite(ms)) return null;
  return Math.floor(ms / 1000);
}

function ymdUtcIntFromUnix(unixSec) {
  const d = new Date(unixSec * 1000);
  const y = d.getUTCFullYear();
  const m = d.getUTCMonth() + 1;
  const dd = d.getUTCDate();
  return y * 10000 + m * 100 + dd;
}

function ymdhmUtcFromUnix(unixSec) {
  const d = new Date(unixSec * 1000);
  const y = d.getUTCFullYear();
  const m = d.getUTCMonth() + 1;
  const dd = d.getUTCDate();
  const hh = d.getUTCHours();
  const mm = d.getUTCMinutes();
  return String(y) + pad2(m) + pad2(dd) + ' ' + pad2(hh) + ':' + pad2(mm);
}

function parseNoaaLevelMeters(v) {
  const f = parseFloat(String(v || '').replace(',', '.'));
  if (!isFinite(f)) return null;
  return f;
}

function fetchNoaaLevelX10ForStationGmt(stationId, nowUnix, useImperial) {
  // Fetch ~±1 hour around now, pick the latest prediction <= now.
  const now = typeof nowUnix === 'number' ? nowUnix : Math.floor(Date.now() / 1000);
  const begin = ymdhmUtcFromUnix(now - 3600);
  const end = ymdhmUtcFromUnix(now + 3600);
  const url =
    'https://api.tidesandcurrents.noaa.gov/api/prod/datagetter' +
    '?product=predictions' +
    '&application=pebble-yes-watch' +
    '&begin_date=' + encodeURIComponent(begin) +
    '&end_date=' + encodeURIComponent(end) +
    '&datum=MLLW' +
    '&station=' + encodeURIComponent(String(stationId)) +
    '&time_zone=gmt' +
    '&units=metric' +
    '&interval=6' +
    '&format=json';
  return httpGetJson(url).then((json) => {
    const preds = json && json.predictions ? json.predictions : null;
    if (!preds || !preds.length) throw new Error('noaa level missing');
    let best = null;
    let bestTs = -1;
    for (let i = 0; i < preds.length; i++) {
      const p = preds[i];
      const ts = safeParseUnixFromNoaaGmtTime(p && p.t);
      if (typeof ts !== 'number') continue;
      if (ts > now) continue;
      if (ts >= bestTs) {
        bestTs = ts;
        best = p;
      }
    }
    if (!best) best = preds[0];
    const meters = parseNoaaLevelMeters(best && best.v);
    if (meters === null) throw new Error('noaa level parse');
    if (useImperial) {
      const ft = meters * 3.28084;
      return Math.round(ft * 10); // ft * 10
    }
    return Math.round(meters * 10); // m * 10
  });
}

function fetchNoaaTideStations() {
  // NOTE: NOAA MDAPI doesn't support lat/lon filtering here, so we download once and pick nearest locally.
  const url = 'https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations.json?type=tidepredictions';
  return httpGetJsonTimeout(url, TIDE_STATION_FETCH_TIMEOUT_MS).then((json) => {
    const arr = json && json.stations ? json.stations : null;
    if (!arr || !arr.length) throw new Error('noaa stations missing');
    const out = [];
    for (let i = 0; i < arr.length; i++) {
      const s = arr[i];
      const id = s && (s.id || s.stationId || s.station);
      const lat = s && (typeof s.lat === 'number' ? s.lat : null);
      const lng = s && (typeof s.lng === 'number' ? s.lng : (typeof s.lon === 'number' ? s.lon : null));
      if (!id || typeof lat !== 'number' || typeof lng !== 'number') continue;
      out.push({ id: String(id), lat: lat, lng: lng });
    }
    if (!out.length) throw new Error('noaa stations empty after filter');
    saveTideStationsToStorage(out);
    return out;
  });
}

function getNearestTideStation(latDeg, lonDeg) {
  const stations = State.tideStations;
  if (!stations || !stations.length) return null;
  let best = null;
  let bestD = 1e30;
  for (let i = 0; i < stations.length; i++) {
    const s = stations[i];
    const d = haversineMeters(latDeg, lonDeg, s.lat, s.lng);
    if (d < bestD) {
      bestD = d;
      best = s;
    }
  }
  if (!best) return null;
  return { id: best.id, distM: bestD };
}

function fetchNoaaHiLoForStationGmt(stationId, nowUnix) {
  // Fetch yesterday..tomorrow in GMT so parsing to unix is unambiguous.
  const now = typeof nowUnix === 'number' ? nowUnix : Math.floor(Date.now() / 1000);
  const day = 86400;
  const begin = ymdUtcIntFromUnix(now - day);
  const end = ymdUtcIntFromUnix(now + day);
  const url =
    'https://api.tidesandcurrents.noaa.gov/api/prod/datagetter' +
    '?product=predictions' +
    '&application=pebble-yes-watch' +
    '&begin_date=' + encodeURIComponent(String(begin)) +
    '&end_date=' + encodeURIComponent(String(end)) +
    '&datum=MLLW' +
    '&station=' + encodeURIComponent(String(stationId)) +
    '&time_zone=gmt' +
    '&units=metric' +
    '&interval=hilo' +
    '&format=json';
  return httpGetJson(url).then((json) => {
    const preds = json && json.predictions ? json.predictions : null;
    if (!preds || !preds.length) throw new Error('noaa predictions missing');
    const events = [];
    for (let i = 0; i < preds.length; i++) {
      const p = preds[i];
      const ts = safeParseUnixFromNoaaGmtTime(p && p.t);
      const type = p && (p.type || p.T || p.event || p.hl);
      const isHigh = String(type || '').toUpperCase().indexOf('H') >= 0;
      if (typeof ts !== 'number') continue;
      events.push({ ts, isHigh });
    }
    events.sort((a, b) => a.ts - b.ts);
    return events;
  });
}

function computeTideWindow(events, nowUnix) {
  if (!events || events.length < 2) return null;
  const now = typeof nowUnix === 'number' ? nowUnix : Math.floor(Date.now() / 1000);
  let last = null;
  let next = null;
  for (let i = 0; i < events.length; i++) {
    const e = events[i];
    if (e.ts <= now) last = e;
    if (e.ts > now) { next = e; break; }
  }
  if (!last) last = events[0];
  if (!next) next = events[events.length - 1];
  if (!last || !next) return null;
  if (next.ts <= last.ts) return null;
  return { lastUnix: last.ts, nextUnix: next.ts, nextIsHigh: next.isHigh ? 1 : 0 };
}

function maybeSendTides(latE6, lonE6, force) {
  const nowMs = Date.now();
  const locChanged = !State.lastTideLoc ||
    (haversineMeters(State.lastTideLoc.latE6 / 1e6, State.lastTideLoc.lonE6 / 1e6, latE6 / 1e6, lonE6 / 1e6) > 2000);
  if (locChanged) {
    // New station likely; allow immediate attempt.
    State.lastTideSuccessAtMs = 0;
    force = true;
  }
  State.lastTideLoc = { latE6: latE6, lonE6: lonE6 };

  if (!force && nowMs - State.lastTideAttemptAtMs < 30 * 1000) return; // avoid bursts at startup
  const sinceSuccess = nowMs - (State.lastTideSuccessAtMs || 0);
  const sinceAttempt = nowMs - (State.lastTideAttemptAtMs || 0);
  if (!force) {
    if (State.lastTideSuccessAtMs && sinceSuccess < TIDE_REFRESH_MS) return;
    if (!State.lastTideSuccessAtMs && State.lastTideAttemptAtMs && sinceAttempt < TIDE_RETRY_MS) return;
  }

  const latDeg = latE6 / 1e6;
  const lonDeg = lonE6 / 1e6;

  State.lastTideAttemptAtMs = nowMs;

  const ensureStations = () => {
    if (State.tideStations && State.tideStations.length && (nowMs - State.tideStationsFetchedAtMs) < TIDE_STATION_CACHE_MS) {
      return Promise.resolve(State.tideStations);
    }
    return fetchNoaaTideStations().then((stations) => {
      State.tideStations = stations;
      State.tideStationsFetchedAtMs = Date.now();
      return stations;
    });
  };

  const nowUnix = Math.floor(nowMs / 1000);
  ensureStations().then(() => {
    const nearest = getNearestTideStation(latDeg, lonDeg);
    if (!nearest || nearest.distM > TIDE_NEAR_COAST_THRESHOLD_M) {
      const payload = {};
      payload[KEYS.TIDE_HAVE] = 0;
      sendQueued(payload);
      State.lastTideSentAtMs = Date.now();
      return;
    }
    log('[pkjs] tide nearest station', nearest.id, 'dist_km', Math.round(nearest.distM / 1000));
    const useImperial = isImperialForLocation(latDeg, lonDeg);

    return Promise.all([
      fetchNoaaHiLoForStationGmt(nearest.id, nowUnix),
      fetchNoaaLevelX10ForStationGmt(nearest.id, nowUnix, useImperial)
    ]).then((arr) => {
      const events = arr[0];
      const levelX10 = arr[1];
      const win = computeTideWindow(events, nowUnix);
      const payload = {};
      if (!win) {
        payload[KEYS.TIDE_HAVE] = 0;
      } else {
        payload[KEYS.TIDE_HAVE] = 1;
        payload[KEYS.TIDE_LAST_UNIX] = win.lastUnix;
        payload[KEYS.TIDE_NEXT_UNIX] = win.nextUnix;
        payload[KEYS.TIDE_NEXT_IS_HIGH] = win.nextIsHigh;
        payload[KEYS.TIDE_LEVEL_X10] = levelX10 | 0;
        payload[KEYS.TIDE_LEVEL_IS_FT] = useImperial ? 1 : 0;
      }
      sendQueued(payload);
      State.lastTideSentAtMs = Date.now();
      if (win) State.lastTideSuccessAtMs = Date.now();
    });
  }).catch((e) => {
    log('[pkjs] tide fetch failed', String(e && e.message ? e.message : e));
    const payload = {};
    payload[KEYS.TIDE_HAVE] = 0;
    sendQueued(payload);
    State.lastTideSentAtMs = Date.now();
  });
}

function maybeSendWeather(latE6, lonE6, force) {
  const nowMs = Date.now();
  const locChanged = !State.lastWeatherLoc ||
    (haversineMeters(State.lastWeatherLoc.latE6 / 1e6, State.lastWeatherLoc.lonE6 / 1e6, latE6 / 1e6, lonE6 / 1e6) > 2000);
  State.lastWeatherLoc = { latE6: latE6, lonE6: lonE6 };
  const effectiveForce = !!force || locChanged;
  // Throttle: avoid bursts.
  if (!effectiveForce && (nowMs - (State.lastWeatherAttemptAtMs || 0)) < 30 * 1000) return;

  const sinceSuccess = nowMs - (State.lastWeatherSuccessAtMs || 0);
  const sinceAttempt = nowMs - (State.lastWeatherAttemptAtMs || 0);
  if (!effectiveForce) {
    if (State.lastWeatherSuccessAtMs && sinceSuccess < WEATHER_REFRESH_MS) return;
    if (!State.lastWeatherSuccessAtMs && State.lastWeatherAttemptAtMs && sinceAttempt < WEATHER_RETRY_MS) return;
  }
  State.lastWeatherAttemptAtMs = nowMs;

  const latDeg = latE6 / 1e6;
  const lonDeg = lonE6 / 1e6;
  const useImperial = isImperialForLocation(latDeg, lonDeg);
  const url =
    'https://api.open-meteo.com/v1/forecast' +
    '?latitude=' + encodeURIComponent(String(latDeg)) +
    '&longitude=' + encodeURIComponent(String(lonDeg)) +
    '&current=' + encodeURIComponent('temperature_2m,weather_code,is_day,wind_speed_10m,wind_direction_10m,precipitation,uv_index,pressure_msl') +
    '&temperature_unit=celsius' +
    '&wind_speed_unit=' + encodeURIComponent(useImperial ? 'mph' : 'ms') +
    '&precipitation_unit=' + encodeURIComponent(useImperial ? 'inch' : 'mm') +
    '&timezone=UTC';

  httpGetJson(url).then((json) => {
    const cur = json && json.current ? json.current : null;
    const temp = cur && typeof cur.temperature_2m === 'number' ? cur.temperature_2m : null;
    const code = cur && typeof cur.weather_code === 'number' ? cur.weather_code : null;
    const isDay = cur && (cur.is_day === 1 || cur.is_day === true) ? 1 : 0;
    const windSpd = cur && typeof cur.wind_speed_10m === 'number' ? cur.wind_speed_10m : null;
    const windDir = cur && typeof cur.wind_direction_10m === 'number' ? cur.wind_direction_10m : null;
    const precip = cur && typeof cur.precipitation === 'number' ? cur.precipitation : null;
    const uv = cur && typeof cur.uv_index === 'number' ? cur.uv_index : null;
    const p = cur && typeof cur.pressure_msl === 'number' ? cur.pressure_msl : null;
    if (temp === null || code === null) throw new Error('open-meteo missing current');
    const isF = useImperial;

    const payload = {};
    payload[KEYS.WEATHER_TEMP_C10] = Math.round(temp * 10);
    payload[KEYS.WEATHER_CODE] = Math.max(0, Math.min(255, Math.round(code)));
    payload[KEYS.WEATHER_IS_DAY] = isDay;
    payload[KEYS.WEATHER_IS_F] = isF ? 1 : 0;
    if (windSpd !== null && isFinite(windSpd)) payload[KEYS.WEATHER_WIND_SPD_X10] = Math.round(windSpd * 10);
    if (windDir !== null && isFinite(windDir)) payload[KEYS.WEATHER_WIND_DIR_DEG] = Math.max(0, Math.min(359, Math.round(windDir)));
    if (precip !== null && isFinite(precip)) payload[KEYS.WEATHER_PRECIP_X10] = Math.round(precip * 10);
    if (uv !== null && isFinite(uv)) payload[KEYS.WEATHER_UV_X10] = Math.round(uv * 10);
    if (p !== null && isFinite(p)) payload[KEYS.WEATHER_PRESSURE_HPA_X10] = Math.round(p * 10);
    sendQueued(payload);
    State.lastWeatherSuccessAtMs = Date.now();
    State.lastWeatherSentAtMs = Date.now();
  }).catch((e) => {
    log('[pkjs] weather fetch failed', String(e && e.message ? e.message : e));
  });
}

// --- "Geo-fence" / update thresholding ---
// Trigger recomputation+send only when location/timezone changed enough.
// 25km often changes sunrise/sunset by ~1 minute or less; use a wider threshold so
// that recomputations are more likely to yield different minute values.
const DIST_THRESHOLD_M = 60000; // 60 km
const TZ_THRESHOLD_MIN = 30;    // 30 minutes
// If the move is unlikely to change rise/set by at least this many minutes, skip.
const EXPECTED_SHIFT_MIN = 2;
const LOC_REFRESH_MS = 5 * 60 * 1000; // poll GPS periodically so emulator location changes are picked up

function haversineMeters(lat1, lon1, lat2, lon2) {
  const R = 6371000;
  const dLat = deg2rad(lat2 - lat1);
  const dLon = deg2rad(lon2 - lon1);
  const a =
    Math.sin(dLat / 2) * Math.sin(dLat / 2) +
    Math.cos(deg2rad(lat1)) * Math.cos(deg2rad(lat2)) *
      Math.sin(dLon / 2) * Math.sin(dLon / 2);
  const c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
  return R * c;
}

function inferImperialFromLocale() {
  // Fallback only: infer imperial from locale region (US/LR/MM).
  let lang = '';
  try {
    if (Pebble.getActiveWatchInfo) {
      const wi = Pebble.getActiveWatchInfo();
      if (wi && wi.language) lang = String(wi.language);
    }
  } catch (e) {}
  if (!lang) {
    try { lang = String(navigator.language || ''); } catch (e) {}
  }
  lang = lang.replace('_', '-').toLowerCase();
  let region = '';
  const parts = lang.split('-');
  if (parts.length >= 2) region = parts[1];
  return (region === 'us' || region === 'lr' || region === 'mm');
}

function isImperialForLocation(latDeg, lonDeg) {
  // Primary: derive units from current GPS location (US → imperial).
  if (!isFinite(latDeg) || !isFinite(lonDeg)) return inferImperialFromLocale();

  const inBox = (latMin, latMax, lonMin, lonMax) =>
    (latDeg >= latMin && latDeg <= latMax && lonDeg >= lonMin && lonDeg <= lonMax);

  // Contiguous US
  if (inBox(24.0, 50.0, -125.0, -66.0)) return true;
  // Alaska
  if (inBox(51.0, 72.5, -170.0, -129.0)) return true;
  // Hawaii
  if (inBox(18.5, 22.6, -161.0, -154.0)) return true;
  // Puerto Rico
  if (inBox(17.8, 18.7, -67.4, -65.1)) return true;
  // US Virgin Islands
  if (inBox(17.6, 18.6, -65.3, -64.3)) return true;
  // Guam
  if (inBox(13.1, 13.8, 144.5, 145.1)) return true;
  // Northern Mariana Islands
  if (inBox(14.0, 20.8, 144.8, 146.2)) return true;
  // American Samoa
  if (inBox(-14.6, -10.7, -171.2, -168.0)) return true;

  return false;
}

function maybeSendAltitude(position, latDeg, lonDeg) {
  // Altitude is optional and often noisy. Send only when we have a sane value.
  const coords = position && position.coords ? position.coords : null;
  const alt = coords && typeof coords.altitude === 'number' ? coords.altitude : null;
  const acc = coords && typeof coords.altitudeAccuracy === 'number' ? coords.altitudeAccuracy : null;
  const valid = (alt !== null && isFinite(alt) && (acc === null || !isFinite(acc) || acc <= 250));

  const payload = {};
  payload[KEYS.ALT_VALID] = valid ? 1 : 0;
  payload[KEYS.ALT_IS_FT] = isImperialForLocation(latDeg, lonDeg) ? 1 : 0;
  if (valid) payload[KEYS.ALT_M] = Math.round(alt);
  sendQueued(payload);
}

function expectedShiftMinutes(lat1, lon1, lat2, lon2) {
  // Rough heuristic: longitude dominates local solar-time shift:
  // ~4 minutes per degree longitude. Latitude change affects rise/set too,
  // but less predictably; approximate ~1 minute per degree latitude.
  const dLon = Math.abs(lon2 - lon1);
  const dLat = Math.abs(lat2 - lat1);
  return dLon * 4.0 + dLat * 1.0;
}

function loadLastLocSent() {
  try {
    const raw = localStorage.getItem('lastLocSent');
    if (!raw) return null;
    const obj = JSON.parse(raw);
    if (!obj || typeof obj.latE6 !== 'number' || typeof obj.lonE6 !== 'number' || typeof obj.tzOffsetMin !== 'number') return null;
    return { latE6: obj.latE6 | 0, lonE6: obj.lonE6 | 0, tzOffsetMin: obj.tzOffsetMin | 0, ymd: obj.ymd | 0 };
  } catch (e) {
    return null;
  }
}

function saveLastLocSent(loc) {
  try { localStorage.setItem('lastLocSent', JSON.stringify(loc)); } catch (e) {}
}

function sendQueued(payload, onSuccess) {
  State.msgQueue.push({ payload, onSuccess });
  pumpQueue();
}

function pumpQueue() {
  if (State.isSending) return;
  const item = State.msgQueue.shift();
  if (!item) return;
  State.isSending = true;

  Pebble.sendAppMessage(
    item.payload,
    () => {
      State.isSending = false;
      try { if (typeof item.onSuccess === 'function') item.onSuccess(); } catch (e) {}
      setTimeout(pumpQueue, 0);
    },
    () => {
      // Drop on failure; don't spin.
      State.isSending = false;
      setTimeout(pumpQueue, 200);
    }
  );
}

// --- Astro helpers (computed on phone to keep watch responsive) ---
const SUN_STATE = { NORMAL: 0, ALWAYS_DAY: 1, ALWAYS_NIGHT: 2, INVALID: 3 };
const MOON_STATE = { NORMAL: 0, ALWAYS_UP: 1, ALWAYS_DOWN: 2, INVALID: 3 };

function deg2rad(deg) { return deg * Math.PI / 180; }
function rad2deg(rad) { return rad * 180 / Math.PI; }
function normDeg(deg) {
  let d = deg % 360;
  if (d < 0) d += 360;
  return d;
}
function normHours(h) {
  let x = h % 24;
  if (x < 0) x += 24;
  return x;
}

function dayOfYearUTC(year, month1, day) {
  const start = Date.UTC(year, 0, 1);
  const now = Date.UTC(year, month1 - 1, day);
  return Math.floor((now - start) / 86400000) + 1;
}

function ymdForOffsetMinutes(offsetMin) {
  // Treat "UTC+offset" as local time by shifting and then reading UTC fields.
  const d = new Date(Date.now() + offsetMin * 60000);
  return { y: d.getUTCFullYear(), m: d.getUTCMonth() + 1, d: d.getUTCDate() };
}

function calcSolarEventLocalHours(N, latDeg, lonDeg, tzHours, isSunrise) {
  const lngHour = lonDeg / 15.0;
  const zenith = 90.833;

  const t = N + ((isSunrise ? 6 : 18) - lngHour) / 24.0;
  const M = (0.9856 * t) - 3.289;
  let L = M + (1.916 * Math.sin(deg2rad(M))) + (0.020 * Math.sin(deg2rad(2 * M))) + 282.634;
  L = normDeg(L);

  let RA = rad2deg(Math.atan(0.91764 * Math.tan(deg2rad(L))));
  RA = normDeg(RA);
  const Lq = Math.floor(L / 90) * 90;
  const RAq = Math.floor(RA / 90) * 90;
  RA = (RA + (Lq - RAq)) / 15.0;

  const sinDec = 0.39782 * Math.sin(deg2rad(L));
  const cosDec = Math.cos(Math.asin(sinDec));
  const cosH = (Math.cos(deg2rad(zenith)) - (sinDec * Math.sin(deg2rad(latDeg)))) /
               (cosDec * Math.cos(deg2rad(latDeg)));

  if (cosH > 1) return { polarNight: true };
  if (cosH < -1) return { polarDay: true };

  let H = isSunrise ? (360 - rad2deg(Math.acos(cosH))) : rad2deg(Math.acos(cosH));
  H = H / 15.0;
  const T = H + RA - (0.06571 * t) - 6.622;
  let UT = T - lngHour;
  UT = normHours(UT);
  const localT = normHours(UT + tzHours);
  return { hours: localT };
}

function calcSunriseSunsetMinutes(latDeg, lonDeg, tzOffsetMin) {
  const tzHours = tzOffsetMin / 60.0;
  const ymd = ymdForOffsetMinutes(tzOffsetMin);
  const N = dayOfYearUTC(ymd.y, ymd.m, ymd.d);

  const rise = calcSolarEventLocalHours(N, latDeg, lonDeg, tzHours, true);
  const set = calcSolarEventLocalHours(N, latDeg, lonDeg, tzHours, false);

  if (rise.polarDay || set.polarDay) {
    return { state: SUN_STATE.ALWAYS_DAY, sunriseMin: 0, sunsetMin: 0 };
  }
  if (rise.polarNight || set.polarNight) {
    return { state: SUN_STATE.ALWAYS_NIGHT, sunriseMin: 0, sunsetMin: 0 };
  }
  if (typeof rise.hours !== 'number' || typeof set.hours !== 'number') {
    return { state: SUN_STATE.INVALID, sunriseMin: 0, sunsetMin: 0 };
  }
  return {
    state: SUN_STATE.NORMAL,
    sunriseMin: (Math.round(rise.hours * 60) % 1440 + 1440) % 1440,
    sunsetMin: (Math.round(set.hours * 60) % 1440 + 1440) % 1440
  };
}

function julianDayFromUnix(unixSec) {
  return 2440587.5 + unixSec / 86400.0;
}

function gmstHoursFromUnix(unixSec) {
  const jd = julianDayFromUnix(unixSec);
  const D = jd - 2451545.0;
  return normHours(18.697374558 + 24.06570982441908 * D);
}

function sunEclipticLonDeg(unixSec) {
  // Approximate apparent ecliptic longitude of the Sun (good enough for Moon phase).
  // NOAA-style formula.
  const jd = julianDayFromUnix(unixSec);
  const n = jd - 2451545.0;
  const L = normDeg(280.460 + 0.9856474 * n);
  const g = normDeg(357.528 + 0.9856003 * n);
  const lambda = normDeg(L + 1.915 * Math.sin(deg2rad(g)) + 0.020 * Math.sin(deg2rad(2 * g)));
  return lambda;
}

// More accurate Moon position: Meeus-style periodic terms (lon/lat/dist).
// This dramatically improves moonrise/moonset timing vs the earlier low-precision model.
function moonRaDec(unixSec) {
  const jd = julianDayFromUnix(unixSec);
  const T = (jd - 2451545.0) / 36525.0;

  const Lp = normDeg(218.3164477 + 481267.88123421 * T - 0.0015786 * T * T + (T * T * T) / 538841.0 - (T * T * T * T) / 65194000.0);
  const D  = normDeg(297.8501921 + 445267.1114034  * T - 0.0018819 * T * T + (T * T * T) / 545868.0  - (T * T * T * T) / 113065000.0);
  const M  = normDeg(357.5291092 + 35999.0502909   * T - 0.0001536 * T * T + (T * T * T) / 24490000.0);
  const Mp = normDeg(134.9633964 + 477198.8675055  * T + 0.0087414 * T * T + (T * T * T) / 69699.0   - (T * T * T * T) / 14712000.0);
  const F  = normDeg(93.2720950  + 483202.0175233  * T - 0.0036539 * T * T - (T * T * T) / 3526000.0 + (T * T * T * T) / 863310000.0);

  // E factor for terms containing M (Sun mean anomaly)
  const E = 1.0 - 0.002516 * T - 0.0000074 * T * T;

  // Periodic terms (truncated but still strong improvement).
  // Format: [d, m, mp, f, lon(1e-6 deg), dist(1e-3 km)]
  // Coeffs derived from Meeus Table 45.A (largest terms).
  const TERMS_LR = [
    [ 0,  0,  1,  0,  6288774, -20905355],
    [ 2,  0, -1,  0,  1274027,  -3699111],
    [ 2,  0,  0,  0,   658314,  -2955968],
    [ 0,  0,  2,  0,   213618,   -569925],
    [ 0,  1,  0,  0,  -185116,     48888],
    [ 0,  0,  0,  2,  -114332,     -3149],
    [ 2,  0, -2,  0,    58793,    246158],
    [ 2, -1, -1,  0,    57066,   -152138],
    [ 2,  0,  1,  0,    53322,   -170733],
    [ 2, -1,  0,  0,    45758,   -204586],
    [ 0,  1, -1,  0,   -40923,   -129620],
    [ 1,  0,  0,  0,   -34720,    108743],
    [ 0,  1,  1,  0,   -30383,    104755],
    [ 2,  0,  0, -2,    15327,     10321],
    [ 0,  0,  1,  2,   -12528,         0],
    [ 0,  0,  1, -2,    10980,     79661],
    [ 4,  0, -1,  0,    10675,    -34782],
    [ 0,  0,  3,  0,    10034,    -23210],
    [ 4,  0, -2,  0,     8548,    -21636],
    [ 2,  1, -1,  0,    -7888,     24208],
    [ 2,  1,  0,  0,    -6766,     30824],
    [ 1,  0, -1,  0,    -5163,     -8379],
    [ 1,  1,  0,  0,     4987,    -16675],
    [ 2, -1,  1,  0,     4036,    -12831],
    [ 2,  0,  2,  0,     3994,    -10445],
    [ 4,  0,  0,  0,     3861,    -11650],
    [ 2,  0, -3,  0,     3665,     14403],
    [ 0,  1, -2,  0,    -2689,     -7003],
    [ 2,  0, -1,  2,    -2602,         0],
    [ 2, -1, -2,  0,     2390,     10056],
    [ 1,  0,  1,  0,    -2348,      6322],
    [ 2, -2,  0,  0,     2236,     -9884],
    [ 0,  1,  2,  0,    -2120,      5751],
    [ 0,  2,  0,  0,    -2069,         0],
    [ 2, -2, -1,  0,     2048,     -4950],
    [ 2,  0,  1, -2,    -1773,      4130],
    [ 2,  0,  0,  2,    -1595,         0],
    [ 4, -1, -1,  0,     1215,     -3958],
    [ 0,  0,  2,  2,    -1110,         0],
    [ 3,  0, -1,  0,     -892,      3258],
    [ 2,  1,  1,  0,     -810,      2616],
    [ 4, -1, -2,  0,      759,     -1897],
    [ 0,  2, -1,  0,     -713,     -2117],
    [ 2,  2, -1,  0,     -700,      2354],
    [ 2,  1, -2,  0,      691,         0],
    [ 2, -1,  0, -2,      596,         0],
    [ 4,  0,  1,  0,      549,     -1423],
    [ 0,  0,  4,  0,      537,     -1117]
  ];

  // Format: [d, m, mp, f, lat(1e-6 deg)]
  // Coeffs derived from Meeus Table 45.B (largest terms).
  const TERMS_B = [
    [ 0,  0,  0,  1,  5128122],
    [ 0,  0,  1,  1,   280602],
    [ 0,  0,  1, -1,   277693],
    [ 2,  0,  0, -1,   173237],
    [ 2,  0, -1,  1,    55413],
    [ 2,  0, -1, -1,    46271],
    [ 2,  0,  0,  1,    32573],
    [ 0,  0,  2,  1,    17198],
    [ 2,  0,  1, -1,     9266],
    [ 0,  0,  2, -1,     8822],
    [ 2, -1,  0, -1,     8216],
    [ 2,  0, -2, -1,     4324],
    [ 2,  0,  1,  1,     4200],
    [ 2,  1,  0, -1,    -3359],
    [ 2, -1, -1,  1,     2463],
    [ 2, -1,  0,  1,     2211],
    [ 2, -1, -1, -1,     2065],
    [ 0,  1, -1, -1,    -1870],
    [ 4,  0, -1, -1,     1828],
    [ 0,  1,  0,  1,    -1794],
    [ 0,  0,  0,  3,    -1749],
    [ 0,  1, -1,  1,    -1565],
    [ 1,  0,  0,  1,    -1491],
    [ 0,  1,  1,  1,    -1475],
    [ 0,  1,  1, -1,    -1410],
    [ 0,  1,  0, -1,    -1344],
    [ 1,  0,  0, -1,    -1335],
    [ 0,  0,  3,  1,     1107],
    [ 4,  0,  0, -1,     1021],
    [ 4,  0, -1,  1,      833],
    [ 0,  0,  1, -3,      777],
    [ 4,  0, -2,  1,      671],
    [ 2,  0,  0, -3,      607],
    [ 2,  0,  2, -1,      596],
    [ 2, -1,  1, -1,      491],
    [ 2,  0, -2,  1,     -451],
    [ 0,  0,  3, -1,      439],
    [ 2,  0,  2,  1,      422],
    [ 2,  0, -3, -1,      421],
    [ 2,  1, -1,  1,     -366],
    [ 2,  1,  0,  1,     -351],
    [ 4,  0,  0,  1,      331],
    [ 2, -1,  1,  1,      315],
    [ 2, -2,  0, -1,      302],
    [ 0,  0,  1,  3,     -283]
  ];

  let sumL = 0;
  let sumR = 0;
  for (let i = 0; i < TERMS_LR.length; i++) {
    const t = TERMS_LR[i];
    const d = t[0], m = t[1], mp = t[2], f = t[3];
    const arg = deg2rad(d * D + m * M + mp * Mp + f * F);
    const eFac = (Math.abs(m) === 1) ? E : ((Math.abs(m) === 2) ? (E * E) : 1.0);
    sumL += eFac * t[4] * Math.sin(arg);
    sumR += eFac * t[5] * Math.cos(arg);
  }

  let sumB = 0;
  for (let i = 0; i < TERMS_B.length; i++) {
    const t = TERMS_B[i];
    const d = t[0], m = t[1], mp = t[2], f = t[3];
    const arg = deg2rad(d * D + m * M + mp * Mp + f * F);
    const eFac = (Math.abs(m) === 1) ? E : ((Math.abs(m) === 2) ? (E * E) : 1.0);
    sumB += eFac * t[4] * Math.sin(arg);
  }

  const lon = normDeg(Lp + sumL / 1e6);
  const lat = sumB / 1e6;
  const distKm = 385000.56 + sumR / 1000.0;
  const rEarth = distKm / 6378.137; // Earth radii

  const eps = deg2rad(23.439291 - 0.0130042 * T); // mean obliquity (good enough here)
  const lonRad = deg2rad(lon);
  const latRad = deg2rad(lat);

  const x = Math.cos(lonRad) * Math.cos(latRad);
  const y = Math.sin(lonRad) * Math.cos(latRad);
  const z = Math.sin(latRad);

  const xeq = x;
  const yeq = y * Math.cos(eps) - z * Math.sin(eps);
  const zeq = y * Math.sin(eps) + z * Math.cos(eps);

  let ra = Math.atan2(yeq, xeq);
  if (ra < 0) ra += 2 * Math.PI;
  const dec = Math.atan2(zeq, Math.sqrt(xeq * xeq + yeq * yeq));
  return { ra: ra, dec: dec, r: rEarth, lonDeg: lon, latDeg: lat, distKm: distKm };
}

function moonPhase0to1(unixSec) {
  // 0=new, 0.5=full, 1=next new.
  // Use ecliptic longitude elongation (Moon - Sun).
  const m = moonRaDec(unixSec);
  const lonMoon = (typeof m.lonDeg === 'number') ? m.lonDeg : 0;
  const lonSun = sunEclipticLonDeg(unixSec);
  const elong = normDeg(lonMoon - lonSun); // 0..360
  return elong / 360.0;
}

function moonAltitudeRad(unixSec, latDeg, lonDeg) {
  // Topocentric altitude with parallax correction (major improvement for rise/set timing).
  // References: Meeus, Astronomical Algorithms (topocentric RA/Dec).
  const m = moonRaDec(unixSec);
  const ra = m.ra;
  const dec = m.dec;
  const r = m.r || 60.0; // Earth radii

  const lat = deg2rad(latDeg);
  const lst = deg2rad(gmstHoursFromUnix(unixSec) * 15.0 + lonDeg);
  let ha = lst - ra;
  while (ha < -Math.PI) ha += 2 * Math.PI;
  while (ha > Math.PI) ha -= 2 * Math.PI;

  // Geocentric to topocentric factors (ignore height)
  const u = Math.atan(0.99664719 * Math.tan(lat));
  const rhoSinPhi = 0.99664719 * Math.sin(u);
  const rhoCosPhi = Math.cos(u);

  const sinPi = 1.0 / r; // sin(horizontal parallax) ~= 1/r (r in Earth radii)
  const cosDec = Math.cos(dec);
  const sinDec = Math.sin(dec);
  const sinH = Math.sin(ha);
  const cosH = Math.cos(ha);

  const denom = cosDec - rhoCosPhi * sinPi * cosH;
  const dRa = Math.atan2(-rhoCosPhi * sinPi * sinH, denom);
  const raTopo = ra + dRa;
  const decTopo = Math.atan2((sinDec - rhoSinPhi * sinPi) * Math.cos(dRa), denom);

  let haTopo = lst - raTopo;
  while (haTopo < -Math.PI) haTopo += 2 * Math.PI;
  while (haTopo > Math.PI) haTopo -= 2 * Math.PI;

  // Altitude
  const alt = Math.asin(Math.sin(lat) * Math.sin(decTopo) + Math.cos(lat) * Math.cos(decTopo) * Math.cos(haTopo));
  return alt;
}

function calcMoonriseMoonsetMinutes(latDeg, lonDeg, tzOffsetMin) {
  const ymd = ymdForOffsetMinutes(tzOffsetMin);
  const baseUtcMidnight = Date.UTC(ymd.y, ymd.m - 1, ymd.d) / 1000 - tzOffsetMin * 60;
  // Rise/set is convention-based (refraction + apparent radius). Using a small negative
  // altitude threshold makes offline results align much closer to MET Norway.
  const horizon = deg2rad(-0.3);
  // Phone-side calculation can afford finer steps; coarse steps caused >1h errors.
  const stepMin = 5;

  let rise = null;
  let set = null;
  let aboveCount = 0;
  let prevAbove = null;

  for (let m = 0; m <= 1440; m += stepMin) {
    const mm = (m === 1440) ? 1439 : m;
    const t = baseUtcMidnight + mm * 60;
    const alt = moonAltitudeRad(t, latDeg, lonDeg);
    const above = alt > horizon;
    if (above) aboveCount++;
    if (prevAbove === null) {
      prevAbove = above;
      continue;
    }
    if (above !== prevAbove) {
      let lo = m - stepMin;
      let hi = m;
      // Binary search to ~< 1 minute resolution.
      for (let i = 0; i < 12; i++) {
        const mid = Math.floor((lo + hi) / 2);
        const tm = baseUtcMidnight + mid * 60;
        const am = moonAltitudeRad(tm, latDeg, lonDeg);
        const ab = am > horizon;
        if (ab === prevAbove) lo = mid; else hi = mid;
      }
      if (!prevAbove && above && rise === null) rise = hi;
      if (prevAbove && !above && set === null) set = hi;
    }
    prevAbove = above;
  }

  if (rise === null && set === null) {
    const samples = Math.floor(1440 / stepMin);
    if (aboveCount > samples / 2) {
      return { state: MOON_STATE.ALWAYS_UP, moonriseMin: 0, moonsetMin: 0 };
    }
    return { state: MOON_STATE.ALWAYS_DOWN, moonriseMin: 0, moonsetMin: 0 };
  }

  return {
    state: MOON_STATE.NORMAL,
    moonriseMin: ((rise || 0) % 1440 + 1440) % 1440,
    moonsetMin: ((set || 0) % 1440 + 1440) % 1440
  };
}

function pad2(n) { return (n < 10 ? '0' : '') + String(n); }

function tzOffsetToIso(offsetMin) {
  const sign = offsetMin < 0 ? '-' : '+';
  const a = Math.abs(offsetMin);
  const hh = Math.floor(a / 60);
  const mm = a % 60;
  return sign + pad2(hh) + ':' + pad2(mm);
}

function ymdToIso(ymd) {
  return String(ymd.y) + '-' + pad2(ymd.m) + '-' + pad2(ymd.d);
}

function timeStringToMinutesLocal(timeStr) {
  // Expect ISO-ish: YYYY-MM-DDTHH:MM... (may include seconds + offset)
  const m = /T(\d{2}):(\d{2})/.exec(String(timeStr || ''));
  if (!m) return null;
  const hh = parseInt(m[1], 10);
  const mm = parseInt(m[2], 10);
  if (!isFinite(hh) || !isFinite(mm)) return null;
  return (hh * 60 + mm) % 1440;
}

function httpGetJson(url, headers) {
  return new Promise((resolve, reject) => {
    try {
      const req = new XMLHttpRequest();
      req.open('GET', url, true);
      try {
        const h = headers || {};
        Object.keys(h).forEach((k) => {
          try { req.setRequestHeader(k, h[k]); } catch (e) {}
        });
      } catch (e) {}
      req.onload = function() {
        try {
          if (req.status < 200 || req.status >= 300) {
            reject(new Error('http ' + req.status));
            return;
          }
          const json = JSON.parse(req.responseText);
          resolve(json);
        } catch (e) {
          reject(e);
        }
      };
      req.onerror = function() { reject(new Error('network error')); };
      req.ontimeout = function() { reject(new Error('timeout')); };
      req.timeout = 15000;
      req.send(null);
    } catch (e) {
      reject(e);
    }
  });
}

function httpGetJsonTimeout(url, timeoutMs, headers) {
  return new Promise((resolve, reject) => {
    try {
      const req = new XMLHttpRequest();
      req.open('GET', url, true);
      try {
        const h = headers || {};
        Object.keys(h).forEach((k) => {
          try { req.setRequestHeader(k, h[k]); } catch (e) {}
        });
      } catch (e) {}
      req.onload = function() {
        try {
          if (req.status < 200 || req.status >= 300) {
            reject(new Error('http ' + req.status));
            return;
          }
          const json = JSON.parse(req.responseText);
          resolve(json);
        } catch (e) {
          reject(e);
        }
      };
      req.onerror = function() { reject(new Error('network error')); };
      req.ontimeout = function() { reject(new Error('timeout')); };
      req.timeout = Math.max(1000, (timeoutMs | 0));
      req.send(null);
    } catch (e) {
      reject(e);
    }
  });
}

function fetchMetNoProperties(kind, latDeg, lonDeg, tzOffsetMin) {
  const ymd = ymdForOffsetMinutes(tzOffsetMin);
  const url =
    'https://api.met.no/weatherapi/sunrise/3.0/' + kind +
    '?lat=' + encodeURIComponent(String(latDeg)) +
    '&lon=' + encodeURIComponent(String(lonDeg)) +
    '&date=' + encodeURIComponent(ymdToIso(ymd)) +
    '&offset=' + encodeURIComponent(tzOffsetToIso(tzOffsetMin));

  log('[pkjs] met.no url', url);
  return httpGetJson(url, {
    // MET Norway requests an identifying UA; some environments forbid setting it, so ignore failures.
    'User-Agent': 'pebble-yes-watch/1.0 (pebble pkjs)'
  }).then((json) => {
    if (!json || !json.properties) throw new Error('met.no bad json');
    return json.properties;
  });
}

function calcSunMoonMinutesInternet(latDeg, lonDeg, tzOffsetMin) {
  return Promise.all([
    fetchMetNoProperties('sun', latDeg, lonDeg, tzOffsetMin),
    fetchMetNoProperties('moon', latDeg, lonDeg, tzOffsetMin)
  ]).then((arr) => {
    const sunP = arr[0];
    const moonP = arr[1];

    const sunriseMin = sunP.sunrise && sunP.sunrise.time ? timeStringToMinutesLocal(sunP.sunrise.time) : null;
    const sunsetMin = sunP.sunset && sunP.sunset.time ? timeStringToMinutesLocal(sunP.sunset.time) : null;
    const moonriseMin = moonP.moonrise && moonP.moonrise.time ? timeStringToMinutesLocal(moonP.moonrise.time) : null;
    const moonsetMin = moonP.moonset && moonP.moonset.time ? timeStringToMinutesLocal(moonP.moonset.time) : null;

    if (sunriseMin === null || sunsetMin === null) throw new Error('met.no missing sun times');

    const sun = { state: SUN_STATE.NORMAL, sunriseMin: sunriseMin, sunsetMin: sunsetMin };
    let moon;
    if (moonriseMin === null && moonsetMin === null) {
      moon = calcMoonriseMoonsetMinutes(latDeg, lonDeg, tzOffsetMin);
    } else {
      moon = { state: MOON_STATE.NORMAL, moonriseMin: (moonriseMin || 0), moonsetMin: (moonsetMin || 0) };
    }
    log('[pkjs] met.no parsed',
      'SR', sunriseMin, 'SS', sunsetMin,
      'MR', moonriseMin, 'MS', moonsetMin);
    return { sun: sun, moon: moon };
  });
}

function ymdIntForOffsetMinutes(offsetMin) {
  const ymd = ymdForOffsetMinutes(offsetMin);
  return ymd.y * 10000 + ymd.m * 100 + ymd.d;
}

function shouldSendUpdate(latE6, lonE6, tzOffsetMin, force) {
  if (force) return true;
  const prev = State.lastLocSent || State.lastLoc;
  if (!prev) return true;
  const tzDelta = Math.abs((tzOffsetMin | 0) - (prev.tzOffsetMin | 0));
  if (tzDelta >= TZ_THRESHOLD_MIN) return true;
  const prevLat = prev.latE6 / 1e6;
  const prevLon = prev.lonE6 / 1e6;
  const curLat = latE6 / 1e6;
  const curLon = lonE6 / 1e6;
  const d = haversineMeters(prevLat, prevLon, curLat, curLon);
  if (d >= DIST_THRESHOLD_M) return true;
  const est = expectedShiftMinutes(prevLat, prevLon, curLat, curLon);
  return est >= EXPECTED_SHIFT_MIN;
}

function maybeSendAstroForDayRollover() {
  if (!State.lastLoc) return;
  const ymdInt = ymdIntForOffsetMinutes(State.lastLoc.tzOffsetMin);
  if (ymdInt !== State.lastAstroYmd) {
    State.lastAstroYmd = ymdInt;
    log('[pkjs] Daily rollover -> recompute astro');
    sendAstroForCurrentLocation(State.lastLoc.latE6, State.lastLoc.lonE6, State.lastLoc.tzOffsetMin);
  }
}

function readUseInternetFromStorage() {
  try {
    const v = localStorage.getItem('useInternet');
    if (v === null || typeof v === 'undefined') return true; // default: prefer Internet
    return v === '1';
  } catch (e) {
    return true; // default: prefer Internet
  }
}

function sendUseInternetFlag() {
  const useInternet = readUseInternetFromStorage();
  const payload = {};
  payload[KEYS.USE_INTERNET_FALLBACK] = useInternet ? 1 : 0;
  sendQueued(payload);
}

function sendAstroForCurrentLocation(latE6, lonE6, tzOffsetMin) {
  const latDeg = latE6 / 1e6;
  const lonDeg = lonE6 / 1e6;
  const useInternet = readUseInternetFromStorage();

  const sendPayload = (sun, moon) => {
    const payload = {};
    payload[KEYS.HOME_SUN_STATE] = sun.state;
    payload[KEYS.HOME_SUNRISE_MIN] = sun.sunriseMin;
    payload[KEYS.HOME_SUNSET_MIN] = sun.sunsetMin;
    payload[KEYS.HOME_MOON_STATE] = moon.state;
    payload[KEYS.HOME_MOONRISE_MIN] = moon.moonriseMin;
    payload[KEYS.HOME_MOONSET_MIN] = moon.moonsetMin;
    // Moon phase "now" (UTC), scaled to 1e6: 0=new, 500000=full.
    const phase = moonPhase0to1(Math.floor(Date.now() / 1000));
    payload[KEYS.MOON_PHASE_E6] = Math.max(0, Math.min(1000000, Math.round(phase * 1e6)));
    payload[KEYS.USE_INTERNET_FALLBACK] = useInternet ? 1 : 0;
    sendQueued(payload);
  };

  if (useInternet) {
    calcSunMoonMinutesInternet(latDeg, lonDeg, tzOffsetMin).then((res) => {
      sendPayload(res.sun, res.moon);
    }).catch(() => {
      log('[pkjs] met.no failed; falling back to local astro');
      sendPayload(
        calcSunriseSunsetMinutes(latDeg, lonDeg, tzOffsetMin),
        calcMoonriseMoonsetMinutes(latDeg, lonDeg, tzOffsetMin)
      );
    });
  } else {
    sendPayload(
      calcSunriseSunsetMinutes(latDeg, lonDeg, tzOffsetMin),
      calcMoonriseMoonsetMinutes(latDeg, lonDeg, tzOffsetMin)
    );
  }
}

function sendLocation(position, force) {
  // If user configured a Home override (useful for emulator/VPN), prefer it.
  if (sendHomeOverride()) return;

  const coords = position.coords;
  if (!coords || typeof coords.latitude !== 'number' || typeof coords.longitude !== 'number' ||
      !isFinite(coords.latitude) || !isFinite(coords.longitude)) {
    // Don't send bogus 0/NaN coords (breaks astro calculations).
    log('[pkjs] Invalid coords', coords);
    return;
  }

  const latE6 = Math.round(coords.latitude * 1e6);
  const lonE6 = Math.round(coords.longitude * 1e6);
  // JS Date.getTimezoneOffset(): minutes behind UTC (e.g. PST=480). Convert to "UTC offset minutes" (PST=-480).
  // NOTE: In the phone simulator, the OS timezone can differ from the simulated GPS coordinates.
  // When that happens, sunrise/sunset will be wildly wrong. Detect obvious mismatches and fall back
  // to a longitude-based rough timezone offset.
  let tzOffsetMin = -new Date().getTimezoneOffset();
  const tzGuessMin = Math.round(coords.longitude / 15) * 60; // rough; ignores political boundaries/DST
  const mismatch =
    (tzOffsetMin === 0 && Math.abs(tzGuessMin) >= 60) ||
    (tzGuessMin === 0 && Math.abs(tzOffsetMin) >= 60) ||
    (Math.sign(tzOffsetMin) !== Math.sign(tzGuessMin) && Math.abs(tzOffsetMin - tzGuessMin) >= 120) ||
    (Math.abs(tzOffsetMin - tzGuessMin) >= 360); // >= 6 hours off is definitely wrong
  if (mismatch) {
    log('[pkjs] TZ mismatch; using lon-based tz', { tzOffsetMin: tzOffsetMin, tzGuessMin: tzGuessMin, lat: coords.latitude, lon: coords.longitude });
    tzOffsetMin = tzGuessMin;
  }
  const locUnix = Math.floor(Date.now() / 1000);

  // Weather/tide should track location even when we geo-fence astro sends.
  // If the user changes emulator location, we want this to update quickly.
  maybeSendAltitude(position, coords.latitude, coords.longitude);
  maybeSendTides(latE6, lonE6, !!force);
  maybeSendWeather(latE6, lonE6, !!force);

  const payload = {};
  payload[KEYS.LAT_E6] = latE6;
  payload[KEYS.LON_E6] = lonE6;
  payload[KEYS.TZ_OFFSET_MIN] = tzOffsetMin;
  payload[KEYS.LOC_UNIX] = locUnix;
  payload[KEYS.USE_INTERNET_FALLBACK] = readUseInternetFromStorage() ? 1 : 0;

  State.lastLoc = { latE6: latE6, lonE6: lonE6, tzOffsetMin: tzOffsetMin };

  const doSend = shouldSendUpdate(latE6, lonE6, tzOffsetMin, !!force);
  if (!doSend) {
    log('[pkjs] Geo-fence: skip send');
    maybeSendAstroForDayRollover();
    return;
  }

  sendQueued(payload, () => {
    State.lastSentAtMs = Date.now();
    const ymdInt = ymdIntForOffsetMinutes(tzOffsetMin);
    State.lastLocSent = { latE6: latE6, lonE6: lonE6, tzOffsetMin: tzOffsetMin, ymd: ymdInt };
    saveLastLocSent(State.lastLocSent);
    State.lastAstroYmd = ymdInt;
    log('[pkjs] Geo-fence: sent update');
    // Defer astro send slightly to avoid back-to-back messages at startup.
    setTimeout(() => {
      sendAstroForCurrentLocation(latE6, lonE6, tzOffsetMin);
      maybeSendTides(latE6, lonE6, true);
      maybeSendWeather(latE6, lonE6, true);
    }, 200);
  });
}

function requestLocation(force) {
  if (sendHomeOverride()) return;
  if (!navigator.geolocation) {
    return;
  }

  State.lastLocRequestAtMs = Date.now();
  navigator.geolocation.getCurrentPosition(
    (pos) => sendLocation(pos, !!force),
    () => { /* ignore */ },
    {
      enableHighAccuracy: false,
      timeout: 15000,
      // If this is a forced refresh (button press / explicit request), don't accept cached coords.
      maximumAge: force ? 0 : 10 * 60 * 1000
    }
  );
}

function maybeRefreshLocation() {
  // Refresh periodically so emulator/location changes are picked up.
  const now = Date.now();
  if (now - (State.lastLocRequestAtMs || 0) > LOC_REFRESH_MS) requestLocation(false);
}

function readHomeOverrideFromStorage() {
  try {
    const raw = localStorage.getItem('home');
    if (!raw) return null;
    const obj = JSON.parse(raw);
    if (!obj || !obj.valid) return { valid: false };
    if (typeof obj.latE6 !== 'number' || typeof obj.lonE6 !== 'number' || typeof obj.tzOffsetMin !== 'number') {
      return { valid: false };
    }
    return {
      valid: true,
      latE6: (obj.latE6 | 0),
      lonE6: (obj.lonE6 | 0),
      tzOffsetMin: (obj.tzOffsetMin | 0)
    };
  } catch (e) {
    return null;
  }
}

function sendHomeOverride() {
  const home = readHomeOverrideFromStorage();
  if (!home || !home.valid) return false;

  const payload = {};
  payload[KEYS.LAT_E6] = home.latE6;
  payload[KEYS.LON_E6] = home.lonE6;
  payload[KEYS.TZ_OFFSET_MIN] = home.tzOffsetMin;
  payload[KEYS.LOC_UNIX] = Math.floor(Date.now() / 1000);
  payload[KEYS.USE_INTERNET_FALLBACK] = readUseInternetFromStorage() ? 1 : 0;

  sendQueued(payload, () => {
    State.lastSentAtMs = Date.now();
    State.lastLoc = { latE6: home.latE6, lonE6: home.lonE6, tzOffsetMin: home.tzOffsetMin };
    const ymdInt = ymdIntForOffsetMinutes(home.tzOffsetMin);
    State.lastLocSent = { latE6: home.latE6, lonE6: home.lonE6, tzOffsetMin: home.tzOffsetMin, ymd: ymdInt };
    saveLastLocSent(State.lastLocSent);
    State.lastAstroYmd = ymdInt;
    // Defer astro send slightly to avoid back-to-back messages at startup.
    setTimeout(() => {
      sendAstroForCurrentLocation(home.latE6, home.lonE6, home.tzOffsetMin);
      maybeSendTides(home.latE6, home.lonE6, true);
      maybeSendWeather(home.latE6, home.lonE6, true);
    }, 200);
  });
  return true;
}

function getUiModelFromStorage() {
  // Read stored values in pkjs context (localStorage is available here).
  const homeStored = readHomeOverrideFromStorage();
  const useInternet = readUseInternetFromStorage();

  const homeUi = homeStored && homeStored.valid ? {
    valid: true,
    lat: homeStored.latE6 / 1e6,
    lon: homeStored.lonE6 / 1e6,
    tzOffsetMin: homeStored.tzOffsetMin
  } : { valid: false };

  return { homeUi, useInternet };
}

function configHtml(homeUi, useInternet) {
  // Inline config page.
  // IMPORTANT: localStorage is disabled for `data:` URLs in many browsers, so do NOT access it here.
  // We inject initial values from pkjs instead and return the user's changes via return_to/pebblejs://close.
  return `
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>YES Watchface</title>
  <style>
    body { font-family: -apple-system, system-ui, sans-serif; margin: 16px; }
    .row { margin: 12px 0; }
    label { display: block; font-weight: 600; margin-bottom: 6px; }
    input { width: 100%; padding: 10px; font-size: 16px; box-sizing: border-box; }
    button { width: 100%; padding: 12px; font-size: 16px; margin-top: 10px; }
    .hint { font-size: 12px; color: #555; }
  </style>
</head>
<body>
  <h2>Home override (optional)</h2>
  <div class="row">
    <label><input type="checkbox" id="homeEnabled"> Override Home location</label>
    <div class="hint">Useful in the emulator/VPN. If disabled, Home uses your phone's current location.</div>
  </div>
  <div class="row">
    <label>Home Latitude (decimal degrees)</label>
    <input id="homeLat" placeholder="e.g. 26.176807">
  </div>
  <div class="row">
    <label>Home Longitude (decimal degrees)</label>
    <input id="homeLon" placeholder="e.g. -80.171041">
  </div>
  <div class="row">
    <label>Home UTC offset minutes</label>
    <input id="homeTz" placeholder="e.g. -300 (EST), -240 (EDT)">
  </div>
  <button id="homeUseGps">Use my current GPS as Home</button>

  <div class="row">
    <label><input type="checkbox" id="useInternet"> Use Internet (MET Norway) for rise/set</label>
    <div class="hint">If enabled, the phone will fetch sun/moon rise/set from the Internet when possible, and fall back to local calculations if needed.</div>
  </div>
  <button id="save">Save</button>

  <script>
    const INITIAL_HOME = ${JSON.stringify(homeUi || { valid: false })};
    const INITIAL_USE_INTERNET = ${JSON.stringify(!!useInternet)};

    function closeWith(payload) {
      const encoded = encodeURIComponent(JSON.stringify(payload));
      // pebble-tool emulator support: it injects ?return_to=http://localhost:PORT/close?
      // If present, navigate there so pebble-tool can deliver the payload back to pkjs.
      try {
        const params = new URLSearchParams(location.search || '');
        const rt = params.get('return_to');
        if (rt) {
          location.href = rt + encoded;
          return;
        }
      } catch (e) {}
      // Real phone app webview path:
      location.href = 'pebblejs://close#' + encoded;
    }

    function load() {
      try {
        // Home
        document.getElementById('homeEnabled').checked = !!INITIAL_HOME.valid;
        if (typeof INITIAL_HOME.lat === 'number') document.getElementById('homeLat').value = String(INITIAL_HOME.lat);
        if (typeof INITIAL_HOME.lon === 'number') document.getElementById('homeLon').value = String(INITIAL_HOME.lon);
        if (typeof INITIAL_HOME.tzOffsetMin === 'number') document.getElementById('homeTz').value = String(INITIAL_HOME.tzOffsetMin);
        document.getElementById('useInternet').checked = !!INITIAL_USE_INTERNET;
      } catch (e) {}
    }

    function save() {
      // Home
      const homeEnabled = document.getElementById('homeEnabled').checked;
      if (!homeEnabled) {
        // no-op
      } else {
        const hlat = parseFloat(document.getElementById('homeLat').value);
        const hlon = parseFloat(document.getElementById('homeLon').value);
        const htz = parseInt(document.getElementById('homeTz').value, 10);
        if (!isFinite(hlat) || !isFinite(hlon) || !isFinite(htz)) {
          alert('Please enter valid Home latitude, longitude, and UTC offset minutes.');
          return;
        }
        // no-op
      }

      const useInternet = document.getElementById('useInternet').checked;
      closeWith({
        useInternet: !!useInternet,
        home: homeEnabled ? { valid: true, lat: parseFloat(document.getElementById('homeLat').value), lon: parseFloat(document.getElementById('homeLon').value), tzOffsetMin: parseInt(document.getElementById('homeTz').value, 10) } : { valid: false }
      });
    }

    function homeUseGps() {
      if (!navigator.geolocation) { alert('Geolocation not available'); return; }
      navigator.geolocation.getCurrentPosition(
        function(pos) {
          document.getElementById('homeEnabled').checked = true;
          document.getElementById('homeLat').value = String(pos.coords.latitude);
          document.getElementById('homeLon').value = String(pos.coords.longitude);
          document.getElementById('homeTz').value = String(-new Date().getTimezoneOffset());
        },
        function() { alert('Failed to get GPS'); },
        { enableHighAccuracy: false, timeout: 15000, maximumAge: 600000 }
      );
    }

    document.getElementById('save').addEventListener('click', save);
    document.getElementById('homeUseGps').addEventListener('click', homeUseGps);
    load();
  </script>
</body>
</html>`;
}

Pebble.addEventListener('ready', () => {
  State.lastLocSent = loadLastLocSent();
  loadTideStationsFromStorage();
  if (State.lastLocSent && typeof State.lastLocSent.ymd === 'number') {
    State.lastAstroYmd = State.lastLocSent.ymd | 0;
  }
  // Prefer Home override if set; otherwise request GPS.
  if (!sendHomeOverride()) requestLocation(true);
  sendUseInternetFlag();
  setInterval(maybeRefreshLocation, 10 * 60 * 1000);

  // Re-send astro data periodically (handles date rollovers even if GPS doesn't change)
  setInterval(() => {
    if (!sendHomeOverride()) requestLocation(false);
    maybeSendAstroForDayRollover();
    if (State.lastLoc) {
      maybeSendTides(State.lastLoc.latE6, State.lastLoc.lonE6, false);
      maybeSendWeather(State.lastLoc.latE6, State.lastLoc.lonE6, false);
    }
  }, 60 * 60 * 1000);
});

Pebble.addEventListener('appmessage', (e) => {
  const dict = e && e.payload ? e.payload : {};
  if (dict[KEYS.REQUEST_LOC]) {
    if (!sendHomeOverride()) requestLocation(true);
  }
});

Pebble.addEventListener('showConfiguration', () => {
  const ui = getUiModelFromStorage();
  const url = 'data:text/html;charset=utf-8,' + encodeURIComponent(configHtml(ui.homeUi, ui.useInternet));
  Pebble.openURL(url);
});

Pebble.addEventListener('webviewclosed', (e) => {
  if (!e || !e.response) return;
  try {
    const payload = JSON.parse(decodeURIComponent(e.response));
    if (payload && payload.home) {
      if (!payload.home.valid) {
        localStorage.setItem('home', JSON.stringify({ valid: false }));
      } else {
        const latE6 = Math.round(payload.home.lat * 1e6);
        const lonE6 = Math.round(payload.home.lon * 1e6);
        const obj = { valid: true, latE6: latE6, lonE6: lonE6, tzOffsetMin: payload.home.tzOffsetMin | 0 };
        localStorage.setItem('home', JSON.stringify(obj));
      }
      // Immediately send HOME override (or resume GPS if disabled)
      if (!sendHomeOverride()) requestLocation(false);
    }
    if (payload && typeof payload.useInternet === 'boolean') {
      localStorage.setItem('useInternet', payload.useInternet ? '1' : '0');
      sendUseInternetFlag();
      if (State.lastLoc) sendAstroForCurrentLocation(State.lastLoc.latE6, State.lastLoc.lonE6, State.lastLoc.tzOffsetMin);
    }
  } catch (err) {
    // ignore invalid responses
  }
});


