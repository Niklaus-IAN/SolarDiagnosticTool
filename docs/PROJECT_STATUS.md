# Solar Diagnostic Tool — Project Status

**Project:** OBD-II–style portable diagnostic tool for 12 V off-grid residential solar systems (DC side: panel → charge controller → battery).
**Platform:** ESP32-WROOM-32 · Arduino framework · PlatformIO
**Branch:** `unified-sensors`
**Last updated:** 2026-07-13

---

## 1. Summary

The tool is a **portable, self-powered handheld instrument**. It clips onto any 12 V
solar installation, reads voltage, current, and temperature, and reports live values
plus fault codes. Today the firmware reads all three sensors and prints them over USB
serial; the standalone display, wireless output, and diagnostic logic are still ahead.

---

## 2. What works now

A single unified sketch ([`src/main.cpp`](../src/main.cpp)) reads three sensors once per
second and prints one line over USB serial at 115200 baud.

| Measurement | Sensor          | ESP32 pin | Method                                   | Status |
|-------------|-----------------|-----------|------------------------------------------|--------|
| Voltage     | 0–25 V divider  | GPIO 34   | `V = (ADC/4095) × 3.3 × 5.0`             | Working |
| Current     | ACS712 30 A     | GPIO 35   | 66 mV/A, zero calibrated at boot         | Working |
| Temperature | DS18B20         | GPIO 4    | 1-Wire, non-blocking conversion          | Working |

**Firmware notes**
- 16-sample ADC oversampling for noise reduction.
- ACS712 zero offset measured at boot (200 samples) — tool must power on with no current flowing.
- DS18B20 conversion is queued and read on the next tick, so the loop never blocks.
- Builds clean: RAM 6.6 %, Flash 21.4 %.

**Current output** — laptop only (PlatformIO Serial Monitor):

```
Voltage: 13.42 V    Current: 2.180 A    Temp: 41.5 C
```

---

## 3. Target design (clip-on tool)

### Field interface
- Clip leads out of the enclosure: **Panel +**, **Battery +**, **Common −** (one shared negative).
- **Current:** ACS712 is *inline* — the DC line is broken and the tool inserted in series
  (break-and-insert; it is not a clamp).
- **Temperature:** DS18B20 probe clipped to the panel; a second probe reads ambient.

### Power
- Self-powered by a **USB power bank / LiPo → ESP32 VIN (5 V rail)**, independent of the system under test.
- ESP32 3V3 regulator pin feeds the **+3V3 rail** (DS18B20, OLED, pull-up).
- ACS712 is the only sensor on **+5 V**.

### Planned capabilities
- **Dual voltage** — add battery channel on **GPIO 33** (panel + battery simultaneously).
- **OLED (SSD1306, I²C 0x3C, GPIO 21/22)** — standalone reading, no laptop.
- **BLE (Nordic UART Service)** — 1 s JSON packets to a Flutter mobile app.
- **Diagnostics** — 12 fault codes across 5 domains (PV, current, battery, temp, performance)
  and a 0–100 health score.

---

## 4. Gap between now and target

### 4.1 Electrical issues to resolve first
These will produce **wrong readings on a real panel** if left as-is.

1. **Panel voltage clips at 16.5 V.** The ÷5 module on a 3.3 V ADC reads to only
   `3.3 × 5 = 16.5 V`, but panel Voc ≈ 22 V. Use a larger divider (≈ ÷8–÷11) and update
   `VCC_DIVIDER_RATIO`, or confirm Voc < 16.5 V. Battery (≤ 14.4 V) is within range.
2. **ACS712 output can exceed 3.3 V.** At 5 V supply it idles ~2.5 V and swings toward 5 V
   under load — past the ESP32 ADC/IO limit. Divide `OUT` (÷2 + recalibrate) or power the
   ACS712 from 3.3 V.

### 4.2 Firmware still to add
- Second voltage channel (GPIO 33).
- OLED output.
- BLE output (NUS + JSON packet).
- Fault-code logic and health score.

### 4.3 Hardware still to build
- Clip-lead harness.
- Handheld enclosure with power bank.

---

## 5. Recommended order

1. Fix the two electrical issues (divider ratio + ACS712 level).
2. Add the second voltage channel (GPIO 33).
3. Wire up the OLED for standalone readout.
4. Add fault-code logic + health score.
5. Add BLE, then integrate the Flutter app.

> Electrical fixes come first because every downstream feature depends on trustworthy numbers.

---

## 6. References

- Firmware: [`src/main.cpp`](../src/main.cpp)
- Build config: [`platformio.ini`](../platformio.ini)
- Repository: https://github.com/Niklaus-IAN/SolarDiagnosticTool
