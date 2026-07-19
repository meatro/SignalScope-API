# Getting started

This guide takes a new SignalScope board from source code to a passive, decoded CAN view. Do that before transmitting or applying a mutation.

## 1. Know the reference build

The included `lilygo-t2can` PlatformIO environment builds for an ESP32-S3 with 16 MB flash and 8 MB PSRAM. The current firmware uses:

- ESP32-S3 TWAI for Bus A: RX GPIO 6, TX GPIO 7;
- MCP2515 for Bus B: CS 10, reset 9, interrupt 8, SCLK 12, MOSI 11, MISO 13;
- 500 kbit/s on both CAN buses;
- USB CDC at 115200 baud;
- a 16 MB partition layout from `partitions.csv`.

These are source-code facts, not a universal T-2CAN wiring promise. Check your board revision and transceiver termination before applying power. If you port to another board, change the hardware definition deliberately instead of treating a successful compile as proof of correct wiring.

## 2. Install the build tools

Use either:

- Visual Studio Code plus the PlatformIO extension; or
- PlatformIO Core, which supplies the `pio` command.

PlatformIO installs the pinned ESP32 platform and the libraries declared in `platformio.ini` on the first build. That first build therefore needs internet access.

## 3. Build and flash

From the repository root:

```powershell
pio run -e lilygo-t2can
pio run -e lilygo-t2can -t upload
pio run -e lilygo-t2can -t uploadfs
```

The first upload writes firmware. The second writes the contents of `data/` to LittleFS. You need both: firmware can boot without the filesystem, but the browser will report that its UI is missing.

If PlatformIO cannot choose a serial port, list ports and pass the correct one:

```powershell
pio device list
pio run -e lilygo-t2can -t upload --upload-port COM13
pio run -e lilygo-t2can -t uploadfs --upload-port COM13
```

Use the port Windows actually reports; `COM13` is only an example.

## 4. Read the boot report

Open a monitor:

```powershell
pio device monitor -b 115200
```

A useful first boot should report the CAN interfaces, LittleFS, any DBC/rule package it loaded, and the access-point address. The API exposes the same runtime health in more detail at `/api/status`.

If the board repeatedly disconnects from USB, remove it from the vehicle network and debug power/boot state on USB first. Firmware upload mode and normal runtime mode are different; do not keep forcing BOOT if the firmware has already uploaded successfully.

## 5. Join SignalScope

Connect a phone or computer to:

```text
Network: SignalScope-AP
Password: signalscope
```

Then open:

```text
http://192.168.4.1/
```

The device is an access point, not an internet router. A phone may label it “no internet” or try to leave it automatically. Disable that automatic switch for the test session if necessary.

## 6. Confirm passive traffic first

Before adding any rule, check the controller-health section:

- Bus A and Bus B should report ready when their hardware initialized.
- Forwarded-frame counts should increase on a live inline network.
- Dropped frames and transmit failures should stay at zero or be explainable.
- Raw frames should appear even if no DBC matches them.

The device is an inline gateway: traffic enters one physical side and is forwarded to the other. Make connections with the network powered down, retain the network's intended termination, and keep a quick way to return to the original wiring.

## 7. Load a DBC

The bundled `data/dbc/default.dbc` describes one synthetic `0x321` learning frame with:

- `OilTemperature`;
- `EngineSpeed`;
- `VehicleSpeed`.

It exists so the interface has understandable metadata immediately. It will not decode unrelated vehicle traffic.

For a real project:

1. Open **Load a DBC**.
2. Choose a `.dbc` that matches the network you are observing.
3. Load it and check the returned message/signal counts.
4. Search the signal catalog for a value you can physically verify.

Uploading a DBC stores it as `/dbc/active.dbc`. Replacing the active DBC also clears observation selections, replay, and active rules because their signal identities may no longer mean the same thing.

## 8. Verify a signal

Choose an easy signal before an obscure one. Examples include engine speed, vehicle speed at standstill, a switch state, or a temperature you can compare with another trusted display.

The catalog distinguishes three states:

- `valid: false`: no decodable frame sample is available;
- `valid: true, live: false`: a sample exists but is older than the live window;
- `valid: true, live: true`: a recent matching physical frame was decoded.

The current live window is 1.5 seconds. A plausible number that never changes is not yet proof that the DBC definition matches your network.

## 9. Stage, apply, then persist

When you are ready to test a rule:

1. Select the decoded signal so its bit position, length, endian setting, factor, and offset populate the builder.
2. Enter the physical value you want. The UI converts it to the raw integer stored in the frame.
3. Stage the rule. Staging does not affect CAN traffic.
4. Review the complete staged set.
5. Apply it. The active table changes atomically in RAM.
6. Observe the result and revert or clear it if it is not what you expected.
7. Only after validation, save a startup `.ssrules` package.

Applied-in-RAM is not saved. Rebooting returns to the last valid `/rules/active.ssrules` or `/rules/default.ssrules` package.

## 10. Next steps

- Build a small real UI with [the oil-temperature walkthrough](FIRST_APP.md).
- Learn why raw and physical values differ in [Core concepts](CONCEPTS.md).
- Use advanced rule types in [Rules and packages](RULES.md).
- Automate the device through the [HTTP API](API.md).
- Add on-device behavior through [Application extension](APPLICATION_EXTENSION.md).

## Troubleshooting

| Symptom | Check |
| --- | --- |
| AP exists but the page is missing | Run the `uploadfs` target and reboot. |
| No AP | Read serial boot output; confirm normal boot rather than ROM download mode. |
| Bus not ready | Check pins, oscillator, SPI/TWAI wiring, transceiver power, and bitrate. |
| Raw traffic but no decoded values | Confirm the DBC contains those CAN IDs and select an observation mode. |
| Value appears but is marked stale | The matching frame stopped arriving more than 1.5 seconds ago. |
| A rule works until reboot | It was applied in RAM but not saved as a startup package. |
| Runtime rule update returns HTTP 409 | Refresh `/api/rules`; the rule table epoch changed. |
| Phone loses internet | Expected while joined directly to the SignalScope AP. |
| DBC upload makes rules disappear | Expected: changing the database invalidates the old rule identities. |
