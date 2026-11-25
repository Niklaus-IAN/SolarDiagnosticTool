# Solar Diagnostic Tool — Project Status

**Project:** OBD-II–style portable diagnostic tool for 12 V off-grid residential solar systems (DC side: panel → charge controller → battery).
**Platform:** ESP32-WROOM-32 · Arduino framework · PlatformIO
**Branch:** `ble-app-integration`
**Last updated:** 2026-09-11

---

## 1. Summary

All five sensor channels are wired and verified on the bench, and the firmware
streams readings over both USB serial and BLE. The tool has **not** been connected
to a real solar system yet — two protective resistors are still missing, and the
voltage channels are uncalibrated.

---

## 2. What works now

Firmware ([`src/main.cpp`](../src/main.cpp)) reads five channels once per second,
prints them over USB serial at 115200 baud, and notifies the same values as JSON
over BLE.

| Measurement | Sensor | ESP32 pin | Status |
|-------------|--------|-----------|--------|
| Panel voltage | 0–25 V divider | GPIO 34 | Reads; uncalibrated |
| Battery voltage | 0–25 V divider | GPIO 33 | Reads; uncalibrated |
| Current | ACS712 30 A | GPIO 35 | Zero stable; slope untested |
| Temperature ×2 | DS18B20 (1-Wire) | GPIO 4 | Both probes detected |
| BLE output | NUS, 1 s JSON | — | Advertising as `SolarDiag` |

All three ADC pins are on **ADC1**, so BLE does not conflict with analog reads.

**Build:** RAM 12.0 %, Flash 87.1 %.

**Current serial output:**

```
Panel: 1.13 V   Batt: 0.71 V   I: 0.000 A   P: 0.0 W   Tp: 26.3 C   Ta: 26.7 C
```

---

## 3. Bench verification (2026-09-11)

Test source: one HYT 18650 cell, **4.04 V** on a multimeter. No solar panel or
12 V battery available yet.

| Channel | Applied | Read | Note |
|---------|---------|------|------|
| Panel (GPIO 34) | 4.04 V | 5.30 V | Inflated ×1.586 — firmware assumes the missing 22 kΩ |
| Battery (GPIO 33) | 4.04 V | 4.29 V | +6.2 % after the ADC fix (was 3.45 V, −14.6 %) |
| Current | none | 0.000–0.076 A | ≈3 ADC counts of noise; see §5.3 |
| Temperature | ambient | 26.3 / 26.7 °C | Both probes enumerate |

### 3.1 ADC conversion fix

`readPinVoltage()` and `calibrateCurrentZero()` now use `analogReadMilliVolts()`
instead of `(counts / 4095) × 3.3`. The raw formula assumes a linear response the
ESP32 ADC does not have at 11 dB attenuation — it read **15 % low** on both
channels. The calibrated call applies the per-chip eFuse correction and more than
halved the error.

### 3.2 Zero-input floor — expected, not a fault

With nothing connected both channels show a steady non-zero reading:

```
Panel: 1.13 V ÷ 7.93 = 142 mV   |   Batt: 0.71 V ÷ 5.00 = 142 mV
```

The ESP32 ADC is blind below ~150 mV — every input in that band yields the same
near-zero count — so the calibration reports the middle of the blind band. In
normal use the pin sits at 1.5–2.8 V, where this is negligible. A deadband is
still needed so a dead panel reports 0 V rather than 1.13 V.

---

## 4. Not yet done

### 4.1 Hardware — blocking, and a hazard

Neither ADC pin is protected. **Do not clip into a live system until both are fitted.**

| Part | Qty | Without it |
|------|-----|------------|
| 22 kΩ series, panel input | 1 | 22 V Voc puts **4.4 V on GPIO34** (max 3.3 V) |
| 10 kΩ ÷2 divider, ACS712 OUT | 2 | 20 A puts **3.82 V on GPIO35** (max 3.3 V) |
| 0.1 µF ADC caps | 3 | Optional noise filtering |
| TO-220 heatsink | 1 | Only if the LM7805 route is chosen (§4.4) |

### 4.2 Calibration

Both divider ratios are still theoretical. Needs a two-point measurement — one
cell (~4 V) and two in series (~8 V) — so the fixed offset can be separated from
the true slope. Calibrating at 4 V alone would over-correct at the 12–14 V
operating point. Procedure: [`CONNECTIONS.md §10`](CONNECTIONS.md).

### 4.3 Probe addressing

`getTempCByIndex(0/1)` returns probes in **ROM address order**, set by factory
serial — not wiring order. The panel/ambient assignment can silently flip if a
probe is replaced, which would invert the app's `t_panel > t_amb + 3` daytime
inference. Identify the probes by warming one, then hard-code both 64-bit
addresses.

### 4.4 Power topology — undecided

This document and [`CONNECTIONS.md §2`](CONNECTIONS.md) currently disagree:

- **USB power bank → VIN** — tool stays isolated from the system under test, drains nothing, needs the COM− clip for a voltage reference.
- **System battery → LM7805 → VIN** — simpler grounding, but drains the battery under test and sheds ~3.5 W as heat.

Settle this before building the enclosure.

### 4.5 Firmware still to add

- **OLED (SSD1306)** — libraries are in `platformio.ini` but no display code exists. Flash is at 87.1 %; use `huge_app.csv` if it overflows.
- **Fault codes / health score** — lives app-side by design, not in firmware.

---

## 5. Known firmware issues

### 5.1 BLE MTU is never raised

`BLEDevice::setMTU()` is never called, and `BLEDevice::init()` does not set it
either. Peer MTU initialises to 23 (`BLEServer.cpp:414`), giving **20 usable
bytes per notification** against a ~90-byte JSON packet. `notify()` only logs the
truncation and sends anyway, so the app would receive malformed JSON and discard
it silently. The app requesting `requestMtu(247)` is not sufficient — the
negotiated value is `min(client, server_local)`.

Fix: `BLEDevice::setMTU(517);` after `init()`. **Not yet verified on hardware** —
test with nRF Connect and confirm the packet arrives whole.

### 5.2 `CAL` command blocks the BLE stack

`RxCallbacks::onWrite` calls `calibrateCurrentZero()` directly — 200 ms of
`delay()` plus serial writes inside a GATT callback. It also writes
`g_iZeroVoltage` from the BLE task while `loop()` reads it. `g_bleConnected` and
`g_advPending` are likewise cross-task and not `volatile`. Set a flag and service
it in `loop()`.

### 5.3 Current clamp rectifies noise

`if (current < 0) current = 0` discards the negative half of the noise, so idle
averages **+0.03 A instead of 0** — the tool reports phantom current on a dead
panel or at night. It also hides reverse/night-time drain, which is a genuine
diagnostic signal. Consider removing the clamp or adding a deadband.

Note the resolution floor: one ADC count ≈ **24 mA** through the ACS712-30A
chain, so ±0.07 A is inherent and will not improve once the ÷2 divider is fitted.

### 5.4 Unpinned platform

`platform = espressif32` has no version. A future update pulls Arduino core 3.x,
where `getValue()` returns `String` and `std::string v = c->getValue()`
(`main.cpp:190`) stops compiling. Pin it: `platform = espressif32@7.0.1`.

### 5.5 Dead code

~90 lines of commented-out single-sensor sketches at the top of `main.cpp`.

---

## 6. Recommended order

1. Fit the 22 kΩ and the two 10 kΩ — safety gate for everything on a live system.
2. Two-point calibration of both voltage ratios.
3. Pinch-test the probes, hard-code the DS18B20 addresses.
4. Apply the BLE MTU fix; verify the whole packet with nRF Connect.
5. Move `CAL` out of the BLE callback; mark cross-task flags `volatile`.
6. Settle the power topology, then the OLED and the enclosure.

> The resistors come first because every downstream feature depends on readings
> the hardware can survive taking.

---

## 7. References

- Firmware: [`src/main.cpp`](../src/main.cpp)
- Wiring: [`CONNECTIONS.md`](CONNECTIONS.md)
- App BLE contract: [`FLUTTER_BLE.md`](FLUTTER_BLE.md)
- Build config: [`platformio.ini`](../platformio.ini)
- Repository: https://github.com/Niklaus-IAN/SolarDiagnosticTool
