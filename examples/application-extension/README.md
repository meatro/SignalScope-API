# Application-extension starter

`starter_app.cpp` is an illustrative, compile-shaped native app that reads the bundled synthetic `OilTemperature` signal and exposes it through `/api/app/status`.

It demonstrates four important patterns:

1. provide a strong `registerSignalScopeApplication()` hook;
2. receive host services without owning CAN hardware;
3. resolve a signal by CAN ID plus name after the DBC loads;
4. re-resolve it after every database change.

It does not create a mutation, choose a vehicle DBC, or implement domain-specific limits.

To try it, add this source file to the PlatformIO build (for example, by copying it into a dedicated app source folder and adding that folder to `build_src_filter`). Do not compile two files that both define `registerSignalScopeApplication()`.

After flashing with the synthetic DBC, query:

```text
GET /api/app/status
```

Read [Application extension](../../docs/APPLICATION_EXTENSION.md) before adding runtime values, diagnostics, resources, or power behavior.
