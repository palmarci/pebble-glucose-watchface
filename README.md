# MiniMed BG — Pebble watchface

A minimal Pebble watchface that displays the current blood glucose value pushed
from the [minimed-pebble-bridge](../minimed-pebble-bridge) Android app, which
reads it directly from a Medtronic MiniMed pump over Bluetooth.

This is an early proof of concept: it shows only the current BG number, how long
ago it was received, and the time/date. Delta, trend arrow and graph are planned
(the protocol keys are already reserved in `src/c/protocol.h`).

## Data path

```
MiniMed pump ──BLE/SAKE──▶ minimed-pebble-bridge (Android)
    ──PebbleKit Android 2──▶ Pebble/Core app ──BLE──▶ this watchface
```

Glucose is formatted (mmol/L) on the Android side; the watchface just displays
the string it receives (`KEY_BG_STRING`) and stamps it with `KEY_BG_TIMESTAMP`.
After `STALE_MINUTES` with no fresh reading it shows `---`.

## Protocol

Raw integer AppMessage keys shared with the Android app (see `src/c/protocol.h`).
The watchface sends a "ready" announcement (`KEY_PROTOCOL_VERSION`,
`KEY_CAPABILITIES = CAP_BG`) on launch and on Bluetooth reconnect, which prompts
the phone to push the latest reading immediately.

- **UUID:** `567a3f6e-97d0-4f3a-b63f-916a8213d284` (must match `APP_UUID` in the bridge app)

## Build & test

```sh
pebble build
pebble install --emulator flint     # Pebble 2 Duo target
pebble screenshot --no-open --emulator flint
pebble install --phone <phone-ip>   # real watch
```

To preview without a phone, enable `#define TEST_MODE` in `src/c/test_mode.h`.
