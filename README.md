# LineGuard — ESP32 + Web Bluetooth Voltage Monitor

LineGuard is a prototype **low-voltage DC** power-line monitoring system built on the classic ESP32-WROOM-32. An ESP32 samples an isolated 12 V DC bench supply through a resistor voltage divider, classifies the voltage (NORMAL / WARNING / CRITICAL UNDERVOLTAGE / SUPPLY LOSS), and streams telemetry over BLE to a Web Bluetooth dashboard that runs in the browser on a phone or desktop.

This prototype is intended for **bench testing on an isolated 12 V DC supply only**. It is **not** connected to, validated on, or designed for utility mains, overhead distribution lines, or any hazardous source.

---

## ⚠️ Safety

- The voltage divider and ESP32 are for an **isolated low-voltage DC prototype only** (≤ 15 V DC from a bench supply).
- **Never** connect the divider or ESP32 to mains AC, overhead distribution lines, or any other hazardous source.
- The ESP32's ADC input (GPIO34) must never exceed **3.3 V**. The default divider (100 kΩ / 27 kΩ) yields ~2.55 V at 12 V input.
- Common ground the bench supply and ESP32. Use a fuse or current-limited supply while experimenting.
- Voltage-only readings **cannot** conclusively identify a cut conductor, short circuit, or sensor fault. Alerts say "possible supply loss / line interruption; inspection required".

---

## Repository contents

| File | Purpose |
|---|---|
| `LineGuard.ino` | ESP32 firmware — ADC sampling, state machine, BLE GATT server |
| `index.html`   | Single-file Web Bluetooth dashboard (mobile-friendly) |
| `README.md`    | This file (also contains the MIT license) |

---

## Hardware

| Component | Value / Part |
|---|---|
| MCU board | ESP32 DevKit V1 (ESP32-WROOM-32) |
| Input supply | Isolated 12 V DC bench supply (nominal) |
| Upper resistor R1 | 100 kΩ, 1/4 W |
| Lower resistor R2 | 27 kΩ, 1/4 W |
| Status LED (optional) | On GPIO2 (disabled by default in firmware) |
| Buzzer (optional) | Any GPIO, not enabled in this build |

### Wiring

```
  +12 V (bench) ──[R1 100 kΩ]──┬── GPIO34 (ADC1_CH6)
                               │
                         [R2 27 kΩ]
                               │
  GND   ───────────────────────┴─── GND (ESP32 + bench supply common)
```

- ESP32 powered via USB (or 5 V pin from a **separate** isolated source).
- **Do not** connect the 12 V rail to any ESP32 GPIO.
- Divider midpoint goes **only** to GPIO34.
- Add a 0.1 µF ceramic from GPIO34 to GND for noise (optional but recommended).
- Confirm 12 V appears as ~2.55 V at GPIO34 with a multimeter before powering the ESP32.

---

## Firmware setup

1. Install **Arduino IDE 2.x**.
2. Install the **ESP32 board package** (Boards Manager → "esp32 by Espressif Systems" — core **3.x**).
3. Install library: **ArduinoJson** by Benoit Blanchon, version **7.x** (Library Manager).
4. Open `LineGuard.ino`.
5. Select **Board: "ESP32 Dev Module"** and the correct COM port.
6. Upload.
7. Open Serial Monitor at **115200 baud**. You should see:
   ```
   LineGuard booting...
   Initial: 12.00 V -> NORMAL
   BLE advertising started
   ```

### Configuration defaults (editable in firmware or via dashboard)

| Parameter | Default |
|---|---|
| Device ID | `LG-NODE-001` |
| Pole | `LG-001` |
| ADC pin | GPIO34 |
| R_top / R_bottom | 100 kΩ / 27 kΩ |
| Calibration factor | `1.0` |
| Normal threshold | 10.0 V |
| Warning threshold | 8.0 V |
| Critical threshold | 1.0 V |
| Hysteresis | 0.3 V |
| Debounce | 3000 ms |

All settings are persisted in ESP32 NVS (`Preferences`) and survive resets.

### Voltage calibration

1. Power the divider from a **known** DC voltage (e.g. exactly 12.00 V, measured by a multimeter at the divider input).
2. Read the voltage shown in the dashboard.
3. Compute `calibrationFactor = true_voltage / reported_voltage`.
4. Enter the factor in the dashboard "Calibration factor" field → tick confirm → **Apply configuration**.
5. The factor range is limited to **0.5 – 1.5** for sanity.

---

## Web dashboard setup

`index.html` is a **single self-contained file** — no build step, no dependencies.

### Requirements

- **Android Chrome** (primary target) or desktop **Chrome / Edge**.
- Served from a **secure context** — `https://` or `http://localhost`. Web Bluetooth refuses to work over plain `http://` on a LAN IP.
- iOS Safari and Firefox **do not** support Web Bluetooth.

### Hosting over HTTPS for phone testing

Three easy options:

**A. GitHub Pages (easiest for a project upload)**

1. Create a GitHub repo → upload the 3 files → **Settings → Pages** → deploy from `main` branch `/root`.
2. Open `https://<your-username>.github.io/<repo>/` on your phone in Chrome.

**B. ngrok (local tunnel)**

```bash
cd path/to/project
python -m http.server 8080
ngrok http 8080
```

Open the `https://*.ngrok.io` URL on your phone.

**C. Chrome DevTools port forwarding**

Connect the phone over USB with `adb`, open `chrome://inspect/#devices`, add a port forward for a local HTTP server, then browse to `http://localhost:8080` on the phone. This counts as a secure context.

### Using the dashboard

1. Open the page on your phone (Chrome).
2. Tap **Connect to LineGuard Node** → pick `LineGuard-LG-001` from the Bluetooth picker.
3. The live voltage, state badge, gauge and last-updated timestamp populate within one heartbeat (≤ 5 s).
4. Tap **Get approximate area** to grant phone location (optional). If denied, monitoring continues and you can type a manual area name instead.
5. On alerts: **Copy Alert** or **Share Alert** produce a plain-text message you can paste or share via your phone's share sheet. The prototype does **not** send SMS, WhatsApp, email, or any cloud notification itself.

### Stale / disconnect behaviour

- If no telemetry arrives for **15 s**, the dashboard marks the reading **stale** and the state becomes `NODE_OFFLINE`. The last voltage is intentionally *not* displayed as `0 V`.
- On BLE disconnect, the same `NODE_OFFLINE` behaviour applies. Last known reading is retained in the debug JSON but shown as `—` in the live panel.

---

## BLE protocol

### Service & characteristics

| Role | UUID |
|---|---|
| Service | `6c1b0001-7a3c-4f9e-9b2a-1d4e5f6a7b80` |
| Device ID (READ) | `6c1b0002-7a3c-4f9e-9b2a-1d4e5f6a7b80` |
| Pole number (READ) | `6c1b0003-7a3c-4f9e-9b2a-1d4e5f6a7b80` |
| Telemetry (READ, NOTIFY) | `6c1b0004-7a3c-4f9e-9b2a-1d4e5f6a7b80` |
| Config (READ, WRITE) | `6c1b0005-7a3c-4f9e-9b2a-1d4e5f6a7b80` |

### Notification chunk format

Each BLE notification carries:

```
[0x01][msgId][seq][total][payload bytes...]
```

`msgId` is an 8-bit counter that increments per logical message. `seq` runs 0..total-1. The client concatenates `payload` bytes in `seq` order until `total` chunks are collected, then parses the resulting UTF-8 JSON. MTU negotiation is honoured; chunks are capped at 500 bytes.

### Telemetry payload (abridged example)

```json
{
  "type": "telemetry",
  "event": "HEARTBEAT",
  "fw": "1.0.0",
  "deviceId": "LG-NODE-001",
  "pole": "LG-001",
  "online": true,
  "uptimeMs": 123456,
  "sensorValid": true,
  "adcMv": 2550,
  "voltage": 12.0,
  "state": "NORMAL",
  "stateCode": 0,
  "stateChangedAtMs": 120000,
  "alert": {
    "active": false,
    "type": "NORMAL",
    "message": "Voltage within expected range.",
    "sinceMs": 0,
    "voltage": 12.0
  },
  "location": { "configured": false }
}
```

`event` is one of: `HELLO` (first frame after subscribe, includes `config`), `HEARTBEAT` (every 5 s), `STATE_CHANGE` (after debounce), `CONFIG` (after a config write).

### Config write payload

Written to the Config characteristic as UTF-8 JSON. **Must include `"commit": true`** — otherwise the device replies `{"ok":false,"error":"commit_required"}`.

```json
{
  "commit": true,
  "deviceId": "LG-NODE-001",
  "pole": "LG-001",
  "calibrationFactor": 1.0,
  "vNormal": 10.0,
  "vWarning": 8.0,
  "vCritical": 1.0,
  "hysteresisV": 0.3,
  "debounceMs": 3000,
  "latitude": 12.971599,
  "longitude": 77.594566
}
```

Send `"clearLocation": true` (with `commit:true`) to wipe stored coordinates.

---

## Test plan (bench supply)

These are **expected software behaviours on a controlled isolated supply**, not validation on a real distribution network.

| Input | Expected state | Expected dashboard |
|---|---|---|
| 12.0 V | `NORMAL` | Green badge, "Voltage normal" |
| 9.0 V | `WARNING` | Yellow badge + warning card |
| 8.0 V | `WARNING` | Yellow badge |
| 7.0 V | `CRITICAL_UNDERVOLTAGE` | Red badge + critical card |
| 1.0 V | `CRITICAL_UNDERVOLTAGE` | Red badge |
| 0.5 V | `SUPPLY_LOSS` | Red badge "Possible supply loss or line interruption" |
| BLE disconnected | `NODE_OFFLINE` | Grey badge, "—" voltage, not 0 V |
| Oscillate 9.8 ↔ 10.2 V quickly | Stable state after debounce (no flicker) | Hysteresis + 3 s debounce |
| Location permission denied | Monitoring continues | Manual area input still works |
| No registered coordinates | `Not configured.` shown | Nothing invented |

To test the state machine:

1. Set the bench supply to 12.0 V → power cycle ESP32 → confirm `NORMAL`.
2. Slowly reduce to 9.0 V → after ~3 s the state becomes `WARNING`.
3. Drop to 0.5 V → `SUPPLY_LOSS` after debounce.
4. Power off the ESP32 → dashboard switches to `NODE_OFFLINE` within 15 s (or immediately on BLE disconnect).

---

## Limitations & future work

- **Voltage-only** classification cannot distinguish a cut conductor, short circuit, open switch, or sensor fault. Additional current sensing and comparison with neighbouring poles would be needed for stronger diagnosis.
- The prototype has **no GPS hardware** — installation coordinates are typed in via the dashboard and stored in ESP32 NVS.
- The dashboard runs entirely in the browser. **No cloud backend** is used; nothing leaves the phone or BLE link unless you explicitly tap **Share**.
- iOS Safari and most non-Chrome mobile browsers do not support Web Bluetooth.
- HTTPS (or localhost) is required for Web Bluetooth.
- Suggested extensions:
  - Add an SCT-013 current sensor for load-side confirmation.
  - Neighbouring-node comparison: a low reading at pole *n* with normal reading at pole *n+1* narrows the fault to the span between them.
  - Optional MQTT/HTTPS uplink to a monitoring dashboard for centralised history.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Connect button disabled | Open over HTTPS or localhost; use Chrome/Edge |
| Device not in picker | Ensure firmware uploaded, Serial Monitor shows "BLE advertising started" |
| Connects, then disconnects repeatedly | Reduce distance; keep the ESP32 away from USB3 ports and power cables |
| Voltage reads 0.00 V while 12 V applied | Check divider wiring; measure at GPIO34 with a multimeter; should be ~2.55 V |
| Voltage reads too high / too low | Recompute `calibrationFactor = true / reported` and apply |
| Alerts flicker | Increase `debounceMs` (min 500) or `hysteresisV` (0.2 – 0.5 typical) |
| `Rejected: threshold_order_invalid` | Ensure `vCritical < vWarning < vNormal` |
| Config write times out | Keep the phone within 1–2 m during configuration |
| History lost after refresh | Browsers may clear localStorage in private mode |

---

## License

MIT License

Copyright (c) 2025 Raagavendan R

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
