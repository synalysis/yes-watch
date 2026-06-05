# pebble-yes-watch

A Pebble SDK 3 watchface inspired by the **YES Watch** style (see [https://yeswatch.com/](https://yeswatch.com/)): 24‑hour dial, day/night sector, moon phase disk, and a moon up/down ring (moonrise→moonset).

The project is designed to run well on both **color** and **black/white** Pebble models and to work in a **hybrid** mode:

- **Primary**: the phone companion (`src/pkjs/index.js`) computes and sends daily sun/moon rise/set plus moon phase.
- **Fallback**: if the phone is unavailable, the watch computes **sunrise/sunset** on-device (libm-free fixed‑point) and continues to render using cached data.

## Features

- **24‑hour scale** with even hour numerals and tick marks
- **Day/Night display** as a filled sector (day wedge over a night disk)
- **Moon ring** showing “moon up” interval
- **Moon phase disk** (with smoothing near full/new so it looks clean)
- **Loading screen** until valid location + astro data are available

## Build

From the project root:

```bash
pebble clean
pebble build
```

The output bundle is:

- `build/pebble-yes-watch.pbw`

Note: a `.pbw` contains **platform-specific binaries** (aplite/basalt/chalk/diorite/emery/flint/gabbro). You ship one `.pbw`, but Pebble installs the correct binary for the target watch.

## Run / Install in the emulator

Examples:

```bash
pebble install --emulator basalt
pebble logs --emulator basalt
```

To start an emulator, install the watchface, and save an app-start screenshot:

```bash
npm run screenshot -- basalt
```

The screenshot is saved to `screenshots/<model>/app-start.png` by default.
Pass a different model (`aplite`, `chalk`, `diorite`, `emery`, `flint`, `gabbro`) or set
`STARTUP_DELAY_SECONDS=10` if the emulator needs more time before capture.
The script seeds deterministic location/sun/moon data so the capture shows the
watchface instead of the startup loading screen; set `SEED_EMULATOR_DATA=0` to
capture the loading state.

## Configure in the emulator
```
pebble emu-app-config --emulator emery
```

You can target other emulators (`aplite`, `diorite`, `flint`, `chalk`, `emery`, `gabbro`) the same way.

## Configuration

The phone companion provides a simple config page:

- “**Use Internet (MET Norway) for rise/set**” (preferred; falls back to local computation on failure)
- **Units**: metric (Celsius, km/h) or imperial (Fahrenheit, mph), defaulting from the existing location heuristic
- **Corner update cycle**: 5s, 10s, 30s, or 60s

## Code layout

- `src/c/pebble-yes-watch.c`: app lifecycle, AppMessage receive, persistence
- `src/c/yes_draw.c`: all rendering (rings, wedges, hand, moon disk, loading screen)
- `src/c/yes_astro.c`: watch-side sunrise/sunset fallback (fixed-point, libm-free)
- `src/pkjs/index.js`: phone-side GPS, MET Norway fetch (preferred), local astro fallback, geofencing


