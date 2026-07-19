# Third-party notices

SignalScope source, its standalone browser interface, examples, and documentation are distributed under the repository's [MIT License](LICENSE). The build downloads third-party software that remains under its own license.

## Direct build dependencies

| Component | Version/source selected by this project | Purpose |
| --- | --- | --- |
| pioarduino ESP32 platform | `platform-espressif32` tag `55.03.36` from the URL in `platformio.ini` | ESP32-S3 Arduino toolchain, framework integration, and PlatformIO build support |
| ArduinoJson | `7.4.2` | Bounded JSON support used by firmware/application code |
| autowp MCP2515 | `1.3.1` | MCP2515 CAN controller driver for Bus B |
| Espressif Arduino/ESP-IDF components | Resolved by the pinned pioarduino platform | Wi-Fi, TWAI, LittleFS, FreeRTOS, sleep, and device runtime |

PlatformIO may install transitive tools and packages in addition to this table. Their copyright and license files are available in the installed PlatformIO packages and their upstream source distributions. This notice is an inventory, not a replacement for those license texts.

Before redistributing a compiled firmware bundle, retain the notices required by the exact dependency versions you ship. Re-check them whenever `platformio.ini` changes.

## Browser assets

The standalone files under `data/` have:

- no CDN dependency;
- no npm runtime dependency;
- no commercial admin theme;
- no copied Velonic/ThemeForest templates, styles, scripts, fonts, or icons.

The interface uses system fonts, CSS, HTML, and JavaScript included with this MIT-licensed project. Application authors are responsible for the licenses of assets they add to their own `data/` tree.

## Example data

`data/dbc/default.dbc` and `data/rules/example.ssrules` are synthetic SignalScope learning data. They are not copied vehicle definitions and must not be presented as coverage for a real network.
