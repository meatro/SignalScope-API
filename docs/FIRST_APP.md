# First app: an oil-temperature display

This walkthrough starts with a named DBC signal and ends with a tiny mobile-friendly web app. It demonstrates the intended SignalScope workflow without adding application logic to the CAN core.

The display itself is passive. The optional rule step uses the bundled synthetic frame so you can learn the mutation workflow on a bench or simulator.

## What you will build

```text
CAN frame → SignalScope gateway/cache → DBC decode → HTTP JSON → your HTML
```

Your page will show:

- the current oil temperature;
- whether the sample is live, stale, or unavailable;
- the source CAN ID and direction;
- the age of the latest frame.

The complete example is in `examples/oil-temperature-dashboard/`.

## 1. Begin with a passive network

Connect and boot SignalScope as described in [Getting started](GETTING_STARTED.md). Confirm that the bus-ready states are healthy and raw frame counts increase. Do not create a rule yet.

## 2. Load the right DBC

For a real vehicle, load a DBC that contains the actual oil-temperature signal for that network. The name might be `OilTemperature`, `EngineOilTemp`, or something else; a text label is not standardized across DBC authors.

For a simulator or learning frame, keep the bundled `default.dbc`. It defines:

```dbc
BO_ 801 EngineStatus: 8 Sensor
 SG_ OilTemperature : 0|8@1+ (1,-40) [-40|215] "degC" Dashboard
```

Decimal CAN ID 801 is hexadecimal `0x321`. Raw byte 130 decodes to 90 °C.

## 3. Find and verify the signal

In **Signal explorer**, search for `oil` and select the likely result. Check:

- CAN ID;
- bit start and length;
- endian and signed settings;
- factor and offset;
- unit;
- live value and age.

On a real network, compare the value with another trusted data source and with the physical behavior of the system. Watch it warm gradually. If it jumps randomly, wraps, or stays implausibly fixed, stop and verify the DBC definition.

## 4. Read it from JavaScript

SignalScope serves your files and API from the same origin, so the browser needs no API key, CORS setup, npm package, or external server.

```js
async function readOilTemperature() {
  const response = await fetch('/api/signal_catalog?q=OilTemperature&limit=16', {
    cache: 'no-store'
  });
  const result = await response.json();
  const signal = result.signals.find(item => item.name === 'OilTemperature');

  if (!signal || !signal.valid) {
    return { state: 'unavailable' };
  }

  return {
    state: signal.live ? 'live' : 'stale',
    value: signal.value,
    ageMs: signal.ageMs,
    canId: signal.canIdHex,
    direction: signal.direction
  };
}
```

Poll at a human-interface rate such as 500–1000 ms. The CAN task and cache operate independently; making the browser poll every millisecond does not improve the underlying signal.

## 5. Customize the page

Copy `examples/oil-temperature-dashboard/index.html` into your own project or use it as the starting point for `data/index.html`. It is intentionally one file and heavily commented.

After changing files under `data/`, upload LittleFS again:

```powershell
pio run -e lilygo-t2can -t uploadfs
```

You are now building an app on SignalScope: the core still owns CAN and decoding, while your page decides what to show.

## 6. Optional: learn the rule workflow

Only do this on the synthetic `0x321` learning network or after independently establishing the correct rule for your own controlled system.

Select `OilTemperature` in the explorer and enter a physical value of `90`. The UI converts it using the DBC:

```text
raw = (90 - (-40)) / 1 = 130
```

The equivalent package row is:

```text
STATIC,0x321,A_TO_B,0,8,1,130
```

Now follow the state changes deliberately:

1. **Stage** — the candidate appears, but forwarded bytes do not change.
2. **Apply** — the complete staged table becomes active in RAM.
3. Observe the output and confirm the mutation marker and expected decoded value.
4. **Revert/clear** if it is not correct.
5. **Save startup package** only after it is proven. That writes `/rules/active.ssrules`.

Reboot once and confirm that a saved package loads. If you only applied it, the rule should not survive reboot.

## 7. Grow the app without rewriting CAN

Natural next steps include:

- add engine speed and vehicle speed cards from the same catalog;
- let users choose any catalog signal and persist its stable CAN-ID/name identity;
- graph only live samples and break the line when data becomes stale;
- record a short session for troubleshooting;
- add a native extension if the app must calculate values while no browser is connected;
- publish a runtime value from that extension and feed it into a dynamic `.ssrules` rule.

That progression is the point of SignalScope: begin with observation and HTML, then add only the embedded behavior your idea actually requires.
