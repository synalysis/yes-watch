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
- **Dedicated debug screen**

## Build

From the project root:

```bash
pebble clean
pebble build
```

The output bundle is:

- `build/pebble-yes-watch.pbw`

Note: a `.pbw` contains **platform-specific binaries** (basalt/chalk/diorite/emery/flint). You ship one `.pbw`, but Pebble installs the correct binary for the target watch.

## Run / Install in the emulator

Examples:

```bash
pebble install --emulator basalt
pebble logs --emulator basalt
```

## Configure in the emulator
```
pebble emu-app-config --emulator emery
```

You can target other emulators (`diorite`, `flint`, `chalk`, `emery`) the same way.

## Configuration

The phone companion provides a simple config page:

- Optional **Home override** (useful for emulator testing)
- “**Use Internet (MET Norway) for rise/set**” (preferred; falls back to local computation on failure)

## Debug screen (how to trigger)

The debug screen is a **separate screen** (not an overlay). It shows things like:

- Current displayed time + timezone offset
- Whether Internet mode is enabled
- Sun/Moon rise/set minutes and special states (always day/night, always up/down)
- Current lat/lon

### Toggle debug on/off

- **Primary**: **tap the watch** (accelerometer tap)
- **Fallback**: **long‑press the Down button**
- **Emulator**: `pebble emu-tap --emulator emery --direction z+`

## Code layout

- `src/c/pebble-yes-watch.c`: app lifecycle, AppMessage receive, persistence, debug toggle
- `src/c/yes_draw.c`: all rendering (rings, wedges, hand, moon disk, loading/debug screens)
- `src/c/yes_astro.c`: watch-side sunrise/sunset fallback (fixed-point, libm-free)
- `src/pkjs/index.js`: phone-side GPS, MET Norway fetch (preferred), local astro fallback, geofencing


