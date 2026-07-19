# Oil-temperature dashboard example

This is the smallest useful SignalScope browser app: one dependency-free HTML file that reads a DBC-decoded signal and presents its freshness honestly.

It does not decode CAN itself and it does not contain a CAN ID, bit offset, or mutation strategy. SignalScope owns those jobs. The page searches the active DBC catalog for `OilTemperature` and displays the returned value.

## Try it

1. Load a DBC containing a verified signal named `OilTemperature` (the bundled synthetic DBC has one).
2. Copy `index.html` to `data/oil-temperature.html`.
3. Upload LittleFS:

   ```powershell
   pio run -e lilygo-t2can -t uploadfs
   ```

4. Join `SignalScope-AP` and open:

   ```text
   http://192.168.4.1/oil-temperature.html
   ```

If your DBC uses another name, change `SIGNAL_NAME` near the top of the script. For a finished app, persist a stable CAN-ID/name identity selected from the catalog rather than relying on a name that might be duplicated.

See [First app](../../docs/FIRST_APP.md) for the full discovery and verification workflow.
