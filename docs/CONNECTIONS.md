# Wiring & Connections — Clip-On Solar Diagnostic Tool

Complete build map. Work top to bottom; tick each box as you go.
Matches the firmware on branch `unified-sensors`.

**Board note:** the Fritzing drawing uses an MH-ET Live ESP32 as a stand-in.
Pins are labelled `IOxx` = `GPIOxx`. On your real **DevKit V1**, always match by the
**printed pin label**, never by physical position — the two boards arrange pins differently.
The stand-in's `VCC` pin = your DevKit's **`VIN`**.

---

## 1. Build status

- [x] LM7805 power + breadboard rails (+5V / GND), both boards bridged
- [x] ESP32 `VCC` → +5V rail, `GND` → GND rail
- [x] Panel voltage module → `IO34`
- [x] Battery voltage module → `IO33`
- [ ] 22 kΩ in series on the panel input
- [ ] ACS712 → `IO35` (with ÷2 divider)
- [ ] DS18B20 ×2 → `IO4` (with 4.7 kΩ pull-up)
- [ ] 0.1 µF caps on the three ADC pins (optional)

---

## 2. Power

| From | To | Notes |
|---|---|---|
| Battery **12 V** (via clip) | LM7805 **IN** | |
| LM7805 **OUT** | **+5 V rail** | |
| LM7805 **GND** | **GND rail** | |
| +5 V rail (board 1) | +5 V rail (board 2) | red bridge jumper |
| GND rail (board 1) | GND rail (board 2) | black bridge jumper |
| **+5 V rail** | ESP32 **VIN** (`VCC` on the stand-in) | |
| **GND rail** | ESP32 **GND** | |
| ESP32 **3V3** pin | **+3V3 rail** | powers DS18B20 + pull-up |

> ⚠ **Real 7805 pinout is IN · GND · OUT** (front view, label facing you, legs down).
> The Fritzing part shows a different leg order — wire the real chip by datasheet, not by
> copying the drawing. Swapping IN and GND will destroy it.

> ⚠ **Fit a heatsink.** Dropping 12 V → 5 V at ESP32 currents sheds ~3.5 W as heat.

---

## 3. Voltage channels

Both modules are the standard 0–25 V passive divider with two sides:

- **Input side** (2-pin screw terminal): `VCC(+)` and `GND(−)` — the voltage being measured
- **Output side** (3-pin header): `S`, `+`, `−` — goes to the ESP32
- The output **`+` pin is a dummy — leave it unconnected**

### 3.1 Panel → `IO34`

| Module pin | Connects to |
|---|---|
| **VCC (+)** input | **22 kΩ** → then Panel **+** clip |
| **GND (−)** input | Common **−** clip |
| **S** output | **`IO34`** |
| **+** output | *leave empty (dummy)* |
| **−** output | **GND rail** |

Ratio with the 22 kΩ: `(22k + 30k + 7.5k) / 7.5k = 7.93`
→ firmware `PANEL_DIVIDER_RATIO = 7.93` (calibrate on the bench)

**Why the 22 kΩ:** without it the plain ÷5 module only reads to `3.3 × 5 = 16.5 V`,
but panel Voc ≈ 22 V would clip. With it, 22 V arrives at the pin as 2.77 V.
Any value ~18k–27k works; calibrate the real ratio afterwards.

### 3.2 Battery → `IO33`

Identical, **minus the 22 kΩ** (battery maxes at ~14.4 V → 2.88 V at the pin, safe on ÷5).

| Module pin | Connects to |
|---|---|
| **VCC (+)** input | Battery **+** clip *(no resistor)* |
| **GND (−)** input | Common **−** clip |
| **S** output | **`IO33`** |
| **+** output | *leave empty* |
| **−** output | **GND rail** |

Firmware `BATTERY_DIVIDER_RATIO = 5.0` (calibrate).

---

## 4. Current — ACS712-30A → `IO35`  *(remaining)*

The ACS712 output can swing toward 5 V, past the ESP32's 3.3 V limit, so it feeds a
**÷2 divider** before the pin.

| ACS712 pin | Connects to |
|---|---|
| **VCC** | **+5 V rail** |
| **GND** | **GND rail** |
| **OUT** | top of the ÷2 divider (see below) |
| **IP+ / IP−** | the in-line current path (heavy clip leads) |

**The ÷2 divider:**

```
ACS712 OUT ──[ 10 kΩ ]──┬──[ 10 kΩ ]── GND
                        │
                        └──→ IO35     (+ optional 0.1 µF to GND)
```

Both resistors 10 kΩ. The midpoint is your signal.
Firmware compensates: `V_sensor = V_adc × 2.0` (`ACS712_OUT_DIVIDER = 2.0`).
The boot-time zero calibration adapts automatically.

**Fritzing note:** use the core part *ACS712 Low Current Sensor Board v12*, ignore its
`Vref` and `GAIN` pins (your 30 A module doesn't have them), and rename the label to
`ACS712-30A` so the drawing isn't misleading.

---

## 5. Temperature — DS18B20 ×2 → `IO4`  *(remaining)*

Both probes share one 1-Wire bus. **No new components** beyond one pull-up.

| Probe wire | Connects to |
|---|---|
| **Red (VDD)** — both probes | **+3V3 rail** |
| **Yellow (DATA)** — both probes | **`IO4`** |
| **Black (GND)** — both probes | **GND rail** |
| **4.7 kΩ** | between `IO4` and **+3V3** (one resistor for the whole bus) |

You have 4.7 kΩ ×3 — only **one** is needed.

> Index order is set by each sensor's factory serial, **not** wiring order.
> Read both, warm one in your hand, and confirm which is index 0 vs 1.
> For the final build, read the 64-bit addresses and hard-code the assignment
> so a probe swap can't silently flip panel/ambient.

---

## 6. The clip harness (field interface)

Five leads leave the enclosure:

| Clip | Colour | Goes to | Lead weight |
|---|---|---|---|
| **IN** | red | Panel side of the opened MC4 | **heavy** — carries current |
| **OUT** | red | Controller side of the MC4 | **heavy** — carries current |
| **BATT+** | red | Battery **+** terminal | thin (sense only) |
| **COM−** | black | System negative (battery **−** terminal) | thin |
| **TEMP** | — | DS18B20 probe onto the panel | probe cable |

`IN` does double duty: it feeds the ACS712 **and** the panel voltage divider.

> ⚠ IN/OUT carry the full system current (~6–8 A on a 100 W/12 V system).
> Use your heaviest clips and wire for those two. If a clip warms up, it's undersized.

---

## 7. Common ground

Everything shares **one** ground:

- solar system negative (panel −, controller −, battery − are already one node)
- the **COM−** clip joins the tool to it
- inside the tool: ESP32 GND, both module `−` pins, ACS712 GND, both DS18B20 GND,
  LM7805 GND, and both breadboards' blue rails (joined by the black bridge)

Without a shared reference the ADC readings are meaningless.

**On-site check:** multimeter between panel **−** and battery **−** should read ≈0 V.
If not, that install has an unusual (isolated or positive-ground) configuration.

---

## 8. Field procedure

1. Power the tool ON with the current path still **open** — the ACS712 zero calibrates at boot.
2. Unplug the **MC4 connector** between panel and charge controller.
3. Clip **IN** to the panel side, **OUT** to the controller side.
4. Clip **BATT+** to the battery terminal, **COM−** to the system negative.
5. Clip the **temperature probe** to the panel surface.
6. Read. To remove: unclip everything and plug the MC4 back together.

---

## 9. Still to buy

| Part | Qty | For |
|---|---|---|
| 22 kΩ resistor | 1 | panel divider range fix — **required** |
| 10 kΩ resistor | 2 | ACS712 ÷2 protection — strongly recommended |
| 0.1 µF ceramic cap | 3 | ADC noise filtering — optional |
| TO-220 heatsink | 1 | LM7805 — recommended |

Everything else you already own.

---

## 10. Calibration (before trusting any reading)

| Quantity | Reference | Test points | Accept |
|---|---|---|---|
| Panel voltage | multimeter | 0, 5, 12, 18, 22 V | ±2 % |
| Battery voltage | multimeter | 10.5–14.4 V | ±2 % |
| Current | clamp meter | 0, 1, 5, 10, 20 A | ±5 % |
| Temperature | reference thermometer | ambient → ~60 °C | ±1 °C |

For each voltage channel compute the true ratio as
`ratio = V_multimeter / V_adc_pin` and put that number in the firmware.
Resistor tolerance (±5 %) makes the calculated ratio only approximate.
