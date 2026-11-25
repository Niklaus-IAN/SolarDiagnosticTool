# Flutter App — BLE Integration Guide

How the mobile app connects to the ESP32 tool and receives live readings.
Mirror of the firmware BLE contract in `src/main.cpp` (Phase 5).

The ESP32 advertises as **`SolarDiag`** over the **Nordic UART Service (NUS)** and
notifies **one JSON packet per second** on the TX characteristic. The app parses that
JSON and feeds it into the fault-code engine (which lives on the app side).

---

## The contract

| | Value |
|---|---|
| Device name | `SolarDiag` |
| Service (NUS) | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| TX characteristic (device → app, **notify**) | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |
| RX characteristic (app → device, **write**) | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| Packet rate | ~1.05 s (1 s delay + sample time) — allow 3 s before flagging stale |
| MTU | app **must** request ≥ 185; firmware ceiling is 517 (packet ~97 bytes) |

### Packet format
```json
{"v_panel":21.80,"v_batt":12.71,"current":2.180,"power":47.5,"t_panel":41.5,"t_amb":33.0,"ts":142}
```

| Key | Unit | Notes |
|---|---|---|
| `v_panel` | V | panel voltage |
| `v_batt` | V | battery voltage |
| `current` | A | **signed** — negative means reverse flow. `abs(I) < 0.1 A` sends exactly `0` (deadband) |
| `power` | W | panel V × current |
| `t_panel` | °C | JSON `null` if probe missing — **never 0** |
| `t_amb` | °C | JSON `null` if probe missing — **never 0** |
| `ts` | s | uptime seconds since boot; monotonic heartbeat for staleness detection |

**Missing-probe convention:** the firmware sends JSON `null` for a disconnected probe
(it uses `DEVICE_DISCONNECTED_C`, i.e. −127, mapped to `null`). It never sends `0`
(0 °C is a valid reading) and never sends `-99`. The app must treat `null` explicitly —
the daytime/irradiance inference (`t_panel > t_amb + 3` and `+15`) depends on it.

### Command (app → device)
Write the text `CAL` to the RX characteristic (`6e400002-…`) to re-zero the ACS712
without a reboot. The RX characteristic **is exposed** in the firmware
(`PROPERTY_WRITE`) and handled server-side — the app side just needs to write to it.

> **Only send `CAL` with the ACS712 connected and no current flowing.** It samples the
> pin and writes the result to NVS permanently. Sent with the sensor unplugged, it
> stores a floating-pin value and destroys a good calibration. The firmware ignores any
> text not starting with `CAL`, so write anything else to test the path safely.

### Gotchas that have actually bitten us

- **MTU is client-initiated.** The server can only raise its own ceiling; it cannot
  force a bigger MTU. If the app never calls `requestMtu`, every packet truncates to
  20 bytes and `jsonDecode` throws on every frame — which the `catch` block swallows
  silently, so the symptom is an app that connects and then shows nothing at all.
- **One client at a time.** If nRF Connect is still connected, the app cannot connect.
- **`ts` resets to 0 on reboot.** A backwards jump means the device restarted, not that
  the data is corrupt. (It also wraps at ~49.7 days of uptime.)
- **Notifications are unacknowledged.** A dropped packet is gone; the next arrives ~1 s
  later. Never assume an unbroken sequence.
- **Current reads 0 at night by design.** The ACS712 is in the panel→controller line, so
  it measures PV current only — never load draw or battery discharge. Fault codes that
  need charge/discharge current cannot be computed with the sensor in this position.
- **Android caches device names.** Scan by service UUID, not by name.

---

## 1. Packages

`pubspec.yaml`:
```yaml
dependencies:
  flutter_blue_plus: ^1.32.12
  permission_handler: ^11.3.1
```
```bash
flutter pub get
```

> These pins date from 2024 and the caret locks `flutter_blue_plus` to 1.x. Check for a
> current release before starting — the API in §6 (`platformName`, `onValueReceived`,
> `requestMtu`) changed across major versions, so a newer package may need small edits.

## 2. Android permissions

`android/app/src/main/AndroidManifest.xml` (inside `<manifest>`, above `<application>`):
```xml
<uses-permission android:name="android.permission.BLUETOOTH_SCAN" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />
```
Set `minSdkVersion 21`+ in `android/app/build.gradle`.

## 3. iOS

`ios/Runner/Info.plist`:
```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>Connects to the solar diagnostic tool over Bluetooth.</string>
```

## 4. Constants

```dart
// ble_constants.dart
class Nus {
  static const service = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
  static const txChar  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // notify: device -> app
  static const rxChar  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // write:  app -> device
  static const deviceName = "SolarDiag";
}
```

## 5. Reading model

```dart
// reading.dart
class Reading {
  final double vPanel, vBatt, current, power;
  final double? tPanel, tAmb; // null when a probe is missing
  final int? ts;              // device uptime seconds (heartbeat)

  Reading({
    required this.vPanel, required this.vBatt, required this.current,
    required this.power, this.tPanel, this.tAmb, this.ts,
  });

  factory Reading.fromJson(Map<String, dynamic> j) => Reading(
    vPanel:  (j['v_panel'] as num).toDouble(),
    vBatt:   (j['v_batt']  as num).toDouble(),
    current: (j['current'] as num).toDouble(),
    power:   (j['power']   as num).toDouble(),
    tPanel: j['t_panel'] == null ? null : (j['t_panel'] as num).toDouble(),
    tAmb:   j['t_amb']   == null ? null : (j['t_amb']   as num).toDouble(),
    ts:     j['ts'] == null ? null : (j['ts'] as num).toInt(),
  );
}
```

## 6. BLE service

```dart
// ble_service.dart
import 'dart:async';
import 'dart:convert';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'ble_constants.dart';
import 'reading.dart';

class BleService {
  BluetoothDevice? _device;
  BluetoothCharacteristic? _tx, _rx;

  final _readings = StreamController<Reading>.broadcast();
  Stream<Reading> get readings => _readings.stream; // feed the fault engine from this

  Future<void> _ensurePermissions() async {
    await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ].request();
  }

  Future<void> connect() async {
    await _ensurePermissions();

    // 1. Scan filtered to the Nordic UART Service
    await FlutterBluePlus.startScan(
      withServices: [Guid(Nus.service)],
      timeout: const Duration(seconds: 10),
    );

    final result = await FlutterBluePlus.scanResults
        .expand((list) => list)
        .firstWhere((r) => r.device.platformName == Nus.deviceName);
    await FlutterBluePlus.stopScan();

    // 2. Connect
    _device = result.device;
    await _device!.connect(timeout: const Duration(seconds: 15));

    // 3. Bigger MTU — REQUIRED or the ~90-byte packet truncates
    await _device!.requestMtu(247);

    // 4. Find the characteristics
    final services = await _device!.discoverServices();
    final nus = services.firstWhere(
        (s) => s.uuid.str128.toLowerCase() == Nus.service);
    for (final c in nus.characteristics) {
      final id = c.uuid.str128.toLowerCase();
      if (id == Nus.txChar) _tx = c;
      if (id == Nus.rxChar) _rx = c;
    }

    // 5. Subscribe and parse each packet
    await _tx!.setNotifyValue(true);
    _tx!.onValueReceived.listen((bytes) {
      try {
        final map = jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
        _readings.add(Reading.fromJson(map));
      } catch (_) {
        // ignore a partial/garbled frame; the next one arrives in 1s
      }
    });
  }

  // App -> device: re-zero the ACS712 without a reboot
  Future<void> recalibrate() async {
    await _rx?.write(utf8.encode("CAL"), withoutResponse: false);
  }

  Future<void> disconnect() async {
    await _device?.disconnect();
  }
}
```

## 7. Wire into UI + fault engine

```dart
final ble = BleService();

// on a "Connect" button:
await ble.connect();

// consume the live stream (replaces the mock):
ble.readings.listen((r) {
  final faults = faultEngine.evaluate(r);          // existing app logic
  setState(() {
    latest = r;
    activeFaults = faults;
    health = faultEngine.healthScore(faults);
  });
});

// recalibrate button:
onPressed: () => ble.recalibrate(),
```

## 8. Test checklist

- [ ] Connect finds `SolarDiag` and connects (ESP32 powered + advertising)
- [ ] Readings arrive ~1/sec; `t_panel` ~28 confirms a live probe
- [ ] Repower the ESP32 → app handles the drop (add reconnect later)
- [ ] `recalibrate()` → serial prints "Recalibrating ACS712 zero" (**only with the ACS712 connected and no current flowing** — see the CAL warning above)
- [ ] Inject a high-voltage `Reading` → fault engine still fires

---

## Notes

- **Keys are aligned** to the app/sim/reference convention (`v_panel, v_batt, current,
  power, t_panel, t_amb, ts`) as of 2026-07-24 — the firmware `sendBlePacket()` in
  `src/main.cpp` was renamed to match (was previously `panel/battery/t_ambient`).
  Mismatches fail silently — a missing key parses as `0.0` — so keep both sides in sync.
- **Advertised device name is `SolarDiag`** (not `SolarOBD-ESP32`). Scanning by service
  UUID connects regardless; use the advertised name for display.
- **The fault engine lives on the app**, not the firmware. The firmware only sends the
  six raw readings; the app computes fault codes, severities, corrective actions, and the
  health score, using per-site panel ratings (Voc, Vmpp, Isc, rated power) as thresholds.
- **Verification status (2026-09-24).** Confirmed with nRF Connect: service, TX/RX
  characteristics, CCCD and notifications all correct, packets arriving. **Not yet
  confirmed:** a full untruncated packet. The observed TX value was
  `{"v_panel":3.09,"v_b` — exactly 20 bytes, i.e. MTU 23 minus the 3-byte ATT header.
  `setMTU(517)` is now in the firmware, but nRF Connect does not request a larger MTU
  on its own (⋮ → Request MTU). A Flutter client calling `requestMtu(247)` supplies
  the missing half. Tick this off once a packet ending in `}` has actually been seen.
