#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const rootDir = path.resolve(__dirname, '..');
const pkjsPath = path.join(rootDir, 'src/pkjs/index.js');
const emulatorMode = process.env.CONFIG_EMULATOR === '1';
const outputPath = process.env.CONFIG_PREVIEW_OUTPUT
  || path.join(rootDir, emulatorMode ? 'build/config-emulator.html' : 'build/config-preview.html');

const storage = {};
if (process.env.CONFIG_USE_INTERNET != null) {
  storage.useInternet = process.env.CONFIG_USE_INTERNET === '1' ? '1' : '0';
}
if (process.env.CONFIG_UNITS_MODE != null) {
  storage.unitsMode = process.env.CONFIG_UNITS_MODE;
}
if (process.env.CONFIG_UI_UPDATE_INTERVAL_SEC != null) {
  storage.uiUpdateIntervalSec = process.env.CONFIG_UI_UPDATE_INTERVAL_SEC;
}
if (process.env.CONFIG_LANGUAGE != null) {
  storage.language = process.env.CONFIG_LANGUAGE;
}
if (process.env.CONFIG_LAT_E6 != null && process.env.CONFIG_LON_E6 != null) {
  storage.lastLocSent = JSON.stringify({
    latE6: parseInt(process.env.CONFIG_LAT_E6, 10),
    lonE6: parseInt(process.env.CONFIG_LON_E6, 10),
    tzOffsetMin: parseInt(process.env.CONFIG_TZ_MIN || '-480', 10),
    ymd: 0
  });
  if (process.env.CONFIG_LOCATION_CITY != null) {
    storage.locationCityName = process.env.CONFIG_LOCATION_CITY;
    storage.locationCityLatE6 = process.env.CONFIG_LAT_E6;
    storage.locationCityLonE6 = process.env.CONFIG_LON_E6;
  }
}

const language = process.env.CONFIG_LANG || process.env.LANG || 'en-US';

const sandbox = {
  Pebble: {
    addEventListener() {},
    openURL() {},
    getActiveWatchInfo() {
      return { language };
    }
  },
  navigator: { language },
  localStorage: {
    getItem(key) {
      return Object.prototype.hasOwnProperty.call(storage, key) ? storage[key] : null;
    },
    setItem(key, value) {
      storage[key] = String(value);
    },
    removeItem(key) {
      delete storage[key];
    }
  },
  console,
  setInterval() {},
  clearInterval() {},
  setTimeout() {},
  clearTimeout() {},
  Date,
  JSON,
  Math,
  parseInt,
  isFinite,
  encodeURIComponent,
  decodeURIComponent,
  String,
  Number,
  Array,
  Object
};

let source = fs.readFileSync(pkjsPath, 'utf8');
const readyMarker = "Pebble.addEventListener('ready'";
const readyIndex = source.indexOf(readyMarker);
if (readyIndex >= 0) {
  source = source.slice(0, readyIndex);
}

vm.createContext(sandbox);
vm.runInContext(source, sandbox, { filename: pkjsPath });

const ui = sandbox.getUiModelFromStorage();
let html = sandbox.configHtml(
  ui.useInternet,
  ui.unitsMode,
  ui.uiUpdateIntervalSec,
  ui.language,
  ui.locationDisplay
);

if (!emulatorMode) {
  html = html.replace(
    /<body>/,
    `<body>
  <div style="margin:0 0 12px;padding:10px 12px;background:#eef5ff;border:1px solid #b8d4ff;border-radius:8px;font-size:13px;">
    Browser preview only. Save shows the values below; nothing is sent to a watch.
  </div>
  <pre id="preview-result" style="display:none;margin:0 0 12px;padding:10px 12px;background:#f6f6f6;border:1px solid #ddd;border-radius:8px;white-space:pre-wrap;"></pre>`
  );

  html = html.replace(
    /function closeWith\(payload\) \{[\s\S]*?\n    \}/,
    `function closeWith(payload) {
      console.log('Config saved:', payload);
      const box = document.getElementById('preview-result');
      if (box) {
        box.textContent = JSON.stringify(payload, null, 2);
        box.style.display = 'block';
      } else {
        alert('Preview save:\\n' + JSON.stringify(payload, null, 2));
      }
    }`
  );
}

fs.mkdirSync(path.dirname(outputPath), { recursive: true });
fs.writeFileSync(outputPath, html, 'utf8');
console.log(outputPath);
