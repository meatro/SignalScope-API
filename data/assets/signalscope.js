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
    "selectedSignal", "ruleCanId", "ruleDirection", "ruleStartBit", "ruleLength",
    "rulePhysicalValue", "ruleRawValue", "ruleLittleEndian", "encodingHelp",
    "stageRuleButton", "resetRuleButton", "ruleStateBadge", "ruleRows",
    "commitRulesButton", "revertRulesButton", "clearRulesButton", "packageText",
    "savePackageButton", "downloadPackageButton", "loadPackageTextButton",
    "copyExampleButton", "oilExample", "logStatus", "startLogButton",
    "stopLogButton", "replayCanId", "replayData", "dryRunButton", "requestLog",
    "toastRegion"
  ].forEach((id) => { dom[id] = byId(id); });

  const state = {
    online: false,
    status: null,
    rules: [],
    ruleEpoch: 0,
    selectedSignal: null,
    catalogSignals: [],
    catalogTotal: 0,
    catalogOffset: 0,
    catalogQuery: "",
    dbcText: "",
    dbcFilename: "",
    frameSeen: new Map(),
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
    setBadge(
      dom.ruleStateBadge,
      candidateCount ? `${candidateCount} candidate rule${candidateCount === 1 ? "" : "s"}` : "Empty candidate",
      candidateCount ? "warning" : "neutral"
    );

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
    const now = Date.now();
    (payload.frames || []).forEach((frame) => {
      const key = frameKey(frame);
      const previous = state.frameSeen.get(key);
      if (!previous || previous.timestamp !== frame.timestamp_us) {
        state.frameSeen.set(key, { timestamp: frame.timestamp_us, at: now });
      }
    });
    renderFrames(payload.frames || []);
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
        const useButton = element("button", "button quiet", "Use in rule");
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
    dom.selectedSignalHelp.textContent = "The DBC supplied this bit location and conversion. Verify its live behavior before applying a rule.";
    dom.ruleCanId.value = signal.canIdHex || formatCanId(signal.canId);
    dom.ruleDirection.value = signal.direction || "A_TO_B";
    dom.ruleStartBit.value = signal.startBit;
    dom.ruleLength.value = signal.length;
    dom.ruleLittleEndian.checked = Boolean(signal.littleEndian);
    if (signal.valid && Number.isFinite(Number(signal.value))) dom.rulePhysicalValue.value = signal.value;
    syncRawFromPhysical();
    document.getElementById("rules").scrollIntoView({ behavior: "smooth", block: "start" });
  }

  function clampBigInt(value, minimum, maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
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

  function resetRuleBuilder() {
    state.selectedSignal = null;
    dom.selectedSignal.hidden = true;
    dom.selectedSignal.replaceChildren();
    dom.selectedSignalHelp.textContent = "Choose Use in rule beside a DBC signal, or enter the bit location manually.";
    dom.ruleCanId.value = "0x321";
    dom.ruleDirection.value = "A_TO_B";
    dom.ruleStartBit.value = "0";
    dom.ruleLength.value = "8";
    dom.rulePhysicalValue.value = "90";
    dom.ruleRawValue.value = "130";
    dom.ruleLittleEndian.checked = true;
    dom.encodingHelp.textContent = "No DBC signal selected; raw value is used directly.";
  }

  function ruleFormValues() {
    const canId = parseCanId(dom.ruleCanId.value);
    const startBit = Number(dom.ruleStartBit.value);
    const length = Number(dom.ruleLength.value);
    if (!Number.isInteger(startBit) || startBit < 0 || startBit > 63) throw new Error("Start bit must be between 0 and 63");
    if (!Number.isInteger(length) || length < 1 || length > 64 || (dom.ruleLittleEndian.checked && startBit + length > 64)) {
      throw new Error("The selected bit range must fit inside an 8-byte CAN frame");
    }
    let raw;
    try {
      raw = BigInt(dom.ruleRawValue.value.trim());
    } catch (error) {
      throw new Error("Raw value must be a whole number");
    }
    const maximum = (1n << BigInt(length)) - 1n;
    if (raw < 0n || raw > maximum) throw new Error(`Raw value must fit in ${length} bits`);
    return {
      canId,
      direction: dom.ruleDirection.value,
      startBit,
      length,
      littleEndian: dom.ruleLittleEndian.checked,
      raw: raw.toString()
    };
  }

  function appendRuleToPackage(rule) {
    const line = `STATIC,${formatCanId(rule.canId)},${rule.direction},${rule.startBit},${rule.length},${rule.littleEndian ? 1 : 0},${rule.raw}`;
    const current = dom.packageText.value.trimEnd();
    const header = "# SignalScope rule package\n# kind,can_id,direction,start_bit,length,little_endian,raw_value";
    dom.packageText.value = `${current || header}\n${line}\n`;
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
      dynamic: 0,
      replace_value: rule.raw,
      enabled: 1
    });
    state.ruleEpoch = Number(payload.rule_epoch || state.ruleEpoch);
    appendRuleToPackage(rule);
    await refreshRules();
    await pollStatus();
    showToast("Rule added to candidate", "Review it, then choose Apply now to change the live pipeline.", "success");
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

  function renderRules() {
    dom.ruleRows.replaceChildren();
    if (!state.rules.length) {
      const row = element("tr");
      const cell = element("td", "empty", "No rules loaded.");
      cell.colSpan = 4;
      row.append(cell);
      dom.ruleRows.append(row);
      return;
    }

    state.rules.forEach((rule) => {
      const row = element("tr");
      const idCell = element("td");
      idCell.append(element("code", "", `#${rule.rule_id}`));
      const target = element("td");
      const targetText = rule.kind === "RAW_MASK"
        ? `${formatCanId(rule.can_id)} raw mask`
        : `${formatCanId(rule.can_id)} bit ${rule.start_bit}:${rule.length}`;
      target.append(element("span", "signal-name"));
      target.firstChild.append(
        element("strong", "", targetText),
        element("small", "", `${rule.direction} · ${rule.kind}`)
      );

      const valueCell = element("td", "rule-value-control");
      if (rule.kind === "BIT_RANGE" && rule.dynamic) {
        const valueInput = element("input");
        valueInput.type = "number";
        valueInput.min = "0";
        valueInput.value = exactRuleValue(rule);
        valueInput.dataset.ruleValue = String(rule.rule_id);
        valueInput.setAttribute("aria-label", `Raw value for rule ${rule.rule_id}`);
        const setButton = element("button", "button quiet", "Set");
        setButton.type = "button";
        setButton.dataset.setRule = String(rule.rule_id);
        valueCell.append(valueInput, setButton);
      } else if (rule.kind === "BIT_RANGE") {
        const value = element("span", "signal-name");
        value.append(
          element("code", "", exactRuleValue(rule)),
          element("small", "", "Restage to change")
        );
        valueCell.append(value);
      } else {
        valueCell.textContent = "mask";
      }

      const stateCell = element("td", "rule-state-control");
      stateCell.append(element("span", "badge warning", "Candidate"));
      const toggle = element("label", "rule-switch");
      const checkbox = element("input");
      checkbox.type = "checkbox";
      // `active` is retained by the API as a compatibility alias for a rule's
      // enabled flag. It does not identify which table this row came from.
      checkbox.checked = typeof rule.enabled === "boolean" ? rule.enabled : Boolean(rule.active);
      checkbox.dataset.toggleRule = String(rule.rule_id);
      checkbox.setAttribute("aria-label", `Enable rule ${rule.rule_id}`);
      toggle.append(checkbox, element("span", "", "Enabled"));
      stateCell.append(toggle);
      row.append(idCell, target, valueCell, stateCell);
      dom.ruleRows.append(row);
    });
  }

  async function refreshRules() {
    // The editor works on the candidate table. Plain GET /api/rules is the
    // active table currently affecting traffic and is intentionally separate.
    const payload = await api("/api/rules?view=staging", { quiet: true });
    state.rules = payload.rules || [];
    state.ruleEpoch = Number(payload.rule_epoch || 0);
    renderRules();
  }

  async function setRuleValue(ruleId, value) {
    await postForm("/api/rules/value", {
      rule_id: ruleId,
      rule_epoch: state.ruleEpoch,
      value
    });
    await refreshRules();
    showToast("Rule value updated", `Rule #${ruleId} now uses raw ${value}.`, "success");
  }

  async function setRuleEnabled(ruleId, enabled) {
    await postForm("/api/rules/enable", {
      rule_id: ruleId,
      rule_epoch: state.ruleEpoch,
      enabled: enabled ? 1 : 0
    });
    await refreshRules();
    showToast(enabled ? "Rule enabled" : "Rule disabled", `Rule #${ruleId} was updated.`, "success");
  }

  async function ruleAction(action, successMessage) {
    const payload = await postText("/api/rules", action);
    state.ruleEpoch = Number(payload.rule_epoch || state.ruleEpoch);
    await Promise.all([refreshRules(), pollStatus()]);
    showToast(successMessage, "The controller accepted the new rule table.", "success");
  }

  function packageFromVisibleRules() {
    const unsupported = state.rules.some((rule) => rule.kind !== "BIT_RANGE" || rule.dynamic);
    if (unsupported) {
      throw new Error("This table contains dynamic or raw-mask rules. Keep their original .ssrules text so source details are not lost.");
    }
    const rows = state.rules.map((rule) =>
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
    const result = await postText(`/api/rules/package?path=${encodeURIComponent(ACTIVE_PACKAGE_PATH)}`, text);
    state.ruleEpoch = Number(result.rule_epoch || state.ruleEpoch);
    await Promise.all([refreshRules(), pollStatus()]);
    showToast("Startup package saved", `${result.count ?? state.rules.length} rules will return after reboot.`, "success");
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
    dom.packageText.value = "";
    await Promise.all([pollStatus(), refreshRules(), refreshCatalog(true)]);
    setBadge(dom.dbcStatus, `${payload.signals || 0} signals`, "success");
    showToast("DBC loaded", `${payload.messages || 0} messages and ${payload.signals || 0} signals are ready.`, "success");
  }

  async function autoloadDbc() {
    const payload = await postForm("/api/dbc/autoload", {});
    state.catalogSignals = [];
    state.catalogOffset = 0;
    await Promise.all([pollStatus(), refreshRules(), refreshCatalog(true)]);
    showToast("Installed DBC reloaded", `${payload.signals || 0} signals are ready.`, "success");
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
    dom.rulePhysicalValue.addEventListener("input", syncRawFromPhysical);
    dom.ruleLength.addEventListener("input", syncRawFromPhysical);
    dom.ruleLittleEndian.addEventListener("change", syncRawFromPhysical);
    dom.ruleRawValue.addEventListener("input", syncPhysicalFromRaw);
    dom.stageRuleButton.addEventListener("click", () => void runAction(dom.stageRuleButton, stageRule).catch(() => {}));
    dom.resetRuleButton.addEventListener("click", resetRuleBuilder);

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
      () => ruleAction("apply_commit", "Rules applied")
    ).catch(() => {}));
    dom.revertRulesButton.addEventListener("click", () => void runAction(
      dom.revertRulesButton,
      () => ruleAction("revert", "Candidate reverted")
    ).catch(() => {}));
    dom.clearRulesButton.addEventListener("click", () => void runAction(dom.clearRulesButton, async () => {
      await ruleAction("clear_rules", "Rules cleared");
      dom.packageText.value = "";
    }).catch(() => {}));
    dom.savePackageButton.addEventListener("click", () => void runAction(dom.savePackageButton, savePackage).catch(() => {}));
    dom.loadPackageTextButton.addEventListener("click", () => void runAction(dom.loadPackageTextButton, savePackage).catch(() => {}));
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
