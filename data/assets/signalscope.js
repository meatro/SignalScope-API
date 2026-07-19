/*
 * SignalScope standalone UI
 *
 * This file is deliberately plain browser JavaScript. A new application can
 * copy the request helpers and replace the page without learning the CAN
 * controller, DBC parser, gateway, recorder, or mutation scheduler first.
 */

(() => {
  "use strict";

  const CATALOG_PAGE_SIZE = 48;
  const ACTIVE_PACKAGE_PATH = "/rules/active.ssrules";
  const MAX_REQUEST_LOG_LINES = 140;
  const byId = (id) => document.getElementById(id);
  const dom = {};

  [
    "themeToggle", "connectionBadge", "busAValue", "busBValue", "fpsValue",
    "dbcValue", "dbcDetail", "ruleCountValue", "rulePathValue", "dropValue",
    "dbcStatus", "dbcFile", "dbcFilename", "loadDbcButton", "autoloadDbcButton",
    "signalSearch", "signalRefresh", "signalRows", "signalTotal", "catalogPage",
    "loadMoreSignals", "frameRows", "frameRefresh", "selectedSignalHelp",
    "selectedSignal", "ruleBehavior", "ruleBehaviorHelp", "signalRulePersistence",
    "ruleCanId", "ruleDirection", "ruleStartBit", "ruleLength", "rulePhysicalValue",
    "ruleRawValue", "ruleLittleEndian", "encodingHelp", "stageRuleButton",
    "resetRuleButton", "rawCanId", "rawDirection", "rawFrameStatus", "rawBitGrid",
    "rawMaskText", "rawValueText", "loadRawFrameButton", "stageRawMaskButton",
    "clearRawMaskButton", "recipeType", "recipeDescription", "recipeFields",
    "recipePreview", "recipeNotice", "addRecipeButton", "resetRecipeButton",
    "ruleStateBadge", "ruleViewBadge", "ruleTableHelp", "ruleRows",
    "commitRulesButton", "revertRulesButton", "clearDraftButton", "clearRulesButton",
    "packageEditor", "packageText", "savePackageButton", "downloadPackageButton",
    "copyExampleButton", "oilExample", "logStatus", "startLogButton",
    "stopLogButton", "replayCanId", "replayData", "dryRunButton", "requestLog",
    "toastRegion"
  ].forEach((id) => { dom[id] = byId(id); });

  const state = {
    online: false,
    status: null,
    candidateRules: [],
    activeRules: [],
    candidateDirty: false,
    rulesInitialized: false,
    ruleEpoch: 0,
    ruleView: "staging",
    builderMode: "signal",
    selectedSignal: null,
    catalogSignals: [],
    catalogTotal: 0,
    catalogOffset: 0,
    catalogQuery: "",
    dbcText: "",
    dbcFilename: "",
    frameSeen: new Map(),
    frames: [],
    rawBitStates: Array(64).fill(-1),
    rawBaseBytes: null,
    pollBusy: new Set(),
    requestLines: []
  };

  class ApiError extends Error {
    constructor(message, status = 0, payload = null) {
      super(message);
      this.name = "ApiError";
      this.status = status;
      this.payload = payload;
    }
  }

  function element(tag, className, value) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (value !== undefined && value !== null) node.textContent = String(value);
    return node;
  }

  function appendRequestLog(method, path, status, elapsed, detail) {
    const stamp = new Date().toLocaleTimeString([], { hour12: false });
    const summary = detail ? ` - ${detail}` : "";
    state.requestLines.push(`${stamp}  ${method.padEnd(4)} ${path}  ${status}  ${Math.round(elapsed)}ms${summary}`);
    if (state.requestLines.length > MAX_REQUEST_LOG_LINES) {
      state.requestLines.splice(0, state.requestLines.length - MAX_REQUEST_LOG_LINES);
    }
    dom.requestLog.value = state.requestLines.join("\n");
    dom.requestLog.scrollTop = dom.requestLog.scrollHeight;
  }

  /**
   * One request boundary keeps error handling consistent for copied example
   * applications. Polls stay out of the visible log so it remains a record of
   * actions the person actually took.
   */
  async function api(path, options = {}) {
    const method = options.method || "GET";
    const quiet = Boolean(options.quiet);
    const expect = options.expect || "json";
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), options.timeout || 5500);
    const started = performance.now();

    try {
      const response = await fetch(path, {
        method,
        body: options.body,
        headers: options.headers || {},
        cache: "no-store",
        signal: controller.signal
      });
      const raw = await response.text();
      let parsed = raw;
      if (expect === "json" || (!response.ok && raw.trim().startsWith("{"))) {
        try {
          parsed = raw ? JSON.parse(raw) : {};
        } catch (error) {
          throw new ApiError("Device returned invalid JSON", response.status, raw);
        }
      }
      if (!response.ok) {
        const reason = parsed && typeof parsed === "object" && parsed.error
          ? parsed.error.replaceAll("_", " ")
          : `HTTP ${response.status}`;
        throw new ApiError(reason, response.status, parsed);
      }
      if (!quiet) appendRequestLog(method, path, response.status, performance.now() - started);
      return parsed;
    } catch (error) {
      const apiError = error.name === "AbortError" ? new ApiError("Device request timed out") : error;
      if (!quiet) appendRequestLog(method, path, apiError.status || "ERR", performance.now() - started, apiError.message);
      throw apiError;
    } finally {
      clearTimeout(timeout);
    }
  }

  function postForm(path, values, options = {}) {
    const body = new URLSearchParams();
    Object.entries(values).forEach(([key, value]) => {
      if (value !== undefined && value !== null) body.set(key, String(value));
    });
    return api(path, {
      ...options,
      method: "POST",
      body: body.toString(),
      headers: { "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8" }
    });
  }

  function postText(path, text, options = {}) {
    return api(path, {
      ...options,
      method: "POST",
      body: text,
      headers: { "Content-Type": "text/plain;charset=UTF-8" }
    });
  }

  function showToast(title, message = "", type = "info") {
    const toast = element("div", `toast ${type}`);
    const copy = element("div");
    copy.append(element("strong", "", title));
    if (message) copy.append(element("span", "", message));
    toast.append(copy);
    dom.toastRegion.append(toast);
    setTimeout(() => toast.remove(), 4800);
  }

  async function runAction(button, action) {
    if (button) button.disabled = true;
    document.body.classList.add("busy");
    try {
      return await action();
    } catch (error) {
      showToast("That did not complete", error.message || String(error), "error");
      throw error;
    } finally {
      if (button) button.disabled = false;
      document.body.classList.remove("busy");
    }
  }

  function setBadge(node, text, type = "neutral") {
    node.textContent = text;
    node.className = `badge ${type}`;
  }

  function formatCanId(value) {
    const id = Number(value);
    if (!Number.isFinite(id)) return "--";
    const width = id <= 0x7ff ? 3 : 8;
    return `0x${id.toString(16).toUpperCase().padStart(width, "0")}`;
  }

  function formatNumber(value, decimals = 1) {
    const number = Number(value);
    if (!Number.isFinite(number)) return "--";
    return number.toLocaleString(undefined, { maximumFractionDigits: decimals });
  }

  function parseCanId(value) {
    const text = String(value).trim();
    if (!/^(?:0x[0-9a-f]+|[0-9]+)$/i.test(text)) throw new Error("Enter a CAN ID such as 0x321");
    const id = Number(text);
    if (!Number.isInteger(id) || id < 0 || id > 0x1fffffff) throw new Error("CAN ID is outside the valid 29-bit range");
    return id;
  }

  function validateBitRange(startBit, length, littleEndian, maximumLength = 64) {
    if (!Number.isInteger(startBit) || startBit < 0 || startBit > 63) {
      throw new Error("Start bit must be between 0 and 63");
    }
    if (!Number.isInteger(length) || length < 1 || length > maximumLength) {
      throw new Error(`Length must be between 1 and ${maximumLength} bits`);
    }
    if (littleEndian) {
      if (startBit + length > 64) {
        throw new Error("The little-endian field does not fit inside the eight-byte frame");
      }
      return;
    }

    // Match MutationEngine::nextMotorolaBit exactly. Motorola/DBC fields do
    // not advance linearly at byte boundaries, so start + length is not a
    // sufficient frame-bounds check.
    let frameBit = startBit;
    for (let index = 0; index < length; index += 1) {
      if (frameBit > 63) {
        throw new Error("The big-endian field does not fit inside the eight-byte frame");
      }
      frameBit = frameBit % 8 === 0 ? frameBit + 15 : frameBit - 1;
    }
  }

  function updateConnection(online) {
    state.online = online;
    setBadge(dom.connectionBadge, online ? "Connected" : "Offline", online ? "success" : "danger");
  }

  function renderStatus(status) {
    state.status = status;
    updateConnection(true);
    dom.busAValue.textContent = status.bus_a_ready ? `${status.bus_a_util_pct || 0}%` : "Offline";
    dom.busBValue.textContent = status.bus_b_ready ? `${status.bus_b_util_pct || 0}%` : "Offline";
    dom.fpsValue.textContent = formatNumber(status.frame_rate_fps, 0);
    dom.dropValue.textContent = formatNumber(status.dropped_frames, 0);

    if (status.dbc_loaded) {
      dom.dbcValue.textContent = "Loaded";
      dom.dbcDetail.textContent = `${status.dbc_signal_count || 0} signals · ${status.dbc_path || "memory"}`;
      setBadge(dom.dbcStatus, `${status.dbc_signal_count || 0} signals`, status.dbc_complete === false ? "warning" : "success");
    } else {
      dom.dbcValue.textContent = "None";
      dom.dbcDetail.textContent = "No dictionary loaded";
      setBadge(dom.dbcStatus, "Waiting", "neutral");
    }

    dom.ruleCountValue.textContent = String(status.active_mutations || 0);
    dom.rulePathValue.textContent = status.rule_package_path || "Memory only";
    const candidateCount = Number(status.staging_mutations || 0);
    state.candidateDirty = Boolean(status.candidate_dirty);
    const draftLabel = state.candidateDirty
      ? `${candidateCount} pending rule${candidateCount === 1 ? "" : "s"}`
      : Number(status.active_mutations || 0) > 0 ? "Draft matches live" : "No rules";
    setBadge(dom.ruleStateBadge, draftLabel, state.candidateDirty ? "warning" : "neutral");

    const incomingEpoch = Number(status.rule_epoch || 0);
    if (incomingEpoch !== state.ruleEpoch) {
      state.ruleEpoch = incomingEpoch;
      void pollOnce("rules", refreshRules);
    }
  }

  async function pollStatus() {
    try {
      renderStatus(await api("/api/status", { quiet: true }));
    } catch (error) {
      updateConnection(false);
    }
  }

  function frameKey(frame) {
    return `${frame.can_id}:${frame.direction}`;
  }

  function renderFrames(frames) {
    dom.frameRows.replaceChildren();
    if (!frames.length) {
      const row = element("tr");
      const cell = element("td", "empty", "No frames observed yet.");
      cell.colSpan = 6;
      row.append(cell);
      dom.frameRows.append(row);
      return;
    }
    const now = Date.now();
    frames.forEach((frame) => {
      const seen = state.frameSeen.get(frameKey(frame));
      const fresh = Boolean(seen && now - seen.at < 2400 && Number(frame.rate_hz || 0) > 0);
      const row = element("tr");
      const id = element("td");
      id.append(element("code", "", formatCanId(frame.can_id)));
      row.append(id, element("td", "", frame.direction || "--"), element("td", "", frame.dlc ?? "--"));
      row.append(element("td", "frame-data", frame.data || "--"));
      row.append(element("td", "", `${formatNumber(frame.rate_hz, 1)} Hz`));
      const condition = element("td");
      const dot = element("span", `state-dot ${frame.mutated ? "mutated" : fresh ? "live" : ""}`);
      condition.append(dot, document.createTextNode(frame.mutated ? "Mutated" : fresh ? "Live" : "Stale"));
      row.append(condition);
      dom.frameRows.append(row);
    });
  }

  async function pollFrames() {
    const payload = await api("/api/frame_cache?limit=64", { quiet: true });
    state.frames = payload.frames || [];
    const now = Date.now();
    state.frames.forEach((frame) => {
      const key = frameKey(frame);
      const previous = state.frameSeen.get(key);
      if (!previous || previous.timestamp !== frame.timestamp_us) {
        state.frameSeen.set(key, { timestamp: frame.timestamp_us, at: now });
      }
    });
    renderFrames(state.frames);
  }

  function signalMetadata(signal) {
    const order = signal.littleEndian ? "Intel" : "Motorola";
    const signed = signal.signed ? "signed" : "unsigned";
    return `#${signal.index} · bit ${signal.startBit}:${signal.length} · ${order}, ${signed}`;
  }

  function renderSignals() {
    dom.signalRows.replaceChildren();
    if (!state.catalogSignals.length) {
      const row = element("tr");
      const message = state.status && state.status.dbc_loaded
        ? "No signals match this search."
        : "Load a DBC to explore named signals.";
      const cell = element("td", "empty", message);
      cell.colSpan = 4;
      row.append(cell);
      dom.signalRows.append(row);
    } else {
      state.catalogSignals.forEach((signal) => {
        const row = element("tr");
        const nameCell = element("td");
        const name = element("span", "signal-name");
        name.append(element("strong", "", signal.name), element("small", "", signalMetadata(signal)));
        nameCell.append(name);

        const liveCell = element("td");
        const liveValue = element("span", `live-value ${signal.live ? "" : "stale"}`, signal.valid ? formatNumber(signal.value, 4) : "--");
        liveValue.dataset.signalValue = String(signal.index);
        liveValue.title = signal.live
          ? `Fresh frame${signal.ageMs === null ? "" : ` · ${signal.ageMs} ms old`}`
          : signal.valid ? "Last decoded value is stale" : "No matching frame observed";
        liveCell.append(liveValue);

        const canCell = element("td");
        canCell.append(element("code", "", signal.canIdHex || formatCanId(signal.canId)));
        const actionCell = element("td");
        const useButton = element("button", "button quiet", "Build rule");
        useButton.type = "button";
        useButton.dataset.useSignal = String(signal.index);
        actionCell.append(useButton);
        row.append(nameCell, liveCell, canCell, actionCell);
        dom.signalRows.append(row);
      });
    }

    dom.signalTotal.textContent = `${state.catalogTotal} signal${state.catalogTotal === 1 ? "" : "s"}`;
    const shown = state.catalogSignals.length;
    dom.catalogPage.textContent = shown ? `Showing 1–${shown} of ${state.catalogTotal}` : "Showing 0";
    dom.loadMoreSignals.hidden = shown >= state.catalogTotal;
  }

  async function refreshCatalog(reset = true) {
    if (reset) {
      state.catalogOffset = 0;
      state.catalogSignals = [];
      state.catalogQuery = dom.signalSearch.value.trim();
    }
    const params = new URLSearchParams({
      q: state.catalogQuery,
      offset: String(state.catalogOffset),
      limit: String(CATALOG_PAGE_SIZE)
    });
    const payload = await api(`/api/signal_catalog?${params}`, { quiet: true });
    const page = payload.signals || [];
    state.catalogSignals = reset ? page : state.catalogSignals.concat(page);
    state.catalogTotal = Number(payload.total || 0);
    state.catalogOffset = state.catalogSignals.length;
    renderSignals();
  }

  async function refreshCatalogLiveValues() {
    if (!state.catalogSignals.length) return;
    const indexes = state.catalogSignals.map((signal) => signal.index).join(",");
    const params = new URLSearchParams({ indexes, limit: String(state.catalogSignals.length) });
    const payload = await api(`/api/signal_catalog?${params}`, { quiet: true });
    const updates = new Map((payload.signals || []).map((signal) => [Number(signal.index), signal]));
    state.catalogSignals = state.catalogSignals.map((signal) => updates.get(Number(signal.index)) || signal);

    document.querySelectorAll("[data-signal-value]").forEach((node) => {
      const signal = updates.get(Number(node.dataset.signalValue));
      if (!signal) return;
      node.textContent = signal.valid ? formatNumber(signal.value, 4) : "--";
      node.classList.toggle("stale", !signal.live);
      node.title = signal.live
        ? `Fresh frame${signal.ageMs === null ? "" : ` · ${signal.ageMs} ms old`}`
        : signal.valid ? "Last decoded value is stale" : "No matching frame observed";
    });
  }

  function selectSignal(signal) {
    state.selectedSignal = signal;
    dom.selectedSignal.hidden = false;
    dom.selectedSignal.replaceChildren();
    dom.selectedSignal.append(
      element("strong", "", signal.name),
      element("span", "", signal.canIdHex || formatCanId(signal.canId)),
      element("small", "", signalMetadata(signal)),
      element("small", "", `factor ${signal.factor} · offset ${signal.offset}`)
    );
    dom.selectedSignalHelp.textContent = "The DBC supplied this bit location and conversion. Verify its decoded value before applying a rule.";
    dom.ruleCanId.value = signal.canIdHex || formatCanId(signal.canId);
    dom.ruleDirection.value = signal.direction || "A_TO_B";
    dom.ruleStartBit.value = signal.startBit;
    dom.ruleLength.value = signal.length;
    dom.ruleLittleEndian.checked = Boolean(signal.littleEndian);
    dom.rulePhysicalValue.disabled = false;
    if (signal.valid && Number.isFinite(Number(signal.value))) dom.rulePhysicalValue.value = signal.value;
    dom.rawCanId.value = signal.canIdHex || formatCanId(signal.canId);
    dom.rawDirection.value = signal.direction || "A_TO_B";
    seedRecipeTargetFromSignal(signal);
    syncRawFromPhysical();
    setBuilderMode("signal");
    document.getElementById("rules").scrollIntoView({ behavior: "smooth", block: "start" });
  }

  function clampBigInt(value, minimum, maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
  }

  function clearSelectedSignal(message = "Target edited manually; raw value is now authoritative.") {
    state.selectedSignal = null;
    dom.selectedSignal.hidden = true;
    dom.selectedSignal.replaceChildren();
    dom.selectedSignalHelp.textContent = "Choose Build rule beside a DBC signal, or use the raw field target below.";
    dom.rulePhysicalValue.disabled = true;
    dom.encodingHelp.textContent = message;
  }

  function clearSignalConversionIfTargetChanged() {
    const signal = state.selectedSignal;
    if (!signal) return;
    let canIdMatches = false;
    try {
      canIdMatches = parseCanId(dom.ruleCanId.value) === Number(signal.canId);
    } catch (error) {
      // An incomplete manual CAN ID no longer identifies the selected signal.
    }
    const stillMatches = canIdMatches &&
      dom.ruleDirection.value === (signal.direction || "A_TO_B") &&
      Number(dom.ruleStartBit.value) === Number(signal.startBit) &&
      Number(dom.ruleLength.value) === Number(signal.length) &&
      dom.ruleLittleEndian.checked === Boolean(signal.littleEndian);
    if (!stillMatches) clearSelectedSignal();
  }

  /** Encode a DBC physical value into the unsigned bit pattern carried by CAN. */
  function syncRawFromPhysical() {
    const signal = state.selectedSignal;
    if (!signal) {
      dom.encodingHelp.textContent = "No DBC signal selected; raw value is used directly.";
      return;
    }
    const physical = Number(dom.rulePhysicalValue.value);
    const factor = Number(signal.factor);
    const offset = Number(signal.offset);
    const length = Number(dom.ruleLength.value);
    if (!Number.isFinite(physical) || !Number.isFinite(factor) || factor === 0 || !Number.isInteger(length) || length < 1 || length > 64) {
      dom.encodingHelp.textContent = "Enter a valid value and bit length before encoding.";
      return;
    }

    let logical = BigInt(Math.round((physical - offset) / factor));
    const bits = BigInt(length);
    const modulus = 1n << bits;
    if (signal.signed) {
      const half = 1n << (bits - 1n);
      logical = clampBigInt(logical, -half, half - 1n);
    } else {
      logical = clampBigInt(logical, 0n, modulus - 1n);
    }
    const encoded = logical < 0n ? modulus + logical : logical;
    dom.ruleRawValue.value = encoded.toString();
    const signedNote = signal.signed && logical < 0n ? " (two's-complement)" : "";
    dom.encodingHelp.textContent = `${physical} physical → raw ${logical.toString()} → stored ${encoded.toString()}${signedNote}`;
  }

  function syncPhysicalFromRaw() {
    const signal = state.selectedSignal;
    if (!signal) return;
    try {
      const encoded = BigInt(dom.ruleRawValue.value.trim());
      const length = BigInt(Number(dom.ruleLength.value));
      const modulus = 1n << length;
      let logical = encoded;
      if (signal.signed && (encoded & (1n << (length - 1n)))) logical = encoded - modulus;
      const physical = Number(logical) * Number(signal.factor) + Number(signal.offset);
      if (Number.isFinite(physical)) dom.rulePhysicalValue.value = String(Number(physical.toPrecision(12)));
      dom.encodingHelp.textContent = `Stored ${encoded.toString()} → raw ${logical.toString()} → ${formatNumber(physical, 6)} physical`;
    } catch (error) {
      dom.encodingHelp.textContent = "Raw value must be a whole number.";
    }
  }

  function setBuilderMode(mode) {
    if (!new Set(["signal", "raw", "package"]).has(mode)) return;
    state.builderMode = mode;
    document.querySelectorAll("[data-builder-mode]").forEach((button) => {
      const selected = button.dataset.builderMode === mode;
      button.classList.toggle("active", selected);
      button.setAttribute("aria-selected", selected ? "true" : "false");
    });
    document.querySelectorAll("[data-builder-panel]").forEach((panel) => {
      panel.hidden = panel.dataset.builderPanel !== mode;
    });
    if (mode === "raw") renderRawBitGrid();
    if (mode === "package") updateRecipePreview();
  }

  function updateRuleBehavior() {
    const dynamic = dom.ruleBehavior.value === "dynamic";
    setBadge(dom.signalRulePersistence, dynamic ? "RAM only" : "Package-ready", dynamic ? "warning" : "success");
    dom.ruleBehaviorHelp.textContent = dynamic
      ? "A live-adjustable rule starts with this raw value and can be changed from the Draft table. It is limited to 32 bits and is not a .ssrules row."
      : "A fixed rule can be written to a startup package after you prove it.";
  }

  function resetRuleBuilder() {
    state.selectedSignal = null;
    dom.selectedSignal.hidden = true;
    dom.selectedSignal.replaceChildren();
    dom.selectedSignalHelp.textContent = "Choose Build rule beside a DBC signal, or open the frame target and enter its bit location manually.";
    dom.ruleBehavior.value = "static";
    dom.ruleCanId.value = "0x321";
    dom.ruleDirection.value = "A_TO_B";
    dom.ruleStartBit.value = "0";
    dom.ruleLength.value = "8";
    dom.rulePhysicalValue.value = "90";
    dom.rulePhysicalValue.disabled = true;
    dom.ruleRawValue.value = "130";
    dom.ruleLittleEndian.checked = true;
    dom.encodingHelp.textContent = "No DBC signal selected; enter the raw integer carried by the CAN field.";
    updateRuleBehavior();
  }

  function ruleFormValues() {
    const canId = parseCanId(dom.ruleCanId.value);
    const startBit = Number(dom.ruleStartBit.value);
    const length = Number(dom.ruleLength.value);
    const dynamic = dom.ruleBehavior.value === "dynamic";
    validateBitRange(startBit, length, dom.ruleLittleEndian.checked);
    if (dynamic && length > 32) throw new Error("Live-adjustable rules are limited to 32 bits");
    let raw;
    try {
      raw = BigInt(dom.ruleRawValue.value.trim());
    } catch (error) {
      throw new Error("CAN raw value must be a whole number");
    }
    const maximum = (1n << BigInt(length)) - 1n;
    if (raw < 0n || raw > maximum) throw new Error(`CAN raw value must fit in ${length} bits`);
    return {
      canId,
      direction: dom.ruleDirection.value,
      startBit,
      length,
      littleEndian: dom.ruleLittleEndian.checked,
      dynamic,
      raw: raw.toString()
    };
  }

  function packageHeader() {
    return "# SignalScope rule package\n# Generated in the Rule workstation; review before installing.";
  }

  function packageRowIdentity(line) {
    const fields = String(line).split(",").map((field) => field.trim());
    const type = fields[0];
    if (!type || type.startsWith("#")) return "";
    if (type === "STATIC") return fields.length >= 6 ? `BIT_RANGE_STATIC,${fields.slice(1, 6).join(",")}` : "";
    if (type === "SOURCE_INT" || type === "SOURCE_SELECT_INT") {
      // Both compile to the same dynamic BIT_RANGE identity. The guided editor
      // must visibly replace one with the other just as the firmware does.
      return fields.length >= 6 ? `BIT_RANGE_SOURCE,${fields.slice(1, 6).join(",")}` : "";
    }
    if (type === "COUNTER" || type === "SEQUENCE8") {
      return fields.length >= 6 ? fields.slice(0, 6).join(",") : "";
    }
    if (["CHECKSUM_XOR", "CHECKSUM_CRC8_AUTOSAR"].includes(type)) {
      return fields.length >= 4 ? fields.slice(0, 4).join(",") : "";
    }
    // BIND_* directives are positional state changes, not engine rules. Two
    // directives with the same names can intentionally define different
    // scopes later in one package, so they must never be deduplicated here.
    return "";
  }

  function insertPackageBinding(lines, line) {
    const fields = String(line).split(",").map((field) => field.trim());
    const type = fields[0];
    let insertAt = lines.length;

    if (type === "BIND_TABLE" || type === "BIND_OVERRIDE") {
      const source = fields[1];
      const matchingSource = lines.findIndex((current) => {
        const row = String(current).split(",").map((field) => field.trim());
        return (row[0] === "SOURCE_INT" || row[0] === "SOURCE_SELECT_INT") && row[6] === source;
      });
      if (matchingSource >= 0) insertAt = matchingSource;
    } else if (type === "BIND_ACTIVE") {
      const firstRule = lines.findIndex((current) => {
        const rowType = String(current).split(",", 1)[0].trim();
        return rowType && !rowType.startsWith("#") && !rowType.startsWith("BIND_");
      });
      if (firstRule >= 0) insertAt = firstRule;
    }

    // Insert only the new directive. Existing directives stay byte-for-byte
    // and position-for-position intact so expert-authored scopes are safe.
    lines.splice(insertAt, 0, line);
  }

  function upsertPackageLine(line) {
    const identity = packageRowIdentity(line);
    const existing = dom.packageText.value.trim()
      ? dom.packageText.value.trimEnd().split(/\r?\n/)
      : packageHeader().split("\n");
    const rowType = String(line).split(",", 1)[0].trim();
    if (rowType.startsWith("BIND_")) {
      insertPackageBinding(existing, line);
      dom.packageText.value = `${existing.join("\n")}\n`;
      return;
    }
    let lastMatch = -1;
    if (identity) {
      existing.forEach((current, index) => {
        if (packageRowIdentity(current) === identity) lastMatch = index;
      });
    }
    const next = [];
    existing.forEach((current, index) => {
      // The engine reserves sequence at the first matching row but keeps the
      // content and positional bindings from the last. Replace only that last
      // row; retaining earlier duplicates preserves both pieces of semantics.
      next.push(index === lastMatch ? line : current);
    });
    if (lastMatch < 0) next.push(line);
    dom.packageText.value = `${next.join("\n")}\n`;
  }

  function appendRuleToPackage(rule) {
    upsertPackageLine(
      `STATIC,${formatCanId(rule.canId)},${rule.direction},${rule.startBit},${rule.length},${rule.littleEndian ? 1 : 0},${rule.raw}`
    );
  }

  async function stageRule() {
    const rule = ruleFormValues();
    const payload = await postForm("/api/rules/stage", {
      rule_kind: "BIT_RANGE",
      can_id: formatCanId(rule.canId),
      direction: rule.direction,
      start_bit: rule.startBit,
      length: rule.length,
      little_endian: rule.littleEndian ? 1 : 0,
      dynamic: rule.dynamic ? 1 : 0,
      replace_value: rule.raw,
      enabled: 1
    });
    state.ruleEpoch = Number(payload.rule_epoch || state.ruleEpoch);
    if (!rule.dynamic) appendRuleToPackage(rule);
    state.ruleView = "staging";
    await Promise.all([refreshRules(), pollStatus()]);
    showToast(
      rule.dynamic ? "Live-adjustable rule added to draft" : "Fixed rule added to draft",
      rule.dynamic
        ? "This rule lives in RAM; use the value control in the Draft table to tune it."
        : "Its matching STATIC row is also ready in the startup package source.",
      "success"
    );
  }

  function rawMaskBytes() {
    const mask = Array(8).fill(0);
    const value = Array(8).fill(0);
    state.rawBitStates.forEach((bitState, index) => {
      if (bitState < 0) return;
      const byteIndex = Math.floor(index / 8);
      const bitIndex = index % 8;
      mask[byteIndex] |= 1 << bitIndex;
      if (bitState === 1) value[byteIndex] |= 1 << bitIndex;
    });
    return { mask, value };
  }

  function bytesToHex(bytes) {
    return bytes.map((byte) => Number(byte).toString(16).toUpperCase().padStart(2, "0")).join("");
  }

  function renderRawBitGrid() {
    const { mask, value } = rawMaskBytes();
    dom.rawMaskText.value = bytesToHex(mask);
    dom.rawValueText.value = bytesToHex(value);
    dom.rawBitGrid.replaceChildren();

    for (let byteIndex = 0; byteIndex < 8; byteIndex += 1) {
      const row = element("div", "byte-row");
      const label = element("div", "byte-label");
      label.append(element("strong", "", `Byte ${byteIndex}`));
      const before = state.rawBaseBytes ? state.rawBaseBytes[byteIndex] : null;
      const after = before === null ? null : ((before & (~mask[byteIndex] & 0xff)) | (value[byteIndex] & mask[byteIndex]));
      label.append(element("small", "", before === null ? "-- → --" : `${before.toString(16).toUpperCase().padStart(2, "0")} → ${after.toString(16).toUpperCase().padStart(2, "0")}`));
      row.append(label);

      const bits = element("div", "byte-bits");
      for (let bitIndex = 7; bitIndex >= 0; bitIndex -= 1) {
        const index = byteIndex * 8 + bitIndex;
        const bitState = state.rawBitStates[index];
        const button = element("button", `bit-state ${bitState < 0 ? "pass" : bitState === 0 ? "zero" : "one"}`, bitState < 0 ? "·" : String(bitState));
        button.type = "button";
        button.dataset.rawBit = String(index);
        button.setAttribute("aria-label", `Byte ${byteIndex} bit ${bitIndex}: ${bitState < 0 ? "pass through" : `force ${bitState}`}`);
        button.title = `Bit ${bitIndex}: ${bitState < 0 ? "pass through" : `force ${bitState}`}`;
        bits.append(button);
      }
      row.append(bits);
      dom.rawBitGrid.append(row);
    }
  }

  function cycleRawBit(index) {
    const current = state.rawBitStates[index];
    state.rawBitStates[index] = current < 0 ? 1 : current === 1 ? 0 : -1;
    renderRawBitGrid();
  }

  function resetRawMask() {
    state.rawBitStates.fill(-1);
    state.rawBaseBytes = null;
    dom.rawFrameStatus.textContent = "No source frame loaded.";
    renderRawBitGrid();
  }

  function parseFrameBytes(data) {
    const bytes = String(data || "").trim().split(/[\s,]+/).filter(Boolean);
    if (!bytes.length || bytes.some((byte) => !/^[0-9a-f]{2}$/i.test(byte))) return null;
    const parsed = bytes.slice(0, 8).map((byte) => Number.parseInt(byte, 16));
    while (parsed.length < 8) parsed.push(0);
    return parsed;
  }

  function useLatestRawFrame() {
    const canId = parseCanId(dom.rawCanId.value);
    const direction = dom.rawDirection.value;
    const frame = state.frames.find((entry) => Number(entry.can_id) === canId && entry.direction === direction);
    const bytes = frame && parseFrameBytes(frame.data);
    if (!frame || !bytes) throw new Error(`No cached ${formatCanId(canId)} frame exists on ${direction}`);
    state.rawBaseBytes = bytes;
    dom.rawFrameStatus.textContent = `${formatCanId(canId)} · ${direction} · DLC ${frame.dlc ?? "--"}`;
    renderRawBitGrid();
  }

  async function stageRawMask() {
    const canId = parseCanId(dom.rawCanId.value);
    const { mask, value } = rawMaskBytes();
    if (mask.every((byte) => byte === 0)) throw new Error("Choose at least one bit to force to zero or one");
    const payload = await postForm("/api/rules/stage", {
      rule_kind: "RAW_MASK",
      can_id: formatCanId(canId),
      direction: dom.rawDirection.value,
      mask: bytesToHex(mask),
      value: bytesToHex(value),
      enabled: 1
    });
    state.ruleEpoch = Number(payload.rule_epoch || state.ruleEpoch);
    state.ruleView = "staging";
    await Promise.all([refreshRules(), pollStatus()]);
    showToast("Raw mask added to draft", "It can be applied live for this session; the package format does not persist raw masks yet.", "success");
  }

  const recipeDefinitions = {
    COUNTER: {
      description: "Write a field that advances once for every matching frame, then wraps at a defined value.",
      fields: [
        { name: "can_id", label: "CAN ID", value: "0x321" },
        { name: "direction", label: "Frame path", control: "select", value: "A_TO_B", options: [["A_TO_B", "Bus A → Bus B"], ["B_TO_A", "Bus B → Bus A"]] },
        { name: "start_bit", label: "Start bit", type: "number", value: "8", min: 0, max: 63 },
        { name: "length", label: "Length", type: "number", value: "4", min: 1, max: 32 },
        { name: "little_endian", label: "Bit order", control: "select", value: "1", options: [["1", "Intel / little-endian"], ["0", "Motorola / big-endian"]] },
        { name: "initial", label: "Initial value", type: "number", value: "0", min: 0 },
        { name: "step", label: "Step per frame", type: "number", value: "1", min: 0 },
        { name: "wrap_after", label: "Wrap after", type: "number", value: "15", min: 0 },
        { name: "wrap_to", label: "Wrap to", type: "number", value: "0", min: 0 }
      ]
    },
    SEQUENCE8: {
      description: "Cycle through one to sixteen byte values as matching frames pass through.",
      fields: [
        { name: "can_id", label: "CAN ID", value: "0x321" },
        { name: "direction", label: "Frame path", control: "select", value: "A_TO_B", options: [["A_TO_B", "Bus A → Bus B"], ["B_TO_A", "Bus B → Bus A"]] },
        { name: "start_bit", label: "Start bit", type: "number", value: "16", min: 0, max: 63 },
        { name: "length", label: "Length", type: "number", value: "8", min: 1, max: 8 },
        { name: "little_endian", label: "Bit order", control: "select", value: "1", options: [["1", "Intel / little-endian"], ["0", "Motorola / big-endian"]] },
        { name: "values", label: "Sequence values", value: "0x10 | 0x20 | 0x30", wide: true, help: "One to sixteen byte values separated by |, spaces, or commas." },
        { name: "initial_index", label: "Initial index", type: "number", value: "0", min: 0, max: 15 }
      ]
    },
    CHECKSUM_XOR: {
      description: "XOR an inclusive payload byte range and write the result into one target byte.",
      fields: [
        { name: "can_id", label: "CAN ID", value: "0x321" },
        { name: "direction", label: "Frame path", control: "select", value: "A_TO_B", options: [["A_TO_B", "Bus A → Bus B"], ["B_TO_A", "Bus B → Bus A"]] },
        { name: "target_byte", label: "Target byte", type: "number", value: "7", min: 0, max: 7 },
        { name: "start_byte", label: "Range starts at byte", type: "number", value: "0", min: 0, max: 7 },
        { name: "end_byte", label: "Range ends at byte", type: "number", value: "6", min: 0, max: 7 },
        { name: "seed", label: "XOR seed", value: "0x00" },
        { name: "enabled", label: "Initial state", control: "select", value: "1", options: [["1", "Enabled"], ["0", "Disabled"]] }
      ]
    },
    CHECKSUM_CRC8_AUTOSAR: {
      description: "Apply SignalScope's specialized CRC-8/AUTOSAR data-ID post-processor.",
      notice: "Use this only when captures or protocol documentation prove the exact CRC profile, byte range, counter byte, and 16-byte data-ID table.",
      fields: [
        { name: "can_id", label: "CAN ID", value: "0x321" },
        { name: "direction", label: "Frame path", control: "select", value: "A_TO_B", options: [["A_TO_B", "Bus A → Bus B"], ["B_TO_A", "Bus B → Bus A"]] },
        { name: "target_byte", label: "CRC target byte", type: "number", value: "0", min: 0, max: 7 },
        { name: "counter_byte", label: "Counter byte", type: "number", value: "1", min: 0, max: 7 },
        { name: "start_byte", label: "Range starts at byte", type: "number", value: "1", min: 0, max: 7 },
        { name: "end_byte", label: "Range ends at byte", type: "number", value: "7", min: 0, max: 7 },
        { name: "data_ids", label: "16 data-ID bytes", value: "00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F", wide: true, help: "Exactly sixteen hexadecimal bytes in counter-nibble order. Prefix an explicit decimal value with 0d." },
        { name: "enabled", label: "Initial state", control: "select", value: "1", options: [["1", "Enabled"], ["0", "Disabled"]] }
      ]
    },
    SOURCE_INT: {
      description: "Map a value published by an installed native application into a CAN field.",
      notice: "The source name must be registered by your app's C++ extension. BIND_TABLE, BIND_OVERRIDE, and BIND_ACTIVE directives affect compatible rows below their position in the package.",
      fields: [
        { name: "can_id", label: "CAN ID", value: "0x321" },
        { name: "direction", label: "Frame path", control: "select", value: "A_TO_B", options: [["A_TO_B", "Bus A → Bus B"], ["B_TO_A", "Bus B → Bus A"]] },
        { name: "start_bit", label: "Start bit", type: "number", value: "0", min: 0, max: 63 },
        { name: "length", label: "Length", type: "number", value: "8", min: 1, max: 32 },
        { name: "little_endian", label: "Bit order", control: "select", value: "1", options: [["1", "Intel / little-endian"], ["0", "Motorola / big-endian"]] },
        { name: "value_source", label: "Runtime source name", value: "app_value" },
        { name: "source_gain", label: "Source gain", type: "number", step: "any", value: "1" },
        { name: "source_offset", label: "Source offset", type: "number", step: "any", value: "0" },
        { name: "output_scale", label: "Output scale", type: "number", step: "any", value: "1" },
        { name: "output_offset", label: "Output offset", type: "number", step: "any", value: "0" },
        { name: "zero_threshold", label: "Zero threshold", type: "number", step: "any", value: "0" },
        { name: "zero_output", label: "Zero output", type: "number", value: "0", min: 0 },
        { name: "full_threshold", label: "Full threshold", type: "number", step: "any", value: "255" },
        { name: "full_output", label: "Full output", type: "number", value: "255", min: 0 }
      ]
    },
    SOURCE_SELECT_INT: {
      description: "Use an application-published mode to choose a direct value or a mode-specific scale.",
      notice: "Both source names must be registered by your app's C++ extension. Selector entries use D for direct output or S for scale plus full output. Package bindings affect compatible rows below their position.",
      fields: [
        { name: "can_id", label: "CAN ID", value: "0x321" },
        { name: "direction", label: "Frame path", control: "select", value: "A_TO_B", options: [["A_TO_B", "Bus A → Bus B"], ["B_TO_A", "Bus B → Bus A"]] },
        { name: "start_bit", label: "Start bit", type: "number", value: "0", min: 0, max: 63 },
        { name: "length", label: "Length", type: "number", value: "8", min: 1, max: 32 },
        { name: "little_endian", label: "Bit order", control: "select", value: "1", options: [["1", "Intel / little-endian"], ["0", "Motorola / big-endian"]] },
        { name: "value_source", label: "Runtime source name", value: "requested_value" },
        { name: "source_gain", label: "Source gain", type: "number", step: "any", value: "1" },
        { name: "source_offset", label: "Source offset", type: "number", step: "any", value: "0" },
        { name: "output_offset", label: "Output offset", type: "number", step: "any", value: "0" },
        { name: "zero_threshold", label: "Zero threshold", type: "number", step: "any", value: "0" },
        { name: "zero_output", label: "Zero output", type: "number", value: "0", min: 0 },
        { name: "full_threshold", label: "Full threshold", type: "number", step: "any", value: "100" },
        { name: "selector_source", label: "Mode source name", value: "mode" },
        { name: "entries", label: "Selector entries", value: "0:D:0 | 1:S:1:100 | 2:S:2:200", wide: true, help: "Examples: 0:D:0 or 1:S:1.5:200. Omitted modes disable the rule." }
      ]
    },
    BIND_TABLE: {
      description: "Attach an application-published lookup table to later app-driven rows using this source.",
      notice: "Bindings are positional. This directive is inserted before the first matching app-driven row and remains in effect until a later BIND_TABLE changes it. Move or repeat it in the package source for narrower scopes. Your app code must publish both names.",
      fields: [
        { name: "value_source", label: "Runtime source name", value: "app_value" },
        { name: "table_name", label: "Runtime table name", value: "app_table" }
      ]
    },
    BIND_OVERRIDE: {
      description: "Choose a second published value whenever an application-published enable source is active.",
      notice: "Bindings are positional. This directive is inserted before the first matching app-driven row and remains in effect until a later BIND_OVERRIDE changes it. Move or repeat it in the package source for narrower scopes. Your app code must publish all three names.",
      fields: [
        { name: "value_source", label: "Primary source", value: "app_value" },
        { name: "active_source", label: "Override-enable source", value: "override_active" },
        { name: "override_source", label: "Override-value source", value: "override_value" }
      ]
    },
    BIND_ACTIVE: {
      description: "Run generated rules only while a published selector is in one of these modes.",
      notice: "This directive is inserted before the first rule and gates compatible rows below it until a later BIND_ACTIVE replaces the gate for following rows. Mode-selecting source rules keep their own selector. Your app code must publish a value from 0 through 15.",
      fields: [
        { name: "selector_source", label: "Mode source name", value: "mode" },
        { name: "states", label: "Active modes", value: "0 | 1 | 4", help: "One or more unique values from 0 through 15." }
      ]
    }
  };

  function recipeControl(field) {
    let control;
    if (field.control === "select") {
      control = element("select");
      field.options.forEach(([value, label]) => {
        const option = element("option", "", label);
        option.value = value;
        control.append(option);
      });
    } else {
      control = element("input");
      control.type = field.type || "text";
      if (field.min !== undefined) control.min = String(field.min);
      if (field.max !== undefined) control.max = String(field.max);
      if (field.step !== undefined) control.step = String(field.step);
    }
    control.value = field.value;
    control.dataset.recipeField = field.name;
    return control;
  }

  function renderRecipeFields() {
    const definition = recipeDefinitions[dom.recipeType.value];
    dom.recipeDescription.textContent = definition.description;
    dom.recipeNotice.innerHTML = "";
    dom.recipeNotice.append(element("strong", "", "Know what feeds the rule. "), document.createTextNode(definition.notice || "This row is package source. Installing the package validates it, replaces the live table, and stores it for boot."));
    dom.recipeFields.replaceChildren();
    definition.fields.forEach((field) => {
      const label = element("label", `field${field.wide ? " span-2" : ""}`);
      label.append(element("span", "", field.label));
      label.append(recipeControl(field));
      if (field.help) label.append(element("small", "field-help", field.help));
      dom.recipeFields.append(label);
    });
    if (state.selectedSignal) seedRecipeTargetFromSignal(state.selectedSignal);
    updateRecipePreview();
  }

  function seedRecipeTargetFromSignal(signal) {
    const values = {
      can_id: signal.canIdHex || formatCanId(signal.canId),
      direction: signal.direction || "A_TO_B",
      start_bit: signal.startBit,
      length: signal.length,
      little_endian: signal.littleEndian ? 1 : 0
    };
    Object.entries(values).forEach(([name, value]) => {
      const input = dom.recipeFields.querySelector(`[data-recipe-field="${name}"]`);
      if (input) input.value = String(value);
    });
    updateRecipePreview();
  }

  function recipeValues() {
    const values = {};
    dom.recipeFields.querySelectorAll("[data-recipe-field]").forEach((control) => {
      values[control.dataset.recipeField] = control.value.trim();
    });
    return values;
  }

  function integerField(values, name, label, minimum = 0, maximum = 0xffffffff) {
    const text = values[name] || "";
    if (!/^(?:0x[0-9a-f]+|[0-9]+)$/i.test(text)) throw new Error(`${label} must be a whole number`);
    const value = Number(text);
    if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
      throw new Error(`${label} must be between ${minimum} and ${maximum}`);
    }
    return value;
  }

  function floatField(values, name, label) {
    const text = String(values[name] ?? "").trim();
    if (!text) throw new Error(`${label} is required`);
    const value = Number(text);
    if (!Number.isFinite(value)) throw new Error(`${label} must be a number`);
    return String(value);
  }

  function sourceName(values, name, label) {
    const value = String(values[name] || "").trim();
    if (!/^[A-Za-z_][A-Za-z0-9_.-]{0,30}$/.test(value)) {
      throw new Error(`${label} must start with a letter or underscore and use only letters, numbers, _, ., or - (31 characters maximum)`);
    }
    return value;
  }

  function commonRecipeFields(values, maximumLength) {
    const canId = formatCanId(parseCanId(values.can_id));
    const direction = values.direction;
    if (direction !== "A_TO_B" && direction !== "B_TO_A") throw new Error("Choose a valid frame path");
    const startBit = integerField(values, "start_bit", "Start bit", 0, 63);
    const length = integerField(values, "length", "Length", 1, maximumLength);
    const littleEndian = values.little_endian === "0" ? 0 : 1;
    validateBitRange(startBit, length, littleEndian === 1, maximumLength);
    return { canId, direction, startBit, length, littleEndian };
  }

  function splitList(text) {
    return String(text).trim().split(/[|,\s]+/).filter(Boolean);
  }

  function buildRecipeRow() {
    const type = dom.recipeType.value;
    const values = recipeValues();

    if (type === "COUNTER") {
      const common = commonRecipeFields(values, 32);
      const maximum = Number((1n << BigInt(common.length)) - 1n);
      const initial = integerField(values, "initial", "Initial value", 0, maximum);
      const step = integerField(values, "step", "Step", 0, 0xffffffff);
      const wrapAfter = integerField(values, "wrap_after", "Wrap after", 0, maximum);
      const wrapTo = integerField(values, "wrap_to", "Wrap to", 0, maximum);
      return `COUNTER,${common.canId},${common.direction},${common.startBit},${common.length},${common.littleEndian},${initial},${step},${wrapAfter},${wrapTo}`;
    }

    if (type === "SEQUENCE8") {
      const common = commonRecipeFields(values, 8);
      const maximum = (1 << common.length) - 1;
      const sequence = splitList(values.values).map((value, index) => {
        const entry = { value };
        return integerField(entry, "value", `Sequence value ${index + 1}`, 0, maximum);
      });
      if (!sequence.length || sequence.length > 16) throw new Error("Sequence must contain 1–16 values");
      const initialIndex = integerField(values, "initial_index", "Initial index", 0, sequence.length - 1);
      return `SEQUENCE8,${common.canId},${common.direction},${common.startBit},${common.length},${common.littleEndian},${sequence.join("|")},${initialIndex}`;
    }

    if (type === "CHECKSUM_XOR") {
      const canId = formatCanId(parseCanId(values.can_id));
      const target = integerField(values, "target_byte", "Target byte", 0, 7);
      const start = integerField(values, "start_byte", "Start byte", 0, 7);
      const end = integerField(values, "end_byte", "End byte", start, 7);
      const seed = integerField(values, "seed", "Seed", 0, 255);
      return `CHECKSUM_XOR,${canId},${values.direction},${target},${start},${end},0x${seed.toString(16).toUpperCase().padStart(2, "0")},${values.enabled === "0" ? 0 : 1}`;
    }

    if (type === "CHECKSUM_CRC8_AUTOSAR") {
      const canId = formatCanId(parseCanId(values.can_id));
      const target = integerField(values, "target_byte", "Target byte", 0, 7);
      const counter = integerField(values, "counter_byte", "Counter byte", 0, 7);
      const start = integerField(values, "start_byte", "Start byte", 0, 7);
      const end = integerField(values, "end_byte", "End byte", start, 7);
      const dataIds = splitList(values.data_ids).map((value, index) => {
        // Unprefixed two-digit values are intentionally interpreted as hex in
        // this byte-oriented field; explicit decimal remains available as 0dNN.
        const normalized = /^0d[0-9]+$/i.test(value)
          ? value.slice(2)
          : /^0x/i.test(value) ? value : `0x${value}`;
        const entry = { value: normalized };
        return integerField(entry, "value", `Data ID ${index}`, 0, 255);
      });
      if (dataIds.length !== 16) throw new Error("Enter exactly 16 data-ID bytes");
      const encoded = dataIds.map((value) => `0x${value.toString(16).toUpperCase().padStart(2, "0")}`).join("|");
      return `CHECKSUM_CRC8_AUTOSAR,${canId},${values.direction},${target},${counter},${start},${end},${encoded},${values.enabled === "0" ? 0 : 1}`;
    }

    if (type === "SOURCE_INT") {
      const common = commonRecipeFields(values, 32);
      const source = sourceName(values, "value_source", "Runtime source name");
      const zeroOutput = integerField(values, "zero_output", "Zero output", 0, 0xffffffff);
      const fullOutput = integerField(values, "full_output", "Full output", 0, 0xffffffff);
      return `SOURCE_INT,${common.canId},${common.direction},${common.startBit},${common.length},${common.littleEndian},${source},${floatField(values, "source_gain", "Source gain")},${floatField(values, "source_offset", "Source offset")},${floatField(values, "output_scale", "Output scale")},${floatField(values, "output_offset", "Output offset")},${floatField(values, "zero_threshold", "Zero threshold")},${zeroOutput},${floatField(values, "full_threshold", "Full threshold")},${fullOutput}`;
    }

    if (type === "SOURCE_SELECT_INT") {
      const common = commonRecipeFields(values, 32);
      const source = sourceName(values, "value_source", "Runtime source name");
      const selector = sourceName(values, "selector_source", "Mode source name");
      const zeroOutput = integerField(values, "zero_output", "Zero output", 0, 0xffffffff);
      const seen = new Set();
      const entries = String(values.entries).split("|").map((entry) => entry.trim()).filter(Boolean).map((entry) => {
        const parts = entry.split(":").map((part) => part.trim());
        const selectorValue = Number(parts[0]);
        if (!Number.isInteger(selectorValue) || selectorValue < 0 || selectorValue > 15 || seen.has(selectorValue)) {
          throw new Error("Selector entries need unique mode values from 0 through 15");
        }
        seen.add(selectorValue);
        if (parts[1] === "D" && parts.length === 3) {
          const direct = integerField({ value: parts[2] }, "value", `Direct output for mode ${selectorValue}`, 0, 0xffffffff);
          return `${selectorValue}:D:${direct}`;
        }
        if (parts[1] === "S" && parts.length === 4) {
          const scale = floatField({ value: parts[2] }, "value", `Scale for mode ${selectorValue}`);
          const full = integerField({ value: parts[3] }, "value", `Full output for mode ${selectorValue}`, 0, 0xffffffff);
          return `${selectorValue}:S:${scale}:${full}`;
        }
        throw new Error("Use selector entries such as 0:D:0 or 1:S:1.5:200");
      });
      if (!entries.length) throw new Error("Add at least one selector entry");
      return `SOURCE_SELECT_INT,${common.canId},${common.direction},${common.startBit},${common.length},${common.littleEndian},${source},${floatField(values, "source_gain", "Source gain")},${floatField(values, "source_offset", "Source offset")},${floatField(values, "output_offset", "Output offset")},${floatField(values, "zero_threshold", "Zero threshold")},${zeroOutput},${floatField(values, "full_threshold", "Full threshold")},${selector},${entries.join("|")}`;
    }

    if (type === "BIND_TABLE") {
      return `BIND_TABLE,${sourceName(values, "value_source", "Runtime source name")},${sourceName(values, "table_name", "Runtime table name")}`;
    }
    if (type === "BIND_OVERRIDE") {
      return `BIND_OVERRIDE,${sourceName(values, "value_source", "Primary source")},${sourceName(values, "active_source", "Override-enable source")},${sourceName(values, "override_source", "Override-value source")}`;
    }
    if (type === "BIND_ACTIVE") {
      const selector = sourceName(values, "selector_source", "Mode source name");
      const states = [...new Set(splitList(values.states).map((value, index) => {
        const entry = { value };
        return integerField(entry, "value", `Active mode ${index + 1}`, 0, 15);
      }))].sort((a, b) => a - b);
      if (!states.length) throw new Error("Add at least one active mode");
      return `BIND_ACTIVE,${selector},${states.join("|")}`;
    }
    throw new Error("Choose a supported recipe type");
  }

  function updateRecipePreview() {
    try {
      dom.recipePreview.textContent = buildRecipeRow();
      dom.recipePreview.classList.remove("invalid");
      dom.addRecipeButton.disabled = false;
    } catch (error) {
      dom.recipePreview.textContent = error.message;
      dom.recipePreview.classList.add("invalid");
      dom.addRecipeButton.disabled = true;
    }
  }

  function addRecipeToPackage() {
    const line = buildRecipeRow();
    upsertPackageLine(line);
    dom.packageEditor.open = true;
    showToast("Recipe row added", `${dom.recipeType.options[dom.recipeType.selectedIndex].text} is ready in the package source.`, "success");
  }

  // JSON numbers stop being exact above 53 bits in a browser. The framework
  // therefore publishes a decimal string beside the convenient numeric value;
  // keep that string intact when displaying or rebuilding a rule package.
  function exactRuleValue(rule) {
    if (typeof rule.replace_value_text === "string" && rule.replace_value_text.trim()) {
      return rule.replace_value_text.trim();
    }
    return String(rule.replace_value ?? 0);
  }

  function exactRuntimeValue(rule) {
    if (typeof rule.runtime_value_text === "string" && rule.runtime_value_text.trim()) {
      return rule.runtime_value_text.trim();
    }
    if (rule.runtime_value !== undefined) return String(rule.runtime_value);
    return exactRuleValue(rule);
  }

  function renderRules() {
    const viewingDraft = state.ruleView === "staging";
    const rules = viewingDraft ? state.candidateRules : state.activeRules;
    dom.ruleRows.replaceChildren();
    if (!rules.length) {
      const row = element("tr");
      const cell = element("td", "empty", viewingDraft ? "No draft rules." : "No live rules.");
      cell.colSpan = 4;
      row.append(cell);
      dom.ruleRows.append(row);
      return;
    }

    rules.forEach((rule) => {
      const row = element("tr");
      const idCell = element("td");
      idCell.append(element("code", "", `#${rule.rule_id}`));
      const target = element("td");
      let targetText = `${formatCanId(rule.can_id)} bit ${rule.start_bit}:${rule.length}`;
      if (rule.kind === "RAW_MASK") targetText = `${formatCanId(rule.can_id)} raw bits`;
      if (String(rule.kind).startsWith("CHECKSUM")) targetText = `${formatCanId(rule.can_id)} checksum`;
      target.append(element("span", "signal-name"));
      target.firstChild.append(
        element("strong", "", targetText),
        element("small", "", `${rule.direction} · ${rule.kind}`)
      );

      const valueCell = element("td", "rule-value-control");
      const manualDynamic = rule.kind === "BIT_RANGE" && rule.dynamic &&
        rule.manual_dynamic !== false && !String(rule.value_source || "").trim();
      const counterControl = rule.kind === "COUNTER";
      const sequenceControl = rule.kind === "SEQUENCE8";
      const runtimeControl = manualDynamic || counterControl || sequenceControl;
      if (runtimeControl && viewingDraft) {
        const valueInput = element("input");
        valueInput.type = "number";
        valueInput.min = "0";
        if (sequenceControl && Number(rule.sequence_count) > 0) {
          valueInput.max = String(Number(rule.sequence_count) - 1);
        } else if (Number(rule.length) > 0 && Number(rule.length) < 32) {
          valueInput.max = String((2 ** Number(rule.length)) - 1);
        }
        valueInput.value = exactRuntimeValue(rule);
        valueInput.dataset.ruleValue = String(rule.rule_id);
        const runtimeLabel = counterControl ? "Counter state" : sequenceControl ? "Sequence index" : "Raw value";
        valueInput.setAttribute("aria-label", `${runtimeLabel} for rule ${rule.rule_id}`);
        const setButton = element("button", "button quiet", "Set");
        setButton.type = "button";
        setButton.dataset.setRule = String(rule.rule_id);
        valueCell.append(valueInput, setButton, element("small", "field-help", runtimeLabel));
      } else if (rule.kind === "BIT_RANGE") {
        const value = element("span", "signal-name");
        const valueKind = rule.dynamic && !manualDynamic
          ? `App source: ${rule.value_source || "registered runtime value"}`
          : rule.dynamic ? "Manual live-adjustable raw value" : "Fixed raw value";
        value.append(
          element("code", "", exactRuleValue(rule)),
          element("small", "", valueKind)
        );
        valueCell.append(value);
      } else if (counterControl || sequenceControl) {
        const value = element("span", "signal-name");
        value.append(
          element("code", "", exactRuntimeValue(rule)),
          element("small", "", counterControl ? "Current counter state" : "Current sequence index")
        );
        valueCell.append(value);
      } else if (rule.kind === "RAW_MASK") {
        const value = element("span", "signal-name raw-rule-value");
        value.append(
          element("code", "", rule.mask || "--"),
          element("small", "", `value ${rule.value || "--"}`)
        );
        valueCell.append(value);
      } else {
        const value = element("span", "signal-name");
        value.append(
          element("strong", "", String(rule.kind || "advanced").replaceAll("_", " ")),
          element("small", "", "Full parameters live in package source")
        );
        valueCell.append(value);
      }

      const stateCell = element("td", "rule-state-control");
      const enabled = typeof rule.enabled === "boolean" ? rule.enabled : Boolean(rule.active);
      const candidateLabel = state.candidateDirty ? "Pending" : "Candidate";
      stateCell.append(element("span", `badge ${viewingDraft && state.candidateDirty ? "warning" : !viewingDraft && enabled ? "success" : "neutral"}`, viewingDraft ? candidateLabel : enabled ? "Live" : "Disabled"));
      if (viewingDraft) {
        const toggle = element("label", "rule-switch");
        const checkbox = element("input");
        checkbox.type = "checkbox";
        // `active` is retained by the API as a compatibility alias for a
        // rule's enabled flag. It does not identify the table view.
        checkbox.checked = enabled;
        checkbox.dataset.toggleRule = String(rule.rule_id);
        checkbox.setAttribute("aria-label", `Enable rule ${rule.rule_id}`);
        toggle.append(checkbox, element("span", "", "Include on Apply"));
        stateCell.append(toggle, element("small", "field-help", "RAM draft only"));
      }
      row.append(idCell, target, valueCell, stateCell);
      dom.ruleRows.append(row);
    });
  }

  function setRuleView(view) {
    if (view !== "staging" && view !== "active") return;
    state.ruleView = view;
    const viewingDraft = view === "staging";
    document.querySelectorAll("[data-rule-view]").forEach((button) => {
      const selected = button.dataset.ruleView === view;
      button.classList.toggle("active", selected);
      button.setAttribute("aria-selected", selected ? "true" : "false");
    });
    const hasPendingChanges = viewingDraft && state.candidateDirty;
    setBadge(
      dom.ruleViewBadge,
      viewingDraft ? (hasPendingChanges ? "Draft changes" : "Candidate copy") : "Live now",
      hasPendingChanges ? "warning" : viewingDraft ? "neutral" : "success"
    );
    dom.ruleTableHelp.textContent = viewingDraft
      ? hasPendingChanges
        ? "These candidate changes do not affect forwarded frames until Apply. Include toggles affect this RAM draft only."
        : "This candidate copy matches the live table. Stage or edit a rule to begin a new RAM draft."
      : "This read-only view is the rule table affecting forwarded frames now.";
    dom.commitRulesButton.hidden = !hasPendingChanges;
    dom.revertRulesButton.hidden = !hasPendingChanges;
    dom.clearDraftButton.hidden = !viewingDraft;
    renderRules();
  }

  async function refreshRules() {
    const [candidate, active] = await Promise.all([
      api("/api/rules?view=staging", { quiet: true }),
      api("/api/rules", { quiet: true })
    ]);
    state.candidateRules = candidate.rules || [];
    state.activeRules = active.rules || [];
    state.candidateDirty = Boolean(candidate.candidate_dirty);
    state.ruleEpoch = Number(candidate.rule_epoch || active.rule_epoch || 0);
    if (!state.rulesInitialized) {
      state.ruleView = state.candidateDirty || !state.activeRules.length ? "staging" : "active";
      state.rulesInitialized = true;
    }
    setRuleView(state.ruleView);
  }

  async function setRuleValue(ruleId, value) {
    const rule = state.candidateRules.find((entry) => Number(entry.rule_id) === Number(ruleId));
    const runtimeKind = String(rule?.runtime_value_kind || "");
    const manualDynamic = runtimeKind === "raw" || Boolean(rule && rule.kind === "BIT_RANGE" && rule.dynamic &&
      rule.manual_dynamic !== false && !String(rule.value_source || "").trim());
    const counter = runtimeKind === "counter_state" || rule?.kind === "COUNTER";
    const sequence = runtimeKind === "sequence_index" || rule?.kind === "SEQUENCE8";
    if (!rule || (!manualDynamic && !counter && !sequence)) {
      throw new Error("Choose a candidate rule with a runtime value control");
    }
    const textValue = String(value).trim();
    const valueLabel = counter ? "Counter state" : sequence ? "Sequence index" : "Raw value";
    if (!/^\d+$/.test(textValue)) throw new Error(`${valueLabel} must be a non-negative whole number`);
    const raw = BigInt(textValue);
    if (sequence) {
      const count = Number(rule.sequence_count || 0);
      if (!Number.isInteger(count) || count < 1 || raw >= BigInt(count)) {
        throw new Error(`Sequence index must be between 0 and ${Math.max(0, count - 1)}`);
      }
    } else {
      const width = Math.min(32, Number(rule.length || 0));
      if (width < 1 || raw >= (1n << BigInt(width))) {
        throw new Error(`${valueLabel} must fit in ${width} bits`);
      }
    }
    await postForm("/api/rules/value", {
      rule_id: ruleId,
      rule_epoch: state.ruleEpoch,
      view: "staging",
      value: raw.toString()
    });
    await refreshRules();
    showToast(`${valueLabel} updated`, `Rule #${ruleId} will start at ${raw.toString()} after you apply the draft.`, "success");
  }

  async function setRuleEnabled(ruleId, enabled) {
    await postForm("/api/rules/enable", {
      rule_id: ruleId,
      rule_epoch: state.ruleEpoch,
      view: "staging",
      enabled: enabled ? 1 : 0
    });
    await refreshRules();
    showToast(
      enabled ? "Rule enabled in draft" : "Rule disabled in draft",
      `Rule #${ruleId} will change after you apply the draft.`,
      "success"
    );
  }

  async function ruleAction(action, successMessage) {
    const payload = await postText("/api/rules", action);
    state.ruleEpoch = Number(payload.rule_epoch || state.ruleEpoch);
    await Promise.all([refreshRules(), pollStatus()]);
    showToast(successMessage, "The controller accepted the new rule table.", "success");
  }

  function packageFromVisibleRules() {
    const unsupported = state.candidateRules.some((rule) => rule.kind !== "BIT_RANGE" || rule.dynamic);
    if (unsupported) {
      throw new Error("This draft contains RAM-only or advanced rules that cannot be reconstructed from the list response. Keep or generate their original .ssrules text.");
    }
    const rows = state.candidateRules.map((rule) =>
      `STATIC,${formatCanId(rule.can_id)},${rule.direction},${rule.start_bit},${rule.length},${rule.little_endian === false ? 0 : 1},${exactRuleValue(rule)}`
    );
    if (!rows.length) throw new Error("Create or paste at least one rule first");
    return `# SignalScope rule package\n# kind,can_id,direction,start_bit,length,little_endian,raw_value\n${rows.join("\n")}\n`;
  }

  function currentPackageText() {
    const current = dom.packageText.value.trim();
    if (current) return `${current}\n`;
    const generated = packageFromVisibleRules();
    dom.packageText.value = generated;
    return generated;
  }

  async function savePackage() {
    const text = currentPackageText();
    try {
      const result = await postText(`/api/rules/package?path=${encodeURIComponent(ACTIVE_PACKAGE_PATH)}`, text);
      state.ruleEpoch = Number(result.rule_epoch || state.ruleEpoch);
      await Promise.all([refreshRules(), pollStatus()]);
      setRuleView("active");
      if (result.runtime_bindings_verified === false) {
        showToast(
          "Package parsed in preview",
          `${result.count ?? state.activeRules.length} rows are simulated. App-owned sources and tables can only be verified by firmware with that native extension installed.`,
          "warning"
        );
      } else {
        showToast("Startup package installed", `${result.count ?? state.activeRules.length} validated rules are live now and will return after reboot.`, "success");
      }
    } catch (error) {
      // The transactional loader preserves the live table but discards its
      // partially parsed candidate on rejection. Refresh so a stale-looking
      // draft can never be applied later as an unintended empty table.
      await Promise.allSettled([refreshRules(), pollStatus()]);
      throw error;
    }
  }

  async function loadExistingPackage() {
    try {
      const text = await api("/api/rules/package", { expect: "text", quiet: true });
      if (text && typeof text === "string") dom.packageText.value = text;
    } catch (error) {
      if (error.status !== 404) appendRequestLog("GET", "/api/rules/package", error.status || "ERR", 0, error.message);
    }
  }

  function downloadPackage() {
    try {
      const text = currentPackageText();
      const blob = new Blob([text], { type: "text/plain;charset=utf-8" });
      const url = URL.createObjectURL(blob);
      const link = element("a");
      link.href = url;
      link.download = "signalscope-rules.ssrules";
      document.body.append(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      showToast("Package downloaded", "Keep it beside the app that uses it.", "success");
    } catch (error) {
      showToast("Nothing to download", error.message, "error");
    }
  }

  async function loadSelectedDbc() {
    if (!state.dbcText) throw new Error("Choose a DBC file first");
    setBadge(dom.dbcStatus, "Validating…", "warning");
    const payload = await postText("/api/dbc", state.dbcText, { timeout: 15000 });
    state.catalogSignals = [];
    state.catalogOffset = 0;
    resetRuleBuilder();
    await Promise.all([pollStatus(), refreshRules(), refreshCatalog(true)]);
    setBadge(dom.dbcStatus, `${payload.signals || 0} signals`, "success");
    showToast("DBC loaded", `${payload.messages || 0} messages and ${payload.signals || 0} signals are ready. Rule source remains in the editor, but no rules are live.`, "success");
  }

  async function autoloadDbc() {
    const payload = await postForm("/api/dbc/autoload", {});
    state.catalogSignals = [];
    state.catalogOffset = 0;
    resetRuleBuilder();
    await Promise.all([pollStatus(), refreshRules(), refreshCatalog(true)]);
    showToast("Installed DBC reloaded", `${payload.signals || 0} signals are ready. Rule source remains in the editor, but no rules are live.`, "success");
  }

  async function acceptDbcFile(file) {
    if (!file) return;
    if (!file.name.toLowerCase().endsWith(".dbc")) {
      showToast("Choose a .dbc file", "SignalScope expects a plain-text DBC dictionary.", "error");
      return;
    }
    try {
      state.dbcText = await file.text();
      state.dbcFilename = file.name;
      dom.dbcFilename.textContent = `${file.name} · ${formatNumber(file.size / 1024, 1)} KiB`;
      dom.loadDbcButton.disabled = !state.dbcText;
      setBadge(dom.dbcStatus, "Ready to load", "neutral");
    } catch (error) {
      showToast("Could not read that file", error.message, "error");
    }
  }

  function renderLogStatus(status) {
    const phase = String(status.phase || status.state || status.status || "idle").replaceAll("_", " ");
    const active = Boolean(status.active || status.recording || ["recording", "running", "stopping"].includes(phase.toLowerCase()));
    const count = status.frames ?? status.frame_count ?? status.records ?? status.records_written;
    const size = status.bytes ?? status.bytes_written ?? status.file_size;
    const details = [];
    if (count !== undefined) details.push(`${formatNumber(count, 0)} frames`);
    if (size !== undefined) details.push(`${formatNumber(Number(size) / 1024, 1)} KiB`);
    dom.logStatus.textContent = `Recorder ${phase}.${details.length ? ` ${details.join(" · ")}.` : ""}`;
    dom.startLogButton.disabled = active;
    dom.stopLogButton.disabled = !active;
  }

  async function pollLog() {
    try {
      renderLogStatus(await api("/api/log", { quiet: true }));
    } catch (error) {
      dom.logStatus.textContent = "Recorder status unavailable.";
    }
  }

  async function logAction(action) {
    const payload = action === "start"
      ? await postForm("/api/log", { action, scope: "physical", durable: 1 })
      : await postForm("/api/log", { action });
    renderLogStatus(payload);
    showToast(
      action === "start" ? "Recording started" : "Recording stop requested",
      action === "start" ? "Physical bus traffic is being captured." : "Wait for the recorder to finish saving before download.",
      "success"
    );
  }

  function validateReplayData(text) {
    const bytes = String(text).trim().split(/[\s,]+/).filter(Boolean);
    if (bytes.length !== 8 || bytes.some((byte) => !/^[0-9a-f]{2}$/i.test(byte))) {
      throw new Error("Enter exactly eight hexadecimal bytes, for example 00 01 02 03 04 05 06 07");
    }
    return bytes.map((byte) => byte.toUpperCase()).join(" ");
  }

  async function dryRunFrame() {
    const canId = parseCanId(dom.replayCanId.value);
    const data = validateReplayData(dom.replayData.value);
    const payload = await postForm("/api/replay/send", {
      can_id: formatCanId(canId),
      direction: "A_TO_B",
      dlc: 8,
      data,
      repeat: 1,
      interval_us: 0,
      start_delay_us: 0,
      auto_start: 1,
      dry_run: 1
    });
    if (!payload.dry_run) throw new Error("Controller did not confirm dry-run mode");
    showToast("Dry-run queued", "The frame exercised the software pipeline without touching physical CAN.", "success");
  }

  async function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text);
      return;
    }
    const helper = element("textarea");
    helper.value = text;
    helper.style.position = "fixed";
    helper.style.opacity = "0";
    document.body.append(helper);
    helper.select();
    const copied = document.execCommand("copy");
    helper.remove();
    if (!copied) throw new Error("Clipboard access is unavailable in this browser");
  }

  function applyTheme(theme) {
    document.documentElement.dataset.theme = theme;
    dom.themeToggle.textContent = theme === "dark" ? "☀" : "☾";
    dom.themeToggle.setAttribute("aria-label", theme === "dark" ? "Switch to light theme" : "Switch to dark theme");
  }

  function initializeTheme() {
    let theme = "";
    try {
      theme = localStorage.getItem("signalscope-theme") || "";
    } catch (error) {
      // Storage is optional on locked-down browsers.
    }
    if (theme !== "dark" && theme !== "light") {
      theme = window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
    }
    applyTheme(theme);
  }

  function debounce(callback, delay) {
    let timer = 0;
    return (...args) => {
      clearTimeout(timer);
      timer = setTimeout(() => callback(...args), delay);
    };
  }

  async function pollOnce(name, operation) {
    if (state.pollBusy.has(name) || document.hidden) return;
    state.pollBusy.add(name);
    try {
      await operation();
    } catch (error) {
      // Periodic failures are reflected by the connection and status labels.
    } finally {
      state.pollBusy.delete(name);
    }
  }

  function bindEvents() {
    dom.themeToggle.addEventListener("click", () => {
      const theme = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
      applyTheme(theme);
      try {
        localStorage.setItem("signalscope-theme", theme);
      } catch (error) {
        // Theme still works for this session when storage is unavailable.
      }
    });

    dom.dbcFile.addEventListener("change", () => void acceptDbcFile(dom.dbcFile.files[0]));
    const dropzone = dom.dbcFile.closest(".dropzone");
    ["dragenter", "dragover"].forEach((name) => dropzone.addEventListener(name, (event) => {
      event.preventDefault();
      dropzone.classList.add("dragging");
    }));
    ["dragleave", "drop"].forEach((name) => dropzone.addEventListener(name, (event) => {
      event.preventDefault();
      dropzone.classList.remove("dragging");
    }));
    dropzone.addEventListener("drop", (event) => void acceptDbcFile(event.dataTransfer.files[0]));
    dom.loadDbcButton.addEventListener("click", () => void runAction(dom.loadDbcButton, loadSelectedDbc).catch(() => {}));
    dom.autoloadDbcButton.addEventListener("click", () => void runAction(dom.autoloadDbcButton, autoloadDbc).catch(() => {}));

    const search = debounce(() => void pollOnce("catalog", () => refreshCatalog(true)), 280);
    dom.signalSearch.addEventListener("input", search);
    dom.signalRefresh.addEventListener("click", () => void runAction(dom.signalRefresh, () => refreshCatalog(true)).catch(() => {}));
    dom.loadMoreSignals.addEventListener("click", () => void runAction(dom.loadMoreSignals, () => refreshCatalog(false)).catch(() => {}));
    dom.signalRows.addEventListener("click", (event) => {
      const button = event.target.closest("[data-use-signal]");
      if (!button) return;
      const signal = state.catalogSignals.find((entry) => Number(entry.index) === Number(button.dataset.useSignal));
      if (signal) selectSignal(signal);
    });

    dom.frameRefresh.addEventListener("click", () => void runAction(dom.frameRefresh, pollFrames).catch(() => {}));
    document.querySelectorAll("[data-builder-mode]").forEach((button) => {
      button.addEventListener("click", () => setBuilderMode(button.dataset.builderMode));
    });
    dom.ruleBehavior.addEventListener("change", updateRuleBehavior);
    dom.rulePhysicalValue.addEventListener("input", syncRawFromPhysical);
    [dom.ruleCanId, dom.ruleStartBit, dom.ruleLength].forEach((input) => {
      input.addEventListener("input", () => {
        clearSignalConversionIfTargetChanged();
        syncRawFromPhysical();
      });
    });
    [dom.ruleDirection, dom.ruleLittleEndian].forEach((input) => {
      input.addEventListener("change", () => {
        clearSignalConversionIfTargetChanged();
        syncRawFromPhysical();
      });
    });
    dom.ruleRawValue.addEventListener("input", syncPhysicalFromRaw);
    dom.stageRuleButton.addEventListener("click", () => void runAction(dom.stageRuleButton, stageRule).catch(() => {}));
    dom.resetRuleButton.addEventListener("click", resetRuleBuilder);

    dom.rawBitGrid.addEventListener("click", (event) => {
      const button = event.target.closest("[data-raw-bit]");
      if (button) cycleRawBit(Number(button.dataset.rawBit));
    });
    dom.loadRawFrameButton.addEventListener("click", () => void runAction(dom.loadRawFrameButton, async () => useLatestRawFrame()).catch(() => {}));
    dom.stageRawMaskButton.addEventListener("click", () => void runAction(dom.stageRawMaskButton, stageRawMask).catch(() => {}));
    dom.clearRawMaskButton.addEventListener("click", resetRawMask);

    dom.recipeType.addEventListener("change", renderRecipeFields);
    dom.recipeFields.addEventListener("input", updateRecipePreview);
    dom.recipeFields.addEventListener("change", updateRecipePreview);
    dom.addRecipeButton.addEventListener("click", () => void runAction(dom.addRecipeButton, async () => addRecipeToPackage()).catch(() => {}));
    dom.resetRecipeButton.addEventListener("click", renderRecipeFields);

    document.querySelectorAll("[data-rule-view]").forEach((button) => {
      button.addEventListener("click", () => setRuleView(button.dataset.ruleView));
    });

    dom.ruleRows.addEventListener("click", (event) => {
      const button = event.target.closest("[data-set-rule]");
      if (!button) return;
      const ruleId = Number(button.dataset.setRule);
      const input = dom.ruleRows.querySelector(`[data-rule-value="${ruleId}"]`);
      void runAction(button, () => setRuleValue(ruleId, input.value)).catch(() => void refreshRules());
    });
    dom.ruleRows.addEventListener("change", (event) => {
      const toggle = event.target.closest("[data-toggle-rule]");
      if (!toggle) return;
      void runAction(toggle, () => setRuleEnabled(Number(toggle.dataset.toggleRule), toggle.checked))
        .catch(() => void refreshRules());
    });

    dom.commitRulesButton.addEventListener("click", () => void runAction(
      dom.commitRulesButton,
      async () => {
        await ruleAction("apply_commit", "Draft applied to live traffic");
        setRuleView("active");
      }
    ).catch(() => {}));
    dom.revertRulesButton.addEventListener("click", () => void runAction(
      dom.revertRulesButton,
      () => ruleAction("revert", "Draft reset to the live table")
    ).catch(() => {}));
    dom.clearDraftButton.addEventListener("click", () => void runAction(
      dom.clearDraftButton,
      () => ruleAction("clear_staging", "Draft emptied")
    ).catch(() => {}));
    dom.clearRulesButton.addEventListener("click", () => void runAction(
      dom.clearRulesButton,
      () => ruleAction("clear_rules", "Live and draft rules stopped")
    ).catch(() => {}));
    dom.savePackageButton.addEventListener("click", () => void runAction(dom.savePackageButton, savePackage).catch(() => {}));
    dom.downloadPackageButton.addEventListener("click", downloadPackage);

    dom.startLogButton.addEventListener("click", () => void runAction(dom.startLogButton, () => logAction("start")).catch(() => {}));
    dom.stopLogButton.addEventListener("click", () => void runAction(dom.stopLogButton, () => logAction("stop")).catch(() => {}));
    dom.dryRunButton.addEventListener("click", () => void runAction(dom.dryRunButton, dryRunFrame).catch(() => {}));
    dom.copyExampleButton.addEventListener("click", () => void runAction(dom.copyExampleButton, async () => {
      await copyText(dom.oilExample.textContent);
      showToast("Example copied", "Paste it into your own dashboard and replace the signal index.", "success");
    }).catch(() => {}));

    document.addEventListener("visibilitychange", () => {
      if (!document.hidden) {
        void pollOnce("status", pollStatus);
        void pollOnce("frames", pollFrames);
        void pollOnce("catalog-live", refreshCatalogLiveValues);
      }
    });
  }

  async function initialize() {
    initializeTheme();
    bindEvents();
    resetRuleBuilder();
    resetRawMask();
    renderRecipeFields();
    setBuilderMode("signal");
    setRuleView("staging");
    dom.stopLogButton.disabled = true;

    await Promise.allSettled([
      pollStatus(),
      pollFrames(),
      refreshRules(),
      refreshCatalog(true),
      pollLog(),
      loadExistingPackage()
    ]);

    setInterval(() => void pollOnce("status", pollStatus), 1500);
    setInterval(() => void pollOnce("frames", pollFrames), 900);
    setInterval(() => void pollOnce("catalog-live", refreshCatalogLiveValues), 1100);
    setInterval(() => void pollOnce("log", pollLog), 1800);
  }

  void initialize();
})();
