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
  lastLoc: null,      // {latE6, lonE6, tzOffsetMin}
  lastLocSent: null,  // {latE6, lonE6, tzOffsetMin, ymd} persisted
  lastAstroYmd: 0,

  isSending: false,
  msgQueue: []
};

// --- "Geo-fence" / update thresholding ---
// Trigger recomputation+send only when location/timezone changed enough.
// 25km often changes sunrise/sunset by ~1 minute or less; use a wider threshold so
// that recomputations are more likely to yield different minute values.
const DIST_THRESHOLD_M = 60000; // 60 km
const TZ_THRESHOLD_MIN = 30;    // 30 minutes
// If the move is unlikely to change rise/set by at least this many minutes, skip.
const EXPECTED_SHIFT_MIN = 2;

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
    setTimeout(() => { sendAstroForCurrentLocation(latE6, lonE6, tzOffsetMin); }, 200);
  });
}

function requestLocation(force) {
  if (sendHomeOverride()) return;
  if (!navigator.geolocation) {
    return;
  }

  navigator.geolocation.getCurrentPosition(
    (pos) => sendLocation(pos, !!force),
    () => { /* ignore */ },
    {
      enableHighAccuracy: false,
      timeout: 15000,
      maximumAge: 10 * 60 * 1000
    }
  );
}

function maybeRefreshLocation() {
  // Refresh at most once per hour.
  const now = Date.now();
  if (now - State.lastSentAtMs > 60 * 60 * 1000) {
    requestLocation(false);
  }
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
    setTimeout(() => { sendAstroForCurrentLocation(home.latE6, home.lonE6, home.tzOffsetMin); }, 200);
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


