#include <Arduino.h>
#include <LittleFS.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <driver/twai.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mcp2515.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/dbc_parser.hpp"
#include "core/application_extension.hpp"
#include "core/can_trace.hpp"
#include "core/frame_cache.hpp"
#include "core/gateway.hpp"
#include "core/mutation_engine.hpp"
#include "core/observation_manager.hpp"
#include "core/parked_power.hpp"
#include "core/replay_engine.hpp"
#include "core/rule_package.hpp"
#include "core/runtime_memory.hpp"
#include "core/runtime_values.hpp"
#include "core/runtime_tables.hpp"
#include "core/signal_cache.hpp"
#include "core/signal_catalog.hpp"
#include "core/signal_codec.hpp"
#include "core/types.hpp"
#include "fs/persistence.hpp"
#include "fs/session_log.hpp"

// setup() mounts and validates application resources before handing work to
// the dedicated 16 KiB runtime tasks. Match that headroom during boot instead
// of relying on Arduino's smaller 8 KiB loop-task default.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

using namespace bored::signalscope;

namespace {

constexpr const char* kApSsid = "SignalScope-AP";
constexpr const char* kApPassword = "signalscope";
constexpr size_t kStatusFrameLimit = 40;
constexpr size_t kSignalSnapshotLimit = 384;
constexpr size_t kSignalCatalogDefaultLimit = 48;
constexpr size_t kSignalCatalogMaximumLimit = 96;
constexpr uint32_t kSignalCatalogFreshnessUs = 1500000U;
constexpr size_t kMaxPollFramesPerBus = 128;
constexpr size_t kMaxDecodedSignalsPerFrame = 24;
constexpr size_t kStatusRuleLimit = 48;
constexpr uint32_t kCanHealthPollMs = 100U;
constexpr uint32_t kCanInitialRecoveryRetryMs = 100U;
constexpr uint32_t kMcpRecoveryRetryMs = 5000U;
constexpr uint32_t kTwaiRecoveryRetryMs = 5000U;

constexpr int kBusARxPin = 6;
constexpr int kBusATxPin = 7;
constexpr int kMcpCsPin = 10;
constexpr int kMcpSclkPin = 12;
constexpr int kMcpMosiPin = 11;
constexpr int kMcpMisoPin = 13;
constexpr int kMcpRstPin = 9;
constexpr int kMcpIntPin = 8;
constexpr uint32_t kParkedCanQuietMs = 3U;
constexpr uint32_t kParkedCanQuiesceTimeoutMs = 250U;
constexpr uint32_t kParkedCanHandshakeTimeoutMs = 1500U;

// Keep CAN runtime isolated from UI/server runtime.
// Set false to run both runtime tasks on core 0 (single-core style scheduling).
constexpr bool kUseDualCore = true;

constexpr BaseType_t selectCanCore() {
    return (kUseDualCore && (portNUM_PROCESSORS > 1)) ? 1 : 0;
}

constexpr BaseType_t selectUiCore() {
    return 0;
}

constexpr bool dualCoreRuntimeEnabled() {
    return (kUseDualCore && (portNUM_PROCESSORS > 1));
}

constexpr uint32_t kCanTaskStackBytes = 16384;
constexpr uint32_t kApplicationTaskStackBytes = 16384;
constexpr uint32_t kUiTaskStackBytes = 16384;
constexpr uint32_t kTwaiAlertTaskStackBytes = 3072;

GatewayCore gateway;
CanTraceQueue can_trace;
MutationEngine mutation_engine;
ReplayEngine replay_engine;
FrameCache frame_cache;
SignalCache signal_cache;
ObservationManager observation_manager;
PersistenceStore persistence;
SessionLogRecorder session_log;
ApplicationExtensionRegistry application_registry;
ApplicationServices application_services;
RuntimeValueRegistry runtime_values;
RuntimeTableRegistry runtime_tables;
ParkedPowerController parked_power;

RTC_DATA_ATTR uint32_t parked_deep_sleep_entries = 0U;

DbcDatabase dbc_database;
std::atomic<const DbcDatabase*> active_dbc{nullptr};

WebServer server(80);
MCP2515 can_mcp(kMcpCsPin, 10000000, &SPI);

TaskHandle_t can_task_handle = nullptr;
TaskHandle_t application_task_handle = nullptr;
TaskHandle_t ui_task_handle = nullptr;
TaskHandle_t twai_alert_task_handle = nullptr;
SemaphoreHandle_t application_mutex = nullptr;
SemaphoreHandle_t session_log_mutex = nullptr;
SemaphoreHandle_t replay_mutex = nullptr;

std::atomic<uint8_t> bus_a_ready{0};
std::atomic<uint8_t> bus_b_ready{0};
std::atomic<uint8_t> fs_mounted{0};
std::atomic<uint16_t> frame_rate_fps{0};
std::atomic<uint32_t> ingress_a_frames{0};
std::atomic<uint32_t> ingress_b_frames{0};
std::atomic<uint32_t> can_stack_min_free{0};
std::atomic<uint32_t> application_stack_min_free{0};
std::atomic<uint32_t> ui_stack_min_free{0};
std::atomic<uint32_t> can_runtime_started_us{0};
std::atomic<uint32_t> application_config_ready_us{0};
std::atomic<uint8_t> application_resource_reload_requested{0};
std::atomic<uint8_t> twai_runtime_state{static_cast<uint8_t>(TWAI_STATE_STOPPED)};
std::atomic<uint8_t> twai_driver_installed{0};
std::atomic<uint32_t> twai_recovery_attempts{0};
std::atomic<uint32_t> twai_restarts{0};
std::atomic<uint32_t> twai_rx_missed{0};
std::atomic<uint32_t> twai_rx_overruns{0};
std::atomic<uint8_t> mcp_error_flags{0};
std::atomic<uint32_t> mcp_recoveries{0};
std::atomic<uint32_t> mcp_rx_overruns{0};
std::atomic<uint32_t> last_physical_ingress_ms{0U};
std::atomic<uint8_t> parked_can_quiesce_requested{0U};
std::atomic<uint8_t> parked_can_quiesced{0U};
std::atomic<uint8_t> parked_can_quiesce_failed{0U};
std::atomic<uint8_t> parked_alert_stop_requested{0U};
std::atomic<uint8_t> parked_alert_stopped{0U};
bool twai_recovery_active = false;
uint32_t last_can_health_poll_ms = 0U;
uint32_t last_session_health_ms = 0U;
uint32_t next_twai_recovery_attempt_ms = 0U;
uint32_t next_mcp_recovery_attempt_ms = 0U;

// Formatting a periodic log line can block a USB/UART stream for several CAN
// frame times. The CAN task publishes an atomic snapshot and the lower-priority
// application task performs the actual Serial write.
std::atomic<uint8_t> gateway_log_pending{0};
std::atomic<uint32_t> gateway_log_drops_boot{0};
std::atomic<uint32_t> gateway_log_drops_run{0};
std::atomic<uint32_t> gateway_log_deferred_a_to_b{0};
std::atomic<uint32_t> gateway_log_deferred_b_to_a{0};
std::atomic<uint32_t> gateway_log_egress_a_to_b{0};
std::atomic<uint32_t> gateway_log_egress_b_to_a{0};
std::atomic<uint32_t> gateway_log_passive{0};
std::atomic<uint32_t> gateway_log_decoded{0};
std::atomic<uint32_t> gateway_log_active_rules{0};
std::atomic<uint32_t> gateway_log_stack_free{0};

// HTTP handlers execute serially on ss_ui. Keep their large snapshot buffers
// in runtime memory (PSRAM when available), never on the UI task stack.
RuleListEntry* ui_rule_scratch = nullptr;
FrameCacheSnapshot* ui_frame_scratch = nullptr;
uint16_t* ui_signal_index_scratch = nullptr;
SignalCacheSnapshot* ui_signal_scratch = nullptr;
String ui_index_path = "/index.html";
String active_dbc_path;
String active_rule_package_path;
constexpr const char* kDbcDirPath = "/dbc";
constexpr const char* kActiveDbcPath = "/dbc/active.dbc";
constexpr const char* kRulesDirPath = "/rules";
constexpr const char* kActiveRulePackagePath = "/rules/active.ssrules";

class ApplicationLockGuard {
public:
    explicit ApplicationLockGuard(TickType_t wait_ticks = portMAX_DELAY) {
        if (application_mutex == nullptr) {
            locked_ = true;
            return;
        }
        locked_ = xSemaphoreTakeRecursive(application_mutex, wait_ticks) == pdTRUE;
    }

    ~ApplicationLockGuard() {
        release();
    }

    ApplicationLockGuard(const ApplicationLockGuard&) = delete;
    ApplicationLockGuard& operator=(const ApplicationLockGuard&) = delete;

    bool locked() const { return locked_; }

    void release() {
        if (locked_ && application_mutex != nullptr) {
            static_cast<void>(xSemaphoreGiveRecursive(application_mutex));
        }
        locked_ = false;
    }

private:
    bool locked_ = false;
};

class SessionLogLockGuard {
public:
    explicit SessionLogLockGuard(TickType_t wait_ticks = portMAX_DELAY) {
        if (session_log_mutex == nullptr) {
            locked_ = true;
            return;
        }
        locked_ = xSemaphoreTakeRecursive(session_log_mutex, wait_ticks) == pdTRUE;
    }

    ~SessionLogLockGuard() { release(); }

    SessionLogLockGuard(const SessionLogLockGuard&) = delete;
    SessionLogLockGuard& operator=(const SessionLogLockGuard&) = delete;

    bool locked() const { return locked_; }

    void release() {
        if (locked_ && session_log_mutex != nullptr) {
            static_cast<void>(xSemaphoreGiveRecursive(session_log_mutex));
        }
        locked_ = false;
    }

private:
    bool locked_ = false;
};

const char* directionToString(Direction direction) {
    return (direction == Direction::A_TO_B) ? "A_TO_B" : "B_TO_A";
}

const char* twaiStateName(uint8_t state) {
    switch (static_cast<twai_state_t>(state)) {
    case TWAI_STATE_RUNNING: return "running";
    case TWAI_STATE_BUS_OFF: return "bus_off";
    case TWAI_STATE_RECOVERING: return "recovering";
    case TWAI_STATE_STOPPED:
    default: return "stopped";
    }
}

Direction parseDirectionFromText(const String& text, Direction fallback) {
    if (text == "A_TO_B") return Direction::A_TO_B;
    if (text == "B_TO_A") return Direction::B_TO_A;
    return fallback;
}

const char* observationModeToString(ObservationMode mode) {
    switch (mode) {
    case ObservationMode::ALL:
        return "all";
    case ObservationMode::SPECIFIC:
        return "specific";
    case ObservationMode::NONE:
    default:
        return "none";
    }
}

ObservationMode parseObservationMode(const String& text) {
    if (text == "all" || text == "ALL") return ObservationMode::ALL;
    if (text == "specific" || text == "SPECIFIC") return ObservationMode::SPECIFIC;
    return ObservationMode::NONE;
}

const char* ruleKindToString(RuleKind kind) {
    switch (kind) {
    case RuleKind::RAW_MASK: return "RAW_MASK";
    case RuleKind::COUNTER: return "COUNTER";
    case RuleKind::CHECKSUM_XOR: return "CHECKSUM_XOR";
    case RuleKind::CHECKSUM_CRC8_AUTOSAR: return "CHECKSUM_CRC8_AUTOSAR";
    case RuleKind::SEQUENCE8: return "SEQUENCE8";
    case RuleKind::BIT_RANGE:
    default: return "BIT_RANGE";
    }
}

bool parseBoolText(const String& text, bool fallback) {
    if (text == "1" || text == "true" || text == "TRUE" || text == "on") return true;
    if (text == "0" || text == "false" || text == "FALSE" || text == "off") return false;
    return fallback;
}

bool tryParseUIntArg(const char* name, uint32_t& output) {
    if (!server.hasArg(name)) return false;
    const String text = server.arg(name);
    if (text.length() == 0U || text[0] == '-') return false;
    char* end_ptr = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end_ptr, 0);
    if (end_ptr == text.c_str() || *end_ptr != '\0' || value > 0xFFFFFFFFULL) return false;
    output = static_cast<uint32_t>(value);
    return true;
}

uint32_t parseUIntArg(const char* name, uint32_t fallback) {
    uint32_t value = 0U;
    return tryParseUIntArg(name, value) ? value : fallback;
}

bool tryParseUInt64Arg(const char* name, uint64_t& output) {
    if (!server.hasArg(name)) return false;
    const String text = server.arg(name);
    if (text.length() == 0U || text[0] == '-') return false;
    char* end_ptr = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end_ptr, 0);
    if (end_ptr == text.c_str() || *end_ptr != '\0') return false;
    output = static_cast<uint64_t>(value);
    return true;
}

int32_t parseIntArg(const char* name, int32_t fallback) {
    if (!server.hasArg(name)) return fallback;
    const String text = server.arg(name);
    if (text.length() == 0U) return fallback;
    char* end_ptr = nullptr;
    const long value = std::strtol(text.c_str(), &end_ptr, 0);
    if (end_ptr == text.c_str() || *end_ptr != '\0') return fallback;
    return static_cast<int32_t>(value);
}

String uint64ToDecimalString(uint64_t value) {
    char text[24] = {};
    std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
    return String(text);
}

float parseFloatArg(const char* name, float fallback) {
    if (!server.hasArg(name)) return fallback;
    return server.arg(name).toFloat();
}

String contentTypeForPath(const String& path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".css")) return "text/css";
    if (path.endsWith(".js")) return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".woff")) return "font/woff";
    if (path.endsWith(".woff2")) return "font/woff2";
    if (path.endsWith(".ico")) return "image/x-icon";
    return "application/octet-stream";
}

String escapeJsonString(const char* input) {
    String out;
    if (input == nullptr) return out;

    for (size_t i = 0; input[i] != '\0'; ++i) {
        const char c = input[i];
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += (static_cast<unsigned char>(c) < 0x20U) ? ' ' : c;
            break;
        }
    }
    return out;
}

void resolveUiIndexPath() {
    ui_index_path = LittleFS.exists("/index.html") ? "/index.html" : "/index.htm";
}
bool loadDbcFromFilePath(const String& fs_path) {
    DbcDatabase candidate_database;
    SignalCache candidate_cache;
    candidate_cache.init();

    size_t source_bytes = 0U;
    bool candidate_loaded = false;
    const ApplicationExtension* const app = application_registry.extension();
    if (app != nullptr && app->loadDatabase != nullptr) {
        const ApplicationDatabaseLoadResult result =
            app->loadDatabase(fs_path.c_str(), &candidate_database, &source_bytes);
        if (result == ApplicationDatabaseLoadResult::FAILED) return false;
        candidate_loaded = result == ApplicationDatabaseLoadResult::LOADED;
    }

    String dbc_text;
    if (!candidate_loaded) {
        if (fs_mounted.load(std::memory_order_acquire) == 0U || !LittleFS.exists(fs_path)) {
            return false;
        }
        File file = LittleFS.open(fs_path, "r");
        if (!file || file.isDirectory()) {
            if (file) file.close();
            return false;
        }
        dbc_text = file.readString();
        file.close();
        if (dbc_text.length() == 0U ||
            !candidate_database.parseFromText(
                dbc_text.c_str(), static_cast<size_t>(dbc_text.length()))) {
            return false;
        }
        source_bytes = static_cast<size_t>(dbc_text.length());
    }
    if (!candidate_cache.resetForDbc(candidate_database)) {
        return false;
    }

    gateway.pauseSignalDecoding();
    const uint32_t wait_started_ms = millis();
    while (!gateway.signalDecodingIdle() && millis() - wait_started_ms < 250U) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!gateway.signalDecodingIdle()) {
        gateway.resumeSignalDecoding();
        return false;
    }

    dbc_database.swap(candidate_database);
    signal_cache.swap(candidate_cache);
    active_dbc.store(&dbc_database, std::memory_order_release);
    signal_cache.clearSubscriptions();
    observation_manager.clearSpecific();
    observation_manager.clearMandatory();
    observation_manager.setMode(ObservationMode::NONE);
    if (replay_mutex == nullptr || xSemaphoreTake(replay_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        replay_engine.stop();
        if (replay_mutex != nullptr) xSemaphoreGive(replay_mutex);
    }
    mutation_engine.clearRules();
    active_rule_package_path = "";
    active_dbc_path = fs_path;

    if (app != nullptr && app->databaseChanged != nullptr) {
        ApplicationLockGuard app_lock;
        if (app_lock.locked()) app->databaseChanged();
    }
    gateway.resumeSignalDecoding();

    Serial.printf(
        "[dbc] auto-loaded %s (%lu bytes, %lu msgs, %lu signals)\n",
        fs_path.c_str(),
        static_cast<unsigned long>(source_bytes),
        static_cast<unsigned long>(dbc_database.messageCount()),
        static_cast<unsigned long>(dbc_database.signalCount()));
    constexpr uint32_t kDecodeProbeIds[] = {0x1A0U, 0x280U, 0x2A0U, 0x38AU, 0x4A0U};
    Serial.print("[dbc] lookup probes");
    for (const uint32_t can_id : kDecodeProbeIds) {
        const DbcMessageDef* probe = dbc_database.findMessage(can_id);
        Serial.printf(" 0x%03lX=%s", static_cast<unsigned long>(can_id),
                      probe == nullptr ? "MISS" : probe->name);
    }
    Serial.println();

    return true;
}

bool autoLoadDbcFromLittleFs() {
    if (fs_mounted.load(std::memory_order_acquire) == 0U) return false;

    constexpr const char* kPreferredPaths[] = {
        "/dbc/active.dbc",
        "/dbc/default.dbc",
    };

    for (const char* path : kPreferredPaths) {
        if (loadDbcFromFilePath(path)) return true;
    }

    File dir = LittleFS.open("/dbc");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return false;
    }

    File entry = dir.openNextFile();
    while (entry) {
        const bool is_dir = entry.isDirectory();
        String name = entry.name();
        entry.close();

        if (!is_dir) {
            String lower = name;
            lower.toLowerCase();
            if (lower.endsWith(".dbc")) {
                if (!name.startsWith("/")) {
                    name = String("/dbc/") + name;
                }
                if (loadDbcFromFilePath(name)) {
                    dir.close();
                    return true;
                }
            }
        }

        entry = dir.openNextFile();
    }

    dir.close();
    return false;
}

bool applyApplicationDatabaseRequest() {
    const ApplicationExtension* app = application_registry.extension();
    if (app == nullptr || app->requestedDatabasePath == nullptr) return true;

    const char* requested = app->requestedDatabasePath();
    if (requested == nullptr || requested[0] == '\0') return true;
    if (active_dbc_path == requested) return true;
    return loadDbcFromFilePath(String(requested));
}

bool validRulePackagePath(const String& path) {
    return path.length() > 15U && path.length() <= 96U &&
        path.startsWith("/rules/") && path.indexOf("..") < 0 &&
        path.indexOf("/.") < 0 && path.indexOf('\\') < 0 &&
        path.indexOf("//") < 0 && path.endsWith(".ssrules");
}

bool loadRulePackageFromFilePath(const String& path) {
    if (active_rule_package_path == path) return true;
    if (!validRulePackagePath(path)) return false;
    if (fs_mounted.load(std::memory_order_acquire) == 0U || !LittleFS.exists(path)) return false;
    File file = LittleFS.open(path, "r");
    if (!file || file.isDirectory()) { if (file) file.close(); return false; }
    String text = file.readString();
    file.close();
    if (text.length() == 0U) return false;
    char* mutable_text = static_cast<char*>(std::malloc(text.length() + 1U));
    if (mutable_text == nullptr) return false;
    std::memcpy(mutable_text, text.c_str(), text.length() + 1U);
    size_t loaded = 0U;
    const bool ok = RulePackageLoader::loadCsv(mutable_text, text.length(), mutation_engine, &loaded);
    std::free(mutable_text);
    if (!ok) return false;
    active_rule_package_path = path;
    Serial.printf("[rules] loaded %s (%lu rules)\n", path.c_str(), static_cast<unsigned long>(loaded));
    return true;
}

bool autoLoadRulePackageFromLittleFs() {
    if (loadRulePackageFromFilePath(kActiveRulePackagePath)) return true;
    const String default_path = String(kRulesDirPath) + "/default.ssrules";
    if (loadRulePackageFromFilePath(default_path)) return true;
    return false;
}

bool applyApplicationRulePackageRequest() {
    const ApplicationExtension* app = application_registry.extension();
    if (app == nullptr || app->requestedRulePackagePath == nullptr) return true;
    const char* requested = app->requestedRulePackagePath();
    if (requested == nullptr || requested[0] == '\0') {
        if (active_rule_package_path.length() == 0U) return true;
        mutation_engine.clearRules();
        active_rule_package_path = "";
        return true;
    }
    return loadRulePackageFromFilePath(String(requested));
}

enum class ApplicationResourceApplyResult : uint8_t {
    OK,
    DATABASE_FAILED,
    RULE_PACKAGE_FAILED,
};

// Caller holds application_mutex. Manual DBC replacement and structural
// generic-rule replacement intentionally remain available to expert
// SignalScope users, but they are no longer an application-owned resource
// transaction.
void invalidateApplicationResourcesLocked() {
    const ApplicationExtension* app = application_registry.extension();
    if (app != nullptr && app->finishConfigure != nullptr) {
        app->finishConfigure(false);
    }
}

ApplicationResourceApplyResult applyApplicationResourceTransaction() {
    const ApplicationExtension* app = application_registry.extension();
    const bool database_ok = applyApplicationDatabaseRequest();
    const bool rules_ok = database_ok && applyApplicationRulePackageRequest();
    const bool resources_ok = database_ok && rules_ok;

    if (app != nullptr && app->finishConfigure != nullptr) {
        app->finishConfigure(resources_ok);
    }

    if (!resources_ok) {
        // finishConfigure(false) restores the application's previous resource
        // selection. If the first half of the staged transaction had already
        // become active, put both resources back together before returning.
        const bool rollback_database_ok = applyApplicationDatabaseRequest();
        const bool rollback_rules_ok = applyApplicationRulePackageRequest();
        if (!rollback_database_ok || !rollback_rules_ok) {
            Serial.println("[app] configuration resource rollback failed");
        }
        if (app != nullptr && app->finishConfigure != nullptr) {
            app->finishConfigure(rollback_database_ok && rollback_rules_ok);
        }
    }

    if (!database_ok) return ApplicationResourceApplyResult::DATABASE_FAILED;
    if (!rules_ok) return ApplicationResourceApplyResult::RULE_PACKAGE_FAILED;
    return ApplicationResourceApplyResult::OK;
}

void applyRequestedApplicationResources() {
    if (application_resource_reload_requested.exchange(0U, std::memory_order_acq_rel) == 0U) return;
    const ApplicationResourceApplyResult result = applyApplicationResourceTransaction();
    if (result == ApplicationResourceApplyResult::DATABASE_FAILED) {
        Serial.println("[app] deferred requested DBC could not be loaded");
    } else if (result == ApplicationResourceApplyResult::RULE_PACKAGE_FAILED) {
        Serial.println("[app] deferred requested rule package could not be loaded");
    }
}

bool isPublicStaticPath(const String& path) {
    if (path.length() == 0U || !path.startsWith("/") ||
        path.indexOf("..") >= 0 || path.indexOf('\\') >= 0) {
        return false;
    }
    if (path == "/" || path == "/index.html" || path == "/index.htm" ||
        path == "/LICENSE.md") {
        return true;
    }
    if (path.startsWith("/assets/")) return true;

    // Only presentation routes are public. DBCs, rule packages, diagnostic
    // sources, logs and any future application-private LittleFS files must be
    // accessed through their bounded API rather than the static-file fallback.
    // Multi-page starter apps can keep public presentation files under this
    // explicit root without exposing databases, rules, recordings or other
    // application data stored elsewhere on LittleFS.
    if (path.startsWith("/pages/") &&
        (path.endsWith(".html") || path.endsWith(".css") || path.endsWith(".js"))) return true;
    return false;
}

bool serveStaticFile(const String& path) {
    if (fs_mounted.load(std::memory_order_acquire) == 0U) return false;
    if (!isPublicStaticPath(path)) return false;

    String fs_path = (path == "/") ? ui_index_path : path;
    if (fs_path == SessionLogRecorder::kLogPath ||
        fs_path == SessionLogRecorder::kTemporaryPath ||
        fs_path == SessionLogRecorder::kBackupPath ||
        fs_path == SessionLogRecorder::kCheckpointPath0 ||
        fs_path == SessionLogRecorder::kCheckpointPath1) {
        return false;
    }
    bool gzip_encoded = false;
    if (!LittleFS.exists(fs_path) && !fs_path.endsWith("/")) {
        const String maybe_index = fs_path + "/index.html";
        if (LittleFS.exists(maybe_index)) fs_path = maybe_index;
    }

    // Public UI assets may be stored once in deterministic gzip form.
    if (!LittleFS.exists(fs_path) && !fs_path.endsWith("/")) {
        const String compressed = fs_path + ".gz";
        if (LittleFS.exists(compressed)) {
            fs_path = compressed;
            gzip_encoded = true;
        }
    }

    if (!LittleFS.exists(fs_path)) return false;
    File file = LittleFS.open(fs_path, "r");
    if (!file) return false;

    const String content_path = gzip_encoded
        ? fs_path.substring(0U, fs_path.length() - 3U)
        : fs_path;
    if (content_path.endsWith(".html") || content_path.endsWith(".js") || content_path.endsWith(".json")) {
        server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
        server.sendHeader("Pragma", "no-cache");
    }
    if (gzip_encoded) {
        // WebServer::_streamFileCore derives Content-Encoding from the File
        // name's .gz suffix.  Adding it here too produces two gzip codings and
        // browsers attempt to decompress the single gzip envelope twice.
        server.sendHeader("Vary", "Accept-Encoding");
    }
    server.streamFile(file, contentTypeForPath(content_path));
    file.close();
    return true;
}

String frameDataHex(const FrameCacheSnapshot& frame) {
    String out;
    out.reserve(24);
    for (uint8_t i = 0; i < frame.dlc && i < 8U; ++i) {
        if (i > 0U) out += ' ';
        char byte_text[3] = {0};
        std::snprintf(byte_text, sizeof(byte_text), "%02X", frame.data[i]);
        out += byte_text;
    }
    return out;
}

bool parseHexNibble(char c, uint8_t& out_value) {
    if (c >= '0' && c <= '9') {
        out_value = static_cast<uint8_t>(c - '0');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out_value = static_cast<uint8_t>(10 + (c - 'A'));
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        out_value = static_cast<uint8_t>(10 + (c - 'a'));
        return true;
    }
    return false;
}

bool parseHexBytes(const String& text, uint8_t out_bytes[8]) {
    char hex[32] = {0};
    size_t n = 0U;
    for (size_t i = 0; i < text.length() && n < sizeof(hex); ++i) {
        uint8_t nibble = 0U;
        if (parseHexNibble(text[i], nibble)) hex[n++] = text[i];
    }
    if (n < 16U) return false;

    for (uint8_t i = 0; i < 8U; ++i) {
        uint8_t hi = 0U;
        uint8_t lo = 0U;
        if (!parseHexNibble(hex[i * 2U], hi) || !parseHexNibble(hex[(i * 2U) + 1U], lo)) return false;
        out_bytes[i] = static_cast<uint8_t>((hi << 4U) | lo);
    }
    return true;
}

size_t parseU16Csv(const String& csv, uint16_t* out_values, size_t capacity) {
    if (out_values == nullptr || capacity == 0U) return 0U;
    size_t count = 0U;
    int start = 0;
    while (start < csv.length() && count < capacity) {
        int end = csv.indexOf(',', start);
        if (end < 0) end = csv.length();
        String token = csv.substring(start, end);
        token.trim();
        if (token.length() > 0) {
            char* end_ptr = nullptr;
            const unsigned long value = std::strtoul(token.c_str(), &end_ptr, 0);
            if (end_ptr != token.c_str() && value <= 0xFFFFUL) out_values[count++] = static_cast<uint16_t>(value);
        }
        start = end + 1;
    }
    return count;
}

size_t parseObservationCsv(const String& csv, ObservationKey* out_keys, size_t capacity) {
    if (out_keys == nullptr || capacity == 0U) return 0U;
    size_t count = 0U;
    int start = 0;
    while (start < csv.length() && count < capacity) {
        int end = csv.indexOf(',', start);
        if (end < 0) end = csv.length();
        String token = csv.substring(start, end);
        token.trim();
        if (token.length() > 0) {
            int sep = token.indexOf(':');
            String id_text = (sep >= 0) ? token.substring(0, sep) : token;
            String dir_text = (sep >= 0) ? token.substring(sep + 1) : "A_TO_B";
            char* end_ptr = nullptr;
            const unsigned long can_id = std::strtoul(id_text.c_str(), &end_ptr, 0);
            if (end_ptr != id_text.c_str()) {
                out_keys[count].can_id = static_cast<uint32_t>(can_id);
                out_keys[count].direction = parseDirectionFromText(dir_text, Direction::A_TO_B);
                ++count;
            }
        }
        start = end + 1;
    }
    return count;
}

bool bodyContains(const String& body, const char* token) {
    return body.indexOf(token) >= 0;
}

bool findRuleIdByIdentity(uint32_t can_id, Direction direction, uint16_t start_bit, uint8_t bit_length, uint16_t& out_rule_id);
bool findRuleIdByRawIdentity(uint32_t can_id, Direction direction, uint16_t& out_rule_id);

// API handlers
void handleStatus();
void handleFrameCache();
void handleSignalCache();
void handleSignalCatalog();
void handleObserve();
void handleRuleStage();
void handleRulesAction();
void handleRulesList();
void handleRuleValue();
void handleRuleEnable();
void handleRulePackageRead();
void handleRulePackageWrite();
void handleRulePackageSelect();
void handleReplayLoad();
void handleReplayControl();
void handleReplaySend();
void handleDbcUpload();
void handleDbcAutoload();
void handleDbcSelect();
void handleNotFound();
void configureHttpServer();
void startAccessPoint();

bool initBusA();
bool initBusB();
bool readBusA(CanFrame& out_frame);
bool readBusB(CanFrame& out_frame);
bool writeBusA(const CanFrame& frame);
bool writeBusB(const CanFrame& frame);
bool txDriver(Direction tx_direction, const CanFrame& frame);
bool diagnosticTxDriver(Direction tx_direction, const CanFrame& frame);
bool replayTxBridge(const CanFrame& frame, ReplayDispatchMode dispatch_mode);
void pollCanIngress();
void monitorCanHealth(uint32_t now_ms);
void publishGatewayLogSnapshot();
void flushGatewayLogSnapshot();
void initializeParkedPower();
bool performParkedPowerBootProbe();
bool parkedPowerHostBusy();
void monitorParkedPower(uint32_t now_ms, bool host_busy, bool application_busy);
bool enterParkedSleep(ParkedPowerDecision decision);
void handleParkedCanQuiesce(uint32_t now_ms);

void canRuntimeTask(void* context);
void twaiAlertTask(void* context);
void uiRuntimeTask(void* context);

}  // namespace

namespace {

bool recoveryDeadlineReached(uint32_t now_ms, uint32_t deadline_ms) {
    return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool setBusAReady(bool ready) {
    const uint8_t next = ready ? 1U : 0U;
    const uint8_t previous = bus_a_ready.exchange(next, std::memory_order_acq_rel);
    if (previous == next) return false;
    // B_TO_A frames are waiting to transmit onto Bus A. Discard them both when
    // that destination disappears and immediately before a recovered bus is
    // allowed to transmit, so old counters/control values cannot burst later.
    gateway.purgeDirection(Direction::B_TO_A);
    return true;
}

bool setBusBReady(bool ready) {
    const uint8_t next = ready ? 1U : 0U;
    const uint8_t previous = bus_b_ready.exchange(next, std::memory_order_acq_rel);
    if (previous == next) return false;
    // A_TO_B frames target the MCP2515 side and have the same stale-on-recovery
    // contract as Bus A traffic above.
    gateway.purgeDirection(Direction::A_TO_B);
    return true;
}

[[noreturn]] void failCanRuntimeStartup(const char* reason) {
    Serial.printf("[runtime] fatal CAN runtime startup failure: %s; restarting\n",
                  reason == nullptr ? "unknown" : reason);
    // Never present a healthy UI while the physical bridge has no owner. A
    // restart retries the early, allocation-light CAN startup from a clean
    // state and is safer than leaving both vehicle segments disconnected.
    esp_restart();
    for (;;) delay(1000);
}

bool initBusA() {
    // Deep-sleep EXT0 leaves the RX pad under RTC control. Relinquish it before
    // TWAI claims the pin again on every CAN/timer wake reset.
    rtc_gpio_deinit(static_cast<gpio_num_t>(kBusARxPin));
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        static_cast<gpio_num_t>(kBusATxPin),
        static_cast<gpio_num_t>(kBusARxPin),
        TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    g_config.tx_queue_len = 64;
    g_config.rx_queue_len = 128;

    esp_err_t err = ESP_OK;
    if (twai_driver_installed.load(std::memory_order_acquire) == 0U) {
        err = twai_driver_install(&g_config, &t_config, &f_config);
        if (err != ESP_OK) {
            Serial.printf("[can-a] driver install failed: %s\n", esp_err_to_name(err));
            return false;
        }
        twai_driver_installed.store(1U, std::memory_order_release);
    }
    err = twai_start();
    if (err != ESP_OK) {
        Serial.printf("[can-a] start failed: %s\n", esp_err_to_name(err));
        return false;
    }
    twai_runtime_state.store(static_cast<uint8_t>(TWAI_STATE_RUNNING), std::memory_order_release);
    twai_recovery_active = false;

    Serial.println("[can-a] TWAI started on pins TX=7 RX=6 @500kbps");
    return true;
}

constexpr uint32_t twaiWakeAlerts() {
    return TWAI_ALERT_RX_DATA | TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_BUS_OFF |
        TWAI_ALERT_RECOVERY_IN_PROGRESS | TWAI_ALERT_BUS_RECOVERED |
        TWAI_ALERT_ABOVE_ERR_WARN | TWAI_ALERT_ERR_PASS;
}

void configureTwaiWakeAlerts() {
    if (twai_driver_installed.load(std::memory_order_acquire) == 0U) return;
    uint32_t pending = 0U;
    static_cast<void>(twai_reconfigure_alerts(twaiWakeAlerts(), &pending));
}

void ARDUINO_ISR_ATTR notifyCanTaskFromMcp() {
    BaseType_t higher_priority_task_woken = pdFALSE;
    TaskHandle_t task = can_task_handle;
    if (task != nullptr) vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) portYIELD_FROM_ISR();
}

void twaiAlertTask(void* /*context*/) {
    for (;;) {
        if (parked_alert_stop_requested.load(std::memory_order_acquire) != 0U) break;
        uint32_t alerts = 0U;
        if (twai_driver_installed.load(std::memory_order_acquire) != 0U &&
            twai_read_alerts(&alerts, pdMS_TO_TICKS(50)) == ESP_OK) {
            if (can_task_handle != nullptr) xTaskNotifyGive(can_task_handle);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    parked_alert_stopped.store(1U, std::memory_order_release);
    if (can_task_handle != nullptr) xTaskNotifyGive(can_task_handle);
    vTaskSuspend(nullptr);
}

bool initBusB() {
    pinMode(kMcpRstPin, OUTPUT);
    digitalWrite(kMcpRstPin, HIGH);
    delay(10);
    digitalWrite(kMcpRstPin, LOW);
    delay(10);
    digitalWrite(kMcpRstPin, HIGH);
    delay(10);

    SPI.begin(kMcpSclkPin, kMcpMisoPin, kMcpMosiPin, kMcpCsPin);
    if (can_mcp.reset() != MCP2515::ERROR_OK) {
        Serial.println("[can-b] MCP2515 reset failed");
        return false;
    }
    if (can_mcp.setBitrate(CAN_500KBPS) != MCP2515::ERROR_OK) {
        Serial.println("[can-b] MCP2515 bitrate set failed");
        return false;
    }
    if (can_mcp.setNormalMode() != MCP2515::ERROR_OK) {
        Serial.println("[can-b] MCP2515 normal mode failed");
        return false;
    }
    mcp_error_flags.store(0U, std::memory_order_release);

    Serial.println("[can-b] MCP2515 started @500kbps");
    return true;
}

bool readBusA(CanFrame& out_frame) {
    twai_message_t rx = {};
    if (twai_receive(&rx, 0) != ESP_OK) return false;

    out_frame.id = rx.identifier;
    out_frame.dlc = (rx.data_length_code <= 8U) ? rx.data_length_code : 8U;
    for (uint8_t i = 0; i < out_frame.dlc; ++i) out_frame.data[i] = rx.data[i];
    out_frame.timestamp_us = micros();
    out_frame.direction = Direction::A_TO_B;
    return true;
}

bool readBusB(CanFrame& out_frame) {
    struct can_frame frame = {};
    if (can_mcp.readMessage(&frame) != MCP2515::ERROR_OK) return false;

    out_frame.id = frame.can_id & CAN_EFF_MASK;
    out_frame.dlc = (frame.can_dlc <= 8U) ? frame.can_dlc : 8U;
    for (uint8_t i = 0; i < out_frame.dlc; ++i) out_frame.data[i] = frame.data[i];
    out_frame.timestamp_us = micros();
    out_frame.direction = Direction::B_TO_A;
    return true;
}

bool writeBusA(const CanFrame& frame) {
    if (bus_a_ready.load(std::memory_order_acquire) == 0U) return false;

    twai_message_t tx = {};
    tx.identifier = frame.id;
    tx.extd = (frame.id > 0x7FFU) ? 1 : 0;
    tx.rtr = 0;
    tx.data_length_code = (frame.dlc <= 8U) ? frame.dlc : 8U;
    for (uint8_t i = 0; i < tx.data_length_code; ++i) tx.data[i] = frame.data[i];

    // GatewayCore owns the retry queue. Never block the high-priority CAN task
    // waiting for a driver mailbox.
    return twai_transmit(&tx, 0) == ESP_OK;
}

bool writeBusB(const CanFrame& frame) {
    if (bus_b_ready.load(std::memory_order_acquire) == 0U) return false;

    struct can_frame tx = {};
    tx.can_id = frame.id & CAN_EFF_MASK;
    if (frame.id > 0x7FFU) tx.can_id |= CAN_EFF_FLAG;
    tx.can_dlc = (frame.dlc <= 8U) ? frame.dlc : 8U;
    for (uint8_t i = 0; i < tx.can_dlc; ++i) tx.data[i] = frame.data[i];

    // Preserve TXB2 only while the diagnostic engine has an actual MCP-bound
    // frame queued. Physical forwarding still runs first and may occupy TXB0/1;
    // outside that narrow window it retains all three hardware buffers.
    if (diagnosticTransportTxPending(Direction::A_TO_B)) {
        const uint8_t status = can_mcp.getStatus();
        constexpr uint8_t kTxB0Request = 0x04U;
        constexpr uint8_t kTxB1Request = 0x10U;
        if ((status & kTxB0Request) == 0U) {
            return can_mcp.sendMessage(MCP2515::TXB0, &tx) == MCP2515::ERROR_OK;
        }
        if ((status & kTxB1Request) == 0U) {
            return can_mcp.sendMessage(MCP2515::TXB1, &tx) == MCP2515::ERROR_OK;
        }
        return false;
    }

    // GatewayCore owns the post-mutation retry queue, so a busy MCP mailbox is
    // returned immediately and the exact prepared frame is retried next tick.
    return can_mcp.sendMessage(&tx) == MCP2515::ERROR_OK;
}

bool txDriver(Direction tx_direction, const CanFrame& frame) {
    return (tx_direction == Direction::A_TO_B)
        ? writeBusB(frame)
        : writeBusA(frame);
}

bool diagnosticTxDriver(Direction tx_direction, const CanFrame& frame) {
    CanFrame transmitted = frame;
    transmitted.direction = tx_direction;
    if (tx_direction == Direction::B_TO_A) {
        const bool sent = writeBusA(frame);
        if (sent) can_trace.pushTransmit(transmitted, true);
        return sent;
    }
    if (bus_b_ready.load(std::memory_order_acquire) == 0U) return false;

    struct can_frame tx = {};
    tx.can_id = frame.id & CAN_EFF_MASK;
    if (frame.id > 0x7FFU) tx.can_id |= CAN_EFF_FLAG;
    tx.can_dlc = (frame.dlc <= 8U) ? frame.dlc : 8U;
    for (uint8_t i = 0; i < tx.can_dlc; ++i) tx.data[i] = frame.data[i];

    // While an MCP-bound diagnostic frame is pending, physical forwarding
    // yields TXB2 and this path owns it. Never overwrite TXB2 while TXREQ is
    // asserted; the transport retries non-blockingly on later CAN-task ticks.
    if ((can_mcp.getStatus() & 0x40U) != 0U) return false;
    const bool sent = can_mcp.sendMessage(MCP2515::TXB2, &tx) == MCP2515::ERROR_OK;
    if (sent) can_trace.pushTransmit(transmitted, true);
    return sent;
}

bool replayTxBridge(const CanFrame& frame, ReplayDispatchMode dispatch_mode) {
    // CSV timestamps are a relative playback schedule, not live CAN ingress
    // time. Stamp a copy at dispatch so SignalCache freshness and application
    // behavior use the same rollover-safe micros() domain as physical frames.
    const CanFrame live_frame = liveReplayIngressFrame(frame, micros());
    return gateway.injectReplayFrame(
        live_frame, dispatch_mode == ReplayDispatchMode::DRY_RUN);
}

void pollCanIngress() {
    CanFrame frame{};
    size_t processed_a = 0U;
    size_t processed_b = 0U;
    bool saturated_a = false;
    bool saturated_b = false;

    // MCP2515 has only two hardware RX buffers while TWAI has a deep software
    // queue. Interleave both inputs with MCP first so a Bus B backlog cannot
    // starve zero-STmin diagnostic or physical frames.
    for (;;) {
        bool progressed = false;
        if (!saturated_b && processed_b < kMaxPollFramesPerBus &&
            bus_b_ready.load(std::memory_order_acquire) != 0U && readBusB(frame)) {
            last_physical_ingress_ms.store(millis(), std::memory_order_release);
            const bool diagnostic_consumed = diagnosticTransportObservePhysicalFrame(frame);
            const bool gateway_accepted = diagnostic_consumed || gateway.onFrameReceivedFromIsr(frame);
            if (!gateway_accepted) {
                // Stop draining this saturated direction for this bounded pass,
                // but always give the independent bus its turn below.
                saturated_b = true;
            }
            can_trace.pushIngress(frame, diagnostic_consumed, !gateway_accepted);
            ingress_b_frames.fetch_add(1U, std::memory_order_relaxed);
            ++processed_b;
            progressed = true;
        }

        if (!saturated_a && processed_a < kMaxPollFramesPerBus &&
            bus_a_ready.load(std::memory_order_acquire) != 0U && readBusA(frame)) {
            const uint32_t frame_now_ms = millis();
            last_physical_ingress_ms.store(frame_now_ms, std::memory_order_release);
            parked_power.observePrimaryFrame(frame_now_ms);
            if (const ApplicationExtension* app = application_registry.extension();
                app != nullptr && app->decodeParkedPowerIgnition != nullptr) {
                bool ignition_on = false;
                uint8_t source = 0U;
                if (app->decodeParkedPowerIgnition(&frame, &ignition_on, &source)) {
                    parked_power.observeIgnition(ignition_on, source, frame_now_ms);
                }
            }
            const bool diagnostic_consumed = diagnosticTransportObservePhysicalFrame(frame);
            const bool gateway_accepted = diagnostic_consumed || gateway.onFrameReceivedFromIsr(frame);
            if (!gateway_accepted) {
                saturated_a = true;
            }
            can_trace.pushIngress(frame, diagnostic_consumed, !gateway_accepted);
            ingress_a_frames.fetch_add(1U, std::memory_order_relaxed);
            ++processed_a;
            progressed = true;
        }

        if (!progressed ||
            (processed_a >= kMaxPollFramesPerBus && processed_b >= kMaxPollFramesPerBus)) break;
    }

    if (digitalRead(kMcpIntPin) == LOW) {
        can_mcp.clearERRIF();
        can_mcp.clearMERR();
    }
}

void monitorCanHealth(uint32_t now_ms) {
    if (now_ms - last_can_health_poll_ms < kCanHealthPollMs) return;
    last_can_health_poll_ms = now_ms;

    twai_status_info_t twai_status{};
    if (twai_driver_installed.load(std::memory_order_acquire) != 0U &&
        twai_get_status_info(&twai_status) == ESP_OK) {
        twai_runtime_state.store(static_cast<uint8_t>(twai_status.state), std::memory_order_release);
        twai_rx_missed.store(twai_status.rx_missed_count, std::memory_order_relaxed);
        twai_rx_overruns.store(twai_status.rx_overrun_count, std::memory_order_relaxed);

        if (twai_status.state == TWAI_STATE_RUNNING) {
            setBusAReady(true);
            twai_recovery_active = false;
        } else if (twai_status.state == TWAI_STATE_BUS_OFF) {
            if (setBusAReady(false)) {
                next_twai_recovery_attempt_ms = now_ms + kCanInitialRecoveryRetryMs;
            }
            if (!twai_recovery_active && twai_initiate_recovery() == ESP_OK) {
                twai_recovery_active = true;
                twai_recovery_attempts.fetch_add(1U, std::memory_order_relaxed);
            }
        } else if (twai_status.state == TWAI_STATE_RECOVERING) {
            if (setBusAReady(false)) {
                next_twai_recovery_attempt_ms = now_ms + kCanInitialRecoveryRetryMs;
            }
            twai_recovery_active = true;
        } else if (twai_status.state == TWAI_STATE_STOPPED) {
            if (setBusAReady(false)) {
                next_twai_recovery_attempt_ms = now_ms + kCanInitialRecoveryRetryMs;
            }
            if (twai_recovery_active ||
                recoveryDeadlineReached(now_ms, next_twai_recovery_attempt_ms)) {
                next_twai_recovery_attempt_ms = now_ms + kTwaiRecoveryRetryMs;
                if (!twai_recovery_active) {
                    twai_recovery_attempts.fetch_add(1U, std::memory_order_relaxed);
                }
                if (twai_start() == ESP_OK) {
                    twai_recovery_active = false;
                    twai_restarts.fetch_add(1U, std::memory_order_relaxed);
                    twai_runtime_state.store(
                        static_cast<uint8_t>(TWAI_STATE_RUNNING), std::memory_order_release);
                    setBusAReady(true);
                    configureTwaiWakeAlerts();
                }
            }
        } else {
            if (setBusAReady(false)) {
                next_twai_recovery_attempt_ms = now_ms + kCanInitialRecoveryRetryMs;
            }
        }
    } else if (twai_driver_installed.load(std::memory_order_acquire) == 0U &&
               recoveryDeadlineReached(now_ms, next_twai_recovery_attempt_ms)) {
        setBusAReady(false);
        next_twai_recovery_attempt_ms = now_ms + kTwaiRecoveryRetryMs;
        twai_recovery_attempts.fetch_add(1U, std::memory_order_relaxed);
        // Only retry an install that never succeeded. Never uninstall beneath
        // twaiAlertTask: ESP-IDF requires that no task be blocked in
        // twai_read_alerts() while a driver is uninstalled.
        if (initBusA()) {
            setBusAReady(true);
            twai_restarts.fetch_add(1U, std::memory_order_relaxed);
            configureTwaiWakeAlerts();
        }
    }

    uint8_t flags = 0U;
    if (bus_b_ready.load(std::memory_order_acquire) != 0U) {
        flags = can_mcp.getErrorFlags();
        mcp_error_flags.store(flags, std::memory_order_release);
        if ((flags & (MCP2515::EFLG_RX0OVR | MCP2515::EFLG_RX1OVR)) != 0U) {
            mcp_rx_overruns.fetch_add(1U, std::memory_order_relaxed);
            can_mcp.clearRXnOVRFlags();
        }
        // reset() enables ERRIF/MERRF on GPIO8. Health polling owns those flags;
        // clear them after sampling so INT can return high and the next RX event
        // creates a new falling edge for the CAN-task wakeup.
        can_mcp.clearERRIF();
        can_mcp.clearMERR();
        if ((flags & MCP2515::EFLG_TXBO) != 0U) {
            if (setBusBReady(false)) {
                next_mcp_recovery_attempt_ms = now_ms + kCanInitialRecoveryRetryMs;
            }
        }
    }

    if (bus_b_ready.load(std::memory_order_acquire) == 0U &&
        recoveryDeadlineReached(now_ms, next_mcp_recovery_attempt_ms)) {
        next_mcp_recovery_attempt_ms = now_ms + kMcpRecoveryRetryMs;
        if (initBusB()) {
            setBusBReady(true);
            mcp_recoveries.fetch_add(1U, std::memory_order_relaxed);
        }
    }
}

void publishGatewayLogSnapshot() {
    const GatewayStats& stats = gateway.canOwnerStats();
    gateway_log_drops_boot.store(stats.rx_drops_boot, std::memory_order_relaxed);
    gateway_log_drops_run.store(stats.rx_drops_run, std::memory_order_relaxed);
    gateway_log_deferred_a_to_b.store(stats.egress_deferred_frames_a_to_b, std::memory_order_relaxed);
    gateway_log_deferred_b_to_a.store(stats.egress_deferred_frames_b_to_a, std::memory_order_relaxed);
    gateway_log_egress_a_to_b.store(stats.egress_queue_depth_a_to_b, std::memory_order_relaxed);
    gateway_log_egress_b_to_a.store(stats.egress_queue_depth_b_to_a, std::memory_order_relaxed);
    gateway_log_passive.store(stats.passive_fast_path_frames, std::memory_order_relaxed);
    gateway_log_decoded.store(stats.observed_decoded_frames, std::memory_order_relaxed);
    gateway_log_active_rules.store(static_cast<uint32_t>(mutation_engine.activeCount()), std::memory_order_relaxed);
    gateway_log_stack_free.store(can_stack_min_free.load(std::memory_order_acquire), std::memory_order_relaxed);
    gateway_log_pending.store(1U, std::memory_order_release);
}

void flushGatewayLogSnapshot() {
    if (gateway_log_pending.exchange(0U, std::memory_order_acq_rel) == 0U) return;
    Serial.printf(
        "[gateway] drops_boot=%lu drops_run=%lu deferred_a_b=%lu deferred_b_a=%lu egress_a_b=%lu egress_b_a=%lu passive=%lu decoded=%lu active_rules=%lu can_stack_free=%lu\n",
        static_cast<unsigned long>(gateway_log_drops_boot.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(gateway_log_drops_run.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(gateway_log_deferred_a_to_b.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(gateway_log_deferred_b_to_a.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(gateway_log_egress_a_to_b.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(gateway_log_egress_b_to_a.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(gateway_log_passive.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(gateway_log_decoded.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(gateway_log_active_rules.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(gateway_log_stack_free.load(std::memory_order_relaxed)));
}

void appendSessionHealthSnapshot(uint32_t now_ms) {
    SessionLogLockGuard log_lock(pdMS_TO_TICKS(5));
    if (!log_lock.locked()) return;
    if (!session_log.recording() || now_ms - last_session_health_ms < 1000U) return;
    last_session_health_ms = now_ms;
    const GatewayStats stats = gateway.snapshotStats();
    char json[1025] = {};
    const int count = std::snprintf(
        json, sizeof(json),
        "{\"uptimeMs\":%lu,\"heapFree\":%u,\"heapMin\":%u,"
        "\"psramFree\":%u,\"psramTotal\":%u,\"fsUsed\":%u,\"fsTotal\":%u,"
        "\"apClients\":%u,\"fps\":%u,\"busAReady\":%s,\"busBReady\":%s,"
        "\"twaiState\":\"%s\",\"twaiRestarts\":%lu,\"twaiMissed\":%lu,"
        "\"twaiOverruns\":%lu,\"mcpFlags\":%u,\"mcpRecoveries\":%lu,"
        "\"mcpOverruns\":%lu,\"ingressA\":%lu,\"ingressB\":%lu,"
        "\"forwarded\":%lu,\"rxDrops\":%lu,\"txFailuresAtoB\":%lu,"
        "\"txFailuresBtoA\":%lu,\"rxQueue\":%u,\"egressAtoB\":%u,"
        "\"egressBtoA\":%u,\"fastPathUs\":%lu,\"activePathUs\":%lu,"
        "\"canStackFree\":%lu,\"appStackFree\":%lu,\"uiStackFree\":%lu,"
        "\"traceQueueCapacity\":%u}",
        static_cast<unsigned long>(now_ms), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
        ESP.getFreePsram(), ESP.getPsramSize(), static_cast<unsigned>(LittleFS.usedBytes()),
        static_cast<unsigned>(LittleFS.totalBytes()), static_cast<unsigned>(WiFi.softAPgetStationNum()),
        static_cast<unsigned>(frame_rate_fps.load(std::memory_order_acquire)),
        bus_a_ready.load(std::memory_order_acquire) ? "true" : "false",
        bus_b_ready.load(std::memory_order_acquire) ? "true" : "false",
        twaiStateName(twai_runtime_state.load(std::memory_order_acquire)),
        static_cast<unsigned long>(twai_restarts.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(twai_rx_missed.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(twai_rx_overruns.load(std::memory_order_relaxed)),
        static_cast<unsigned>(mcp_error_flags.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(mcp_recoveries.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(mcp_rx_overruns.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(ingress_a_frames.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(ingress_b_frames.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(stats.forwarded_frames),
        static_cast<unsigned long>(stats.rx_drops_run),
        static_cast<unsigned long>(stats.tx_failures_a_to_b),
        static_cast<unsigned long>(stats.tx_failures_b_to_a), stats.rx_queue_depth,
        stats.egress_queue_depth_a_to_b, stats.egress_queue_depth_b_to_a,
        static_cast<unsigned long>(stats.fast_path_latency_avg_us),
        static_cast<unsigned long>(stats.active_path_latency_avg_us),
        static_cast<unsigned long>(can_stack_min_free.load(std::memory_order_acquire)),
        static_cast<unsigned long>(application_stack_min_free.load(std::memory_order_acquire)),
        static_cast<unsigned long>(ui_stack_min_free.load(std::memory_order_acquire)),
        static_cast<unsigned>(can_trace.capacity()));
    if (count > 0 && static_cast<size_t>(count) < sizeof(json)) {
        static_cast<void>(session_log.appendAnnotation(
            "signalscope", "runtime", json, static_cast<size_t>(count), micros()));
    }
}

void appendSessionResourceContext() {
    SessionLogLockGuard log_lock(pdMS_TO_TICKS(5));
    if (!log_lock.locked()) return;
    if (!session_log.recording()) return;
    const DbcDatabase* const dbc = active_dbc.load(std::memory_order_acquire);
    String json = "{\"dbcPath\":\"" + escapeJsonString(active_dbc_path.c_str()) +
        "\",\"rulePackagePath\":\"" + escapeJsonString(active_rule_package_path.c_str()) +
        "\",\"dbcLoaded\":" + String(dbc != nullptr ? "true" : "false") +
        ",\"dbcMessages\":" + String(static_cast<uint32_t>(dbc == nullptr ? 0U : dbc->messageCount())) +
        ",\"dbcSignals\":" + String(static_cast<uint32_t>(dbc == nullptr ? 0U : dbc->signalCount())) +
        ",\"ruleEpoch\":" + String(mutation_engine.ruleEpoch()) +
        ",\"activeRules\":" + String(static_cast<uint32_t>(mutation_engine.activeCount())) +
        ",\"buildDate\":\"" __DATE__ "\",\"buildTime\":\"" __TIME__ "\"}";
    if (json.length() <= SessionLogRecorder::kMaximumAnnotationJsonBytes) {
        static_cast<void>(session_log.appendAnnotation(
            "signalscope", "resource_context", json.c_str(), json.length(), micros()));
    }
}

bool sendBufferedJson(int status, const char* body, size_t length) {
    if (body == nullptr || length == 0U) {
        server.send(500, "application/json", "{\"ok\":false,\"error\":\"json_body_empty\"}");
        return false;
    }

    // Arduino WebServer::sendContent() discards the socket write count. A
    // single large write can therefore advertise a complete Content-Length
    // while delivering only a prefix over a weak AP link. Bound every write,
    // verify progress, and close an incomplete response so the browser can
    // reconcile it with a fresh GET instead of trying to parse a partial body.
    constexpr size_t kChunkBytes = 2048U;
    constexpr uint32_t kNoProgressTimeoutMs = 2000U;
    server.setContentLength(length);
    server.send(status, "application/json", "");
    NetworkClient& client = server.client();
    size_t offset = 0U;
    uint32_t last_progress_ms = millis();
    while (offset < length && client.connected()) {
        const size_t chunk = std::min(kChunkBytes, length - offset);
        const size_t sent = client.write(
            reinterpret_cast<const uint8_t*>(body + offset), chunk);
        if (sent > 0U) {
            offset += std::min(sent, chunk);
            last_progress_ms = millis();
        } else if (millis() - last_progress_ms >= kNoProgressTimeoutMs) {
            break;
        }
        if (offset < length) vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (offset == length) return true;

    Serial.printf("[http] JSON body incomplete status=%d sent=%u expected=%u\n",
                  status, static_cast<unsigned>(offset), static_cast<unsigned>(length));
    client.stop();
    return false;
}

void configureHttpServer() {
    server.on("/", HTTP_GET, []() {
        if (fs_mounted.load(std::memory_order_acquire) == 0U) {
            server.send(500, "text/plain", "LittleFS not mounted");
            return;
        }

        if (!serveStaticFile(ui_index_path)) {
            server.send(500, "text/plain", "UI index missing");
            return;
        }
    });

    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/app/status", HTTP_GET, []() {
        const ApplicationExtension* app = application_registry.extension();
        if (app == nullptr) {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"application_not_installed\"}");
            return;
        }
        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        char json[4096] = {};
        if (!app->writeStatusJson(json, sizeof(json))) {
            app_lock.release();
            server.send(500, "application/json", "{\"ok\":false,\"error\":\"application_status_failed\"}");
            return;
        }
        app_lock.release();
        server.send(200, "application/json", json);
    });
    server.on("/api/app/config", HTTP_POST, []() {
        const ApplicationExtension* app = application_registry.extension();
        if (app == nullptr || app->configure == nullptr) {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"application_config_unavailable\"}");
            return;
        }
        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        char json[4096] = {};
        const bool ok = app->configure(server.arg("key").c_str(), server.arg("value").c_str(), json, sizeof(json));
        if (!ok) {
            server.send(422, "application/json", json[0] == '\0' ? "{\"ok\":false}" : json);
            return;
        }

        const ApplicationResourceApplyResult resource_result =
            applyApplicationResourceTransaction();
        if (resource_result == ApplicationResourceApplyResult::DATABASE_FAILED) {
            app_lock.release();
            server.send(422, "application/json", "{\"ok\":false,\"error\":\"application_database_load_failed\"}");
            return;
        }
        if (resource_result == ApplicationResourceApplyResult::RULE_PACKAGE_FAILED) {
            app_lock.release();
            server.send(422, "application/json", "{\"ok\":false,\"error\":\"application_rule_package_load_failed\"}");
            return;
        }
        // Staged applications only expose the committed value after their
        // resources have loaded, so refresh the response after the commit.
        if (app->finishConfigure != nullptr && app->writeStatusJson != nullptr) {
            json[0] = '\0';
            static_cast<void>(app->writeStatusJson(json, sizeof(json)));
        }
        app_lock.release();
        server.send(200, "application/json", json[0] == '\0' ? "{\"ok\":true}" : json);
    });
    server.on("/api/app/resource", HTTP_GET, []() {
        const ApplicationExtension* app = application_registry.extension();
        const String name = server.arg("name");
        if (app == nullptr || app->resourceCapacity == nullptr || app->readResource == nullptr) {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"application_resource_unavailable\"}");
            return;
        }
        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        const size_t capacity = app->resourceCapacity(name.c_str());
        if (capacity == 0U || capacity > 96U * 1024U) {
            app_lock.release();
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"application_resource_not_found\"}");
            return;
        }
        char* output = static_cast<char*>(allocateRuntimeMemory(capacity));
        if (output == nullptr) {
            app_lock.release();
            server.send(507, "application/json", "{\"ok\":false,\"error\":\"application_resource_allocation_failed\"}");
            return;
        }
        output[0] = '\0';
        size_t written = 0U;
        const bool ok = app->readResource(name.c_str(), output, capacity, &written);
        if (!ok || written >= capacity) {
            freeRuntimeMemory(output);
            app_lock.release();
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"application_resource_read_failed\"}");
            return;
        }
        app_lock.release();
        static_cast<void>(sendBufferedJson(200, output, written));
        freeRuntimeMemory(output);
    });
    server.on("/api/app/resource", HTTP_POST, []() {
        const ApplicationExtension* app = application_registry.extension();
        const String name = server.arg("name");
        if (app == nullptr || app->resourceCapacity == nullptr || app->writeResource == nullptr) {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"application_resource_unavailable\"}");
            return;
        }
        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        const size_t capacity = app->resourceCapacity(name.c_str());
        if (capacity == 0U || capacity > 96U * 1024U) {
            app_lock.release();
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"application_resource_not_found\"}");
            return;
        }
        const String input = server.arg("plain");
        if (input.length() == 0U || input.length() >= capacity) {
            app_lock.release();
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"application_resource_body_invalid\"}");
            return;
        }
        char* output = static_cast<char*>(allocateRuntimeMemory(capacity));
        if (output == nullptr) {
            app_lock.release();
            server.send(507, "application/json", "{\"ok\":false,\"error\":\"application_resource_allocation_failed\"}");
            return;
        }
        output[0] = '\0';
        size_t written = 0U;
        const bool ok = app->writeResource(
            name.c_str(), input.c_str(), static_cast<size_t>(input.length()), output, capacity, &written);
        if (written == 0U && output[0] != '\0') written = std::strlen(output);
        if (written >= capacity) {
            freeRuntimeMemory(output);
            app_lock.release();
            server.send(500, "application/json", "{\"ok\":false,\"error\":\"application_resource_response_too_large\"}");
            return;
        }
        if (written == 0U) {
            const char* fallback = ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"application_resource_write_failed\"}";
            std::snprintf(output, capacity, "%s", fallback);
            written = std::strlen(output);
        }
        if (ok && name == "diagnostics") {
            // Diagnostic commands are not idempotent. Return only a delivery
            // receipt; the browser obtains the authoritative job/result state
            // with GET and never resends a command after an ambiguous response.
            constexpr const char* kAccepted =
                "{\"ok\":true,\"accepted\":true,\"resource\":\"diagnostics\"}";
            std::snprintf(output, capacity, "%s", kAccepted);
            written = std::strlen(output);
        }
        app_lock.release();
        static_cast<void>(sendBufferedJson(ok ? 200 : 422, output, written));
        freeRuntimeMemory(output);
    });
    server.on("/api/log", HTTP_GET, []() {
        SessionLogLockGuard log_lock(pdMS_TO_TICKS(1000));
        if (!log_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"recorder_busy\"}");
            return;
        }
        char json[1024] = {};
        if (!session_log.writeStatusJson(json, sizeof(json))) {
            server.send(500, "application/json", "{\"ok\":false,\"error\":\"recorder_status_failed\"}");
            return;
        }
        log_lock.release();
        server.send(200, "application/json", json);
    });
    server.on("/api/log", HTTP_POST, []() {
        SessionLogLockGuard log_lock(pdMS_TO_TICKS(1000));
        if (!log_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"recorder_busy\"}");
            return;
        }
        const String action = server.arg("action");
        const char* error = nullptr;
        bool ok = false;
        if (action == "start") {
            CanTraceScope scope{};
            if (!CanTraceQueue::parseScope(server.arg("scope").c_str(), scope)) {
                server.send(400, "application/json", "{\"ok\":false,\"error\":\"recording_scope_invalid\"}");
                return;
            }
            const String durable_value = server.arg("durable");
            const bool durable = durable_value == "1" || durable_value == "true" ||
                durable_value == "checkpoint";
            const ApplicationExtension* app = application_registry.extension();
            ok = session_log.start(
                can_trace, scope, durable, app == nullptr ? "" : app->id, error);
            last_session_health_ms = 0U;
            if (ok) appendSessionResourceContext();
        } else if (action == "stop") {
            ok = session_log.requestStop(can_trace, "user");
            if (!ok) error = "recording_not_active";
        } else if (action == "delete" || action == "clear") {
            ok = session_log.clear();
            if (!ok) error = "recording_clear_failed";
        } else if (action == "retry") {
            ok = session_log.retrySave(error);
        } else {
            error = "recording_action_invalid";
        }
        if (!ok) {
            char json[160] = {};
            std::snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}",
                          error == nullptr ? "recording_command_failed" : error);
            server.send(422, "application/json", json);
            return;
        }
        char json[1024] = {};
        static_cast<void>(session_log.writeStatusJson(json, sizeof(json)));
        log_lock.release();
        server.send(200, "application/json", json);
    });
    server.on("/api/log/download", HTTP_GET, []() {
        SessionLogLockGuard log_lock(pdMS_TO_TICKS(1000));
        if (!log_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"recorder_busy\"}");
            return;
        }
        if (!session_log.downloadReady() || !LittleFS.exists(SessionLogRecorder::kLogPath)) {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"recording_not_ready\"}");
            return;
        }
        File file = LittleFS.open(SessionLogRecorder::kLogPath, "r");
        if (!file) {
            server.send(500, "application/json", "{\"ok\":false,\"error\":\"recording_open_failed\"}");
            return;
        }
        log_lock.release();
        server.sendHeader("Content-Disposition", "attachment; filename=signalscope-session.sslog");
        server.sendHeader("Cache-Control", "no-store");
        server.streamFile(file, "application/octet-stream");
        file.close();
    });
    server.on("/api/frame_cache", HTTP_GET, handleFrameCache);
    server.on("/api/signal_cache", HTTP_GET, handleSignalCache);
    server.on("/api/signal_catalog", HTTP_GET, handleSignalCatalog);
    server.on("/api/observe", HTTP_POST, handleObserve);

    server.on("/api/rules/stage", HTTP_POST, handleRuleStage);
    server.on("/api/rules", HTTP_POST, handleRulesAction);
    server.on("/api/rules", HTTP_GET, handleRulesList);
    server.on("/api/rules/value", HTTP_POST, handleRuleValue);
    server.on("/api/rules/enable", HTTP_POST, handleRuleEnable);
    server.on("/api/rules/package", HTTP_GET, handleRulePackageRead);
    server.on("/api/rules/package", HTTP_POST, handleRulePackageWrite);
    server.on("/api/rules/select", HTTP_POST, handleRulePackageSelect);

    // Backward-compatible paths
    server.on("/api/mutations/stage", HTTP_POST, handleRuleStage);
    server.on("/api/mutations", HTTP_POST, handleRulesAction);
    server.on("/api/mutations/toggle", HTTP_POST, []() {
        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        const bool enabled = parseBoolText(server.arg("enabled"), true);

        uint16_t rule_id = 0U;
        uint32_t expected_epoch = 0U;
        const int32_t explicit_rule_id = parseIntArg("rule_id", -1);
        if (explicit_rule_id >= 0 && explicit_rule_id < static_cast<int32_t>(MutationEngine::kMaxRules)) {
            rule_id = static_cast<uint16_t>(explicit_rule_id);
            expected_epoch = parseUIntArg("rule_epoch", parseUIntArg("epoch", 0U));
            if (expected_epoch != mutation_engine.ruleEpoch()) {
                server.send(409, "application/json", "{\"ok\":false,\"error\":\"stale_rule_handle\"}");
                return;
            }
        } else {
            const uint32_t can_id = parseUIntArg("can_id", 0U);
            const Direction direction = parseDirectionFromText(server.arg("direction"), Direction::A_TO_B);
            const bool is_raw = server.hasArg("kind") && server.arg("kind") == "RAW_MASK";
            if (is_raw) {
                if (!findRuleIdByRawIdentity(can_id, direction, rule_id)) {
                    server.send(404, "application/json", "{\"ok\":false,\"error\":\"mutation_not_found\"}");
                    return;
                }
            } else {
                const uint16_t start_bit = static_cast<uint16_t>(parseUIntArg("start_bit", 0U));
                const uint8_t bit_length = static_cast<uint8_t>(parseUIntArg("length", 0U));
                if (!findRuleIdByIdentity(can_id, direction, start_bit, bit_length, rule_id)) {
                    server.send(404, "application/json", "{\"ok\":false,\"error\":\"mutation_not_found\"}");
                    return;
                }
            }
            expected_epoch = mutation_engine.ruleEpoch();
        }

        const bool ok = mutation_engine.enableRule(rule_id, enabled, expected_epoch);
        if (!ok) {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"rule_not_found\"}");
            return;
        }
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/replay", HTTP_POST, handleReplayControl);
    server.on("/api/replay/load", HTTP_POST, handleReplayLoad);
    server.on("/api/replay/send", HTTP_POST, handleReplaySend);
    server.on("/api/dbc", HTTP_POST, handleDbcUpload);
    server.on("/api/dbc/autoload", HTTP_POST, handleDbcAutoload);
    server.on("/api/dbc/select", HTTP_POST, handleDbcSelect);

    server.onNotFound(handleNotFound);
    server.begin();
}

void startAccessPoint() {
    WiFi.mode(WIFI_MODE_AP);
    if (!WiFi.softAP(kApSsid, kApPassword)) {
        Serial.println("[wifi] AP start failed");
        return;
    }
    const IPAddress ap_ip = WiFi.softAPIP();
    Serial.printf("[wifi] AP started: SSID=%s PASS=%s IP=%s\n", kApSsid, kApPassword, ap_ip.toString().c_str());
}

ParkedPowerWakeCause parkedPowerWakeCause() {
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0: return ParkedPowerWakeCause::CanRx;
    case ESP_SLEEP_WAKEUP_EXT1: return ParkedPowerWakeCause::Gpio;
    case ESP_SLEEP_WAKEUP_TIMER: return ParkedPowerWakeCause::Timer;
    case ESP_SLEEP_WAKEUP_UNDEFINED: return ParkedPowerWakeCause::PowerOnOrReset;
    default: return ParkedPowerWakeCause::Other;
    }
}

void initializeParkedPower() {
    ParkedPowerPolicy policy{};
    if (const ApplicationExtension* app = application_registry.extension();
        app != nullptr && app->loadParkedPowerPolicy != nullptr) {
        if (!app->loadParkedPowerPolicy(&policy)) policy = {};
    }
    parked_power.initialize(
        policy, false, parkedPowerWakeCause(),
        parked_deep_sleep_entries, millis());
    ParkedPowerStatus status{};
    parked_power.snapshot(millis(), status);
    Serial.printf(
        "[power] available=%d enabled=%d active=%d override=%d wake=%s delay=%lums probe=%lums\n",
        status.available ? 1 : 0, status.enabled ? 1 : 0, status.active ? 1 : 0,
        status.maintenance_override ? 1 : 0, parkedPowerWakeCauseName(status.wake_cause),
        static_cast<unsigned long>(status.sleep_delay_ms),
        static_cast<unsigned long>(status.boot_probe_ms));
}

bool parkedPowerHostBusy() {
    // An operator joined to the maintenance AP must not lose the controller
    // midway through a settings change, capture, or API bench.
    if (WiFi.softAPgetStationNum() > 0U) return true;
    if (bus_a_ready.load(std::memory_order_acquire) == 0U ||
        bus_b_ready.load(std::memory_order_acquire) == 0U ||
        application_resource_reload_requested.load(std::memory_order_acquire) != 0U) {
        return true;
    }
    const DiagnosticTransportStats diagnostic = diagnosticTransportStats();
    if (diagnostic.active || diagnosticTransportTxPending(Direction::A_TO_B) ||
        diagnosticTransportTxPending(Direction::B_TO_A)) {
        return true;
    }
    {
        SessionLogLockGuard log_lock(0);
        if (!log_lock.locked() || !session_log.sleepSafe()) return true;
    }
    if (replay_mutex != nullptr) {
        if (xSemaphoreTake(replay_mutex, 0) != pdTRUE) return true;
        const bool replay_active = replay_engine.isPlaying();
        xSemaphoreGive(replay_mutex);
        if (replay_active) return true;
    }
    return false;
}

void handleParkedCanQuiesce(uint32_t now_ms) {
    enum class Phase : uint8_t { Idle, WaitingForQuiet, WaitingForAlertTask };
    static Phase phase = Phase::Idle;
    static uint32_t requested_ms = 0U;

    if (parked_can_quiesce_requested.load(std::memory_order_acquire) == 0U) {
        phase = Phase::Idle;
        requested_ms = 0U;
        return;
    }
    if (phase == Phase::Idle) {
        requested_ms = now_ms;
        phase = Phase::WaitingForQuiet;
    }

    const uint32_t last_ingress = last_physical_ingress_ms.load(std::memory_order_acquire);
    const bool ingress_quiet = last_ingress == 0U || now_ms - last_ingress >= kParkedCanQuietMs;
    twai_status_info_t twai_status{};
    const bool twai_empty = twai_driver_installed.load(std::memory_order_acquire) != 0U &&
        twai_get_status_info(&twai_status) == ESP_OK && twai_status.msgs_to_tx == 0U;
    const bool mcp_empty = bus_b_ready.load(std::memory_order_acquire) != 0U &&
        (can_mcp.getStatus() & 0x54U) == 0U;
    const bool queues_empty = !gateway.physicalBacklogPending();
    const bool quiet = ingress_quiet && twai_empty && mcp_empty && queues_empty;

    if (phase == Phase::WaitingForQuiet) {
        if (!quiet) {
            if (now_ms - requested_ms >= kParkedCanQuiesceTimeoutMs) {
                parked_can_quiesce_requested.store(0U, std::memory_order_release);
                parked_can_quiesce_failed.store(1U, std::memory_order_release);
                phase = Phase::Idle;
                Serial.println("[power] sleep deferred: CAN did not become idle");
            }
            return;
        }
        parked_alert_stop_requested.store(1U, std::memory_order_release);
        if (twai_alert_task_handle != nullptr) xTaskNotifyGive(twai_alert_task_handle);
        phase = Phase::WaitingForAlertTask;
        return;
    }

    if (twai_alert_task_handle != nullptr &&
        parked_alert_stopped.load(std::memory_order_acquire) == 0U) {
        return;
    }
    // Re-check the owner queues and physical lines after the alert reader has
    // parked. The CAN task keeps polling during that short handshake window.
    if (!quiet) return;

    detachInterrupt(digitalPinToInterrupt(kMcpIntPin));
    setBusAReady(false);
    setBusBReady(false);
    const MCP2515::ERROR mcp_sleep = can_mcp.setSleepMode();
    if (mcp_sleep != MCP2515::ERROR_OK) {
        Serial.printf("[power] MCP2515 sleep failed: %u\n", static_cast<unsigned>(mcp_sleep));
    }
    SPI.end();

    if (twai_driver_installed.load(std::memory_order_acquire) != 0U) {
        const esp_err_t stop_error = twai_stop();
        if (stop_error != ESP_OK && stop_error != ESP_ERR_INVALID_STATE) {
            Serial.printf("[power] TWAI stop failed: %s\n", esp_err_to_name(stop_error));
        }
        const esp_err_t uninstall_error = twai_driver_uninstall();
        if (uninstall_error != ESP_OK) {
            Serial.printf("[power] TWAI uninstall failed: %s\n", esp_err_to_name(uninstall_error));
        } else {
            twai_driver_installed.store(0U, std::memory_order_release);
        }
    }
    twai_runtime_state.store(static_cast<uint8_t>(TWAI_STATE_STOPPED), std::memory_order_release);
    parked_can_quiesced.store(1U, std::memory_order_release);
    vTaskSuspend(nullptr);
}

bool enterParkedSleep(ParkedPowerDecision decision) {
    if (decision == ParkedPowerDecision::StayAwake || !parked_power.active()) return false;
    // Close the interval between the one-second policy tick and sleep entry: a
    // client may associate after the tick decided the host was idle.
    if (WiFi.softAPgetStationNum() > 0U) {
        parked_power.deferSleep(millis());
        Serial.println("[power] sleep deferred: maintenance client connected");
        return false;
    }
    if (digitalRead(kBusARxPin) == LOW) {
        parked_power.deferSleep(millis());
        Serial.println("[power] sleep deferred: primary-side CAN RX is dominant");
        return false;
    }
    if (!parked_power.beginSleep()) return false;

    if (const ApplicationExtension* app = application_registry.extension();
        app != nullptr && app->prepareForParkedSleep != nullptr) {
        app->prepareForParkedSleep();
    }
    const bool restore_active_writes = gateway.activeCanWritesAllowed();
    gateway.setActiveCanWritesAllowed(false);
    diagnosticTransportAbort(DiagnosticError::ABORTED);
    if (replay_mutex == nullptr || xSemaphoreTake(replay_mutex, pdMS_TO_TICKS(25)) == pdTRUE) {
        replay_engine.stop();
        if (replay_mutex != nullptr) xSemaphoreGive(replay_mutex);
    }

    parked_can_quiesced.store(0U, std::memory_order_release);
    parked_can_quiesce_failed.store(0U, std::memory_order_release);
    parked_alert_stop_requested.store(0U, std::memory_order_release);
    parked_alert_stopped.store(twai_alert_task_handle == nullptr ? 1U : 0U, std::memory_order_release);
    parked_can_quiesce_requested.store(1U, std::memory_order_release);
    if (can_task_handle != nullptr) xTaskNotifyGive(can_task_handle);

    const uint32_t handshake_started_ms = millis();
    while (parked_can_quiesced.load(std::memory_order_acquire) == 0U &&
           parked_can_quiesce_failed.load(std::memory_order_acquire) == 0U &&
           millis() - handshake_started_ms < kParkedCanHandshakeTimeoutMs) {
        delay(1);
    }
    if (parked_can_quiesce_failed.load(std::memory_order_acquire) != 0U) {
        parked_power.deferSleep(millis());
        gateway.setActiveCanWritesAllowed(restore_active_writes);
        return false;
    }
    if (parked_can_quiesced.load(std::memory_order_acquire) == 0U) {
        Serial.println("[power] CAN shutdown handshake timed out; restarting safely");
        delay(20);
        esp_restart();
    }

    Serial.printf("[power] entering parked sleep reason=%s\n", parkedPowerDecisionName(decision));
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    WiFi.setSleep(true);
    if (fs_mounted.load(std::memory_order_acquire) != 0U) LittleFS.end();

    ParkedPowerStatus status{};
    parked_power.snapshot(millis(), status);
    static_cast<void>(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
    const gpio_num_t wake_gpio = static_cast<gpio_num_t>(kBusARxPin);
    rtc_gpio_deinit(wake_gpio);
    pinMode(kBusARxPin, INPUT);
    rtc_gpio_pullup_dis(wake_gpio);
    rtc_gpio_pulldown_dis(wake_gpio);
    const esp_err_t can_wake_error = esp_sleep_enable_ext0_wakeup(wake_gpio, 0);
    const esp_err_t timer_wake_error = esp_sleep_enable_timer_wakeup(
        static_cast<uint64_t>(status.wake_timer_seconds) * 1000000ULL);
    if (can_wake_error != ESP_OK) {
        Serial.printf("[power] CAN RX wake setup failed: %s\n", esp_err_to_name(can_wake_error));
    }
    if (timer_wake_error != ESP_OK) {
        Serial.printf("[power] timer wake setup failed: %s\n", esp_err_to_name(timer_wake_error));
    }
    if (can_wake_error != ESP_OK && timer_wake_error != ESP_OK) {
        Serial.println("[power] no wake source available; restarting instead of sleeping");
        delay(20);
        esp_restart();
    }

    ++parked_deep_sleep_entries;
    delay(20);
    esp_deep_sleep_start();
    return true;
}

bool performParkedPowerBootProbe() {
    if (!parked_power.bootProbeRequired()) return true;
    if (bus_a_ready.load(std::memory_order_acquire) == 0U ||
        bus_b_ready.load(std::memory_order_acquire) == 0U) {
        parked_power.deferSleep(millis());
        Serial.println("[power] boot qualification failed awake: both CAN buses are not online");
        return true;
    }

    parked_power.setBootProbePending(true);
    const uint32_t started_ms = millis();
    const uint32_t duration_ms = parked_power.bootProbeDurationMs();
    Serial.printf("[power] parked-power boot probe started with bridge online (%lums)\n",
                  static_cast<unsigned long>(duration_ms));
    while (millis() - started_ms < duration_ms) {
        if (!parked_power.active() || parked_power.ignitionRecentlyOn(millis())) break;
        delay(1);
    }
    const ParkedPowerDecision decision = parked_power.finishBootProbe(millis());
    if (decision == ParkedPowerDecision::StayAwake) {
        Serial.println("[power] continuing active boot for ignition or maintenance grace");
        return true;
    }
    static_cast<void>(enterParkedSleep(decision));
    return true;
}

void monitorParkedPower(uint32_t now_ms, bool host_busy, bool application_busy) {
    static uint32_t last_check_ms = 0U;
    if (now_ms - last_check_ms < 1000U) return;
    last_check_ms = now_ms;
    const ParkedPowerDecision decision = parked_power.tick(
        now_ms, host_busy || application_busy);
    if (decision != ParkedPowerDecision::StayAwake) {
        static_cast<void>(enterParkedSleep(decision));
    }
}

void canRuntimeTask(void* /*context*/) {
    uint32_t last_rate_sample_ms = millis();
    uint32_t last_forwarded = 0U;
    uint32_t last_stats_log_ms = millis();

    gateway.setReadyGate(true);
    can_runtime_started_us.store(micros(), std::memory_order_release);

    for (;;) {
        const uint32_t now_us = micros();
        const uint32_t now_ms = millis();

        // Free prepared/raw capacity before touching hardware RX, then process a
        // second bounded batch after ingress. This prevents newly arrived frames
        // from being dropped behind an egress queue that has just become writable.
        gateway.pollRx(now_us, now_ms);
        pollCanIngress();
        gateway.pollRx(now_us, now_ms);
        monitorCanHealth(now_ms);
        diagnosticTransportTick(now_us, now_ms);
        // UI loads and controls replay on the other core. Never block physical
        // CAN for that bench-only feature; simply skip this replay tick while a
        // new log is being staged.
        if (replay_mutex == nullptr || xSemaphoreTake(replay_mutex, 0) == pdTRUE) {
            replay_engine.tick(now_us);
            if (replay_mutex != nullptr) xSemaphoreGive(replay_mutex);
        }
        gateway.publishStats();
        handleParkedCanQuiesce(now_ms);

        if (now_ms - last_rate_sample_ms >= 1000U) {
            const uint32_t forwarded = gateway.canOwnerStats().forwarded_frames;
            frame_rate_fps.store(static_cast<uint16_t>(forwarded - last_forwarded), std::memory_order_release);
            last_forwarded = forwarded;
            last_rate_sample_ms = now_ms;
            can_stack_min_free.store(
                static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)),
                std::memory_order_release);
        }

        if (now_ms - last_stats_log_ms >= 5000U) {
            publishGatewayLogSnapshot();
            last_stats_log_ms = now_ms;
        }

        // MCP INT and TWAI alert notifications wake this task immediately. The
        // one-tick timeout only advances diagnostic/replay timers while idle;
        // unlike an unconditional delay it does not create a polling blind spot.
        if (digitalRead(kMcpIntPin) == LOW) continue;
        static_cast<void>(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1)));
    }
}

void uiRuntimeTask(void* /*context*/) {
    uint32_t last_stack_sample_ms = 0U;
    for (;;) {
        server.handleClient();
        // If the dedicated application task could not be created, retain the
        // original single-task behavior as a degraded but functional fallback.
        if (application_task_handle == nullptr) {
            const uint32_t app_now_ms = millis();
            {
                SessionLogLockGuard log_lock(pdMS_TO_TICKS(5));
                if (log_lock.locked()) session_log.tick(can_trace, app_now_ms);
            }
            const bool host_power_busy = parkedPowerHostBusy();
            ApplicationLockGuard app_lock(pdMS_TO_TICKS(5));
            if (app_lock.locked()) {
                bool application_power_busy = false;
                if (const ApplicationExtension* app = application_registry.extension();
                    app != nullptr && app->tick != nullptr) {
                    app->tick();
                }
                applyRequestedApplicationResources();
                appendSessionHealthSnapshot(app_now_ms);
                if (const ApplicationExtension* app = application_registry.extension();
                    app != nullptr && app->parkedPowerBusy != nullptr) {
                    application_power_busy = app->parkedPowerBusy();
                }
                monitorParkedPower(app_now_ms, host_power_busy, application_power_busy);
            }
        }
        const uint32_t now_ms = millis();
        if (now_ms - last_stack_sample_ms >= 1000U) {
            ui_stack_min_free.store(
                static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)),
                std::memory_order_release);
            last_stack_sample_ms = now_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void applicationRuntimeTask(void* /*context*/) {
    uint32_t last_stack_sample_ms = 0U;
    for (;;) {
        flushGatewayLogSnapshot();
        const uint32_t app_now_ms = millis();
        {
            SessionLogLockGuard log_lock(pdMS_TO_TICKS(5));
            if (log_lock.locked()) session_log.tick(can_trace, app_now_ms);
        }
        const bool host_power_busy = parkedPowerHostBusy();
        {
            ApplicationLockGuard app_lock(pdMS_TO_TICKS(5));
            if (app_lock.locked()) {
                bool application_power_busy = false;
                if (const ApplicationExtension* app = application_registry.extension();
                    app != nullptr && app->tick != nullptr) {
                    app->tick();
                }
                applyRequestedApplicationResources();
                appendSessionHealthSnapshot(app_now_ms);
                if (const ApplicationExtension* app = application_registry.extension();
                    app != nullptr && app->parkedPowerBusy != nullptr) {
                    application_power_busy = app->parkedPowerBusy();
                }
                monitorParkedPower(app_now_ms, host_power_busy, application_power_busy);
            }
        }
        const uint32_t now_ms = millis();
        if (now_ms - last_stack_sample_ms >= 1000U) {
            application_stack_min_free.store(
                static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)),
                std::memory_order_release);
            last_stack_sample_ms = now_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    application_mutex = xSemaphoreCreateRecursiveMutex();
    session_log_mutex = xSemaphoreCreateRecursiveMutex();
    replay_mutex = xSemaphoreCreateMutex();
    if (application_mutex == nullptr) {
        Serial.println("[runtime] application mutex unavailable; using UI-task fallback");
    }
    if (session_log_mutex == nullptr) {
        Serial.println("[runtime] session-log mutex unavailable; recorder shares its caller task");
    }
    if (replay_mutex == nullptr) {
        Serial.println("[runtime] replay mutex unavailable; replay must remain disabled");
    }
    // Registration is allocation-free and immutable. The policy's NVS read is
    // deliberately deferred until the full two-bus CAN owner is forwarding.
    registerSignalScopeApplication(application_registry);

    gateway.init();
    if (const ApplicationExtension* app = application_registry.extension(); app != nullptr) {
        gateway.setMutationDirectionMask(app->mutation_direction_mask);
    }
    const bool trace_ready = can_trace.init();
    mutation_engine.init();
    runtime_values.init();
    runtime_tables.init();
    mutation_engine.setRuntimeValueRegistry(&runtime_values);
    mutation_engine.setRuntimeTableRegistry(&runtime_tables);
    replay_engine.init();
    frame_cache.init();
    signal_cache.init();
    observation_manager.init();

    gateway.setMutationEngine(&mutation_engine);
    gateway.setReplayEngine(&replay_engine);
    gateway.setTxDriver(txDriver);
    gateway.setTraceCallback([](const CanFrame& input, const CanFrame& output,
                                bool mutated, bool synthetic) {
        can_trace.pushPrepared(input, output, mutated, synthetic);
    });
    gateway.setFrameCache(&frame_cache);
    gateway.setSignalCache(&signal_cache);
    gateway.setObservationManager(&observation_manager);
    gateway.setDbcPointer(&active_dbc);
    gateway.setReadyGate(false);
    replay_engine.setTxCallback(replayTxBridge);
    diagnosticTransportInit(diagnosticTxDriver);
    Serial.printf("[session-log] trace queue ready=%d capacity=%u\n", trace_ready ? 1 : 0,
                  static_cast<unsigned>(CanTraceQueue::kDefaultCapacity));

    const bool bus_a_initialized = initBusA();
    const bool bus_b_initialized = initBusB();
    setBusAReady(bus_a_initialized);
    setBusBReady(bus_b_initialized);
    const uint32_t initial_recovery_base_ms = millis();
    // A failed initial install gets one short retry once the CAN owner starts;
    // only repeated failures use the normal five-second backoff.
    next_twai_recovery_attempt_ms = initial_recovery_base_ms + kCanInitialRecoveryRetryMs;
    next_mcp_recovery_attempt_ms = initial_recovery_base_ms + kCanInitialRecoveryRetryMs;
    pinMode(kMcpIntPin, INPUT_PULLUP);

    const BaseType_t can_core = selectCanCore();
    const BaseType_t ui_core = selectUiCore();
    const bool dual_runtime = dualCoreRuntimeEnabled();
    const BaseType_t can_ok = xTaskCreatePinnedToCore(
        canRuntimeTask,
        "ss_can",
        kCanTaskStackBytes,
        nullptr,
        3,
        &can_task_handle,
        can_core);
    for (uint16_t wait_ms = 0U;
         can_ok == pdPASS && can_runtime_started_us.load(std::memory_order_acquire) == 0U && wait_ms < 250U;
         ++wait_ms) {
        delay(1);
    }
    if (can_ok != pdPASS) failCanRuntimeStartup("task_create_failed");
    if (can_runtime_started_us.load(std::memory_order_acquire) == 0U) {
        failCanRuntimeStartup("task_start_timeout");
    }
    attachInterrupt(digitalPinToInterrupt(kMcpIntPin), notifyCanTaskFromMcp, FALLING);
    configureTwaiWakeAlerts();
    const BaseType_t alert_ok = xTaskCreatePinnedToCore(
        twaiAlertTask,
        "ss_twai_alert",
        kTwaiAlertTaskStackBytes,
        nullptr,
        4,
        &twai_alert_task_handle,
        can_core);
    if (alert_ok != pdPASS) {
        twai_alert_task_handle = nullptr;
        Serial.println("[runtime] TWAI alert helper unavailable; CAN task retains 1 ms idle polling fallback");
    }

    // CAN has priority over NVS, filesystem, and networking. Unlike the legacy
    // primary-side-only probe, both sides of the inline bridge are already
    // forwarding throughout policy load and ignition qualification.
    initializeParkedPower();
    static_cast<void>(performParkedPowerBootProbe());

    persistence.begin();
    ui_rule_scratch = static_cast<RuleListEntry*>(
        allocateRuntimeMemory(sizeof(RuleListEntry) * MutationEngine::kMaxRules));
    ui_frame_scratch = static_cast<FrameCacheSnapshot*>(
        allocateRuntimeMemory(sizeof(FrameCacheSnapshot) * kStatusFrameLimit));
    ui_signal_index_scratch = static_cast<uint16_t*>(
        allocateRuntimeMemory(sizeof(uint16_t) * kSignalSnapshotLimit));
    ui_signal_scratch = static_cast<SignalCacheSnapshot*>(
        allocateRuntimeMemory(sizeof(SignalCacheSnapshot) * kSignalSnapshotLimit));
    Serial.printf(
        "[runtime] UI scratch rules=%d frames=%d signal_indexes=%d signals=%d\n",
        ui_rule_scratch != nullptr,
        ui_frame_scratch != nullptr,
        ui_signal_index_scratch != nullptr,
        ui_signal_scratch != nullptr);
    application_services.findSignalByName = [](const char* name) {
        return signal_cache.findSignalIndexByName(name);
    };
    application_services.findSignalByIdentity = [](uint32_t can_id, const char* name) {
        return signal_cache.findSignalIndexByName(can_id, name);
    };
    application_services.subscribeSignal = [](uint16_t index, bool enabled) {
        if (!signal_cache.subscribeSignal(index, enabled)) return false;
        uint32_t can_id = 0U;
        if (!signal_cache.signalCanId(index, can_id)) return false;
        if (enabled) {
            const bool a = observation_manager.addMandatory(can_id, Direction::A_TO_B);
            const bool b = observation_manager.addMandatory(can_id, Direction::B_TO_A);
            return a && b;
        }
        // Multiple subscribed signals can share one CAN ID. Retain the cheap
        // mandatory observation until the next DBC rebind rather than remove a
        // frame that another safety/control signal may still need.
        return true;
    };
    application_services.readSignal = [](uint16_t index, float* value, uint32_t* generation, bool* valid) {
        if (value == nullptr || generation == nullptr || valid == nullptr) return false;
        return signal_cache.readSignal(index, *value, *generation, *valid);
    };
    application_services.readSignalState = [](
        uint16_t index, float* value, uint32_t* generation, bool* valid,
        uint32_t* timestamp_us, Direction* direction) {
        if (value == nullptr || generation == nullptr || valid == nullptr ||
            timestamp_us == nullptr || direction == nullptr) return false;
        return signal_cache.readSignalState(index, *value, *generation, *valid, *timestamp_us, *direction);
    };
    application_services.signalCanId = [](uint16_t index, uint32_t* can_id) {
        return can_id != nullptr && signal_cache.signalCanId(index, *can_id);
    };
    application_services.readFrame = [](uint32_t can_id, Direction direction, ApplicationFrameSnapshot* frame) {
        if (frame == nullptr) return false;
        FrameCacheSnapshot snapshot{};
        if (!frame_cache.read(can_id, direction, &snapshot)) return false;
        frame->can_id = snapshot.can_id;
        frame->direction = snapshot.direction;
        frame->dlc = snapshot.dlc;
        std::memcpy(frame->data, snapshot.data, sizeof(frame->data));
        std::memcpy(frame->input_data, snapshot.input_data, sizeof(frame->input_data));
        frame->has_input = snapshot.has_input;
        frame->mutated = snapshot.mutated;
        frame->timestamp_us = snapshot.last_timestamp_us;
        frame->total_frames = snapshot.total_frames;
        return true;
    };
    application_services.readPhysicalFrame = [](
        uint32_t can_id, Direction direction, ApplicationFrameSnapshot* frame) {
        if (frame == nullptr) return false;
        FrameCacheSnapshot snapshot{};
        if (!frame_cache.readPhysical(can_id, direction, &snapshot)) return false;
        frame->can_id = snapshot.can_id;
        frame->direction = snapshot.direction;
        frame->dlc = snapshot.dlc;
        std::memcpy(frame->data, snapshot.data, sizeof(frame->data));
        std::memcpy(frame->input_data, snapshot.input_data, sizeof(frame->input_data));
        frame->has_input = false;
        frame->mutated = false;
        frame->timestamp_us = snapshot.last_timestamp_us;
        frame->total_frames = snapshot.total_frames;
        return true;
    };
    application_services.snapshotFramesByDirection = [](
        Direction direction, ApplicationFrameSnapshot* frames, size_t capacity) {
        if (frames == nullptr || capacity == 0U) return static_cast<size_t>(0U);
        constexpr size_t kMaximumApplicationFrames = 16U;
        FrameCacheSnapshot snapshots[kMaximumApplicationFrames] = {};
        const size_t limited = capacity < kMaximumApplicationFrames ? capacity : kMaximumApplicationFrames;
        const size_t count = frame_cache.snapshotDirection(direction, snapshots, limited);
        for (size_t i = 0U; i < count; ++i) {
            frames[i].can_id = snapshots[i].can_id;
            frames[i].direction = snapshots[i].direction;
            frames[i].dlc = snapshots[i].dlc;
            std::memcpy(frames[i].data, snapshots[i].data, sizeof(frames[i].data));
            std::memcpy(frames[i].input_data, snapshots[i].input_data, sizeof(frames[i].input_data));
            frames[i].has_input = snapshots[i].has_input;
            frames[i].mutated = snapshots[i].mutated;
            frames[i].timestamp_us = snapshots[i].last_timestamp_us;
            frames[i].total_frames = snapshots[i].total_frames;
        }
        return count;
    };
    application_services.snapshotMutatedFrames = [](ApplicationFrameSnapshot* frames, size_t capacity) {
        if (frames == nullptr || capacity == 0U) return static_cast<size_t>(0U);
        constexpr size_t kMaximumCaptureFrames = 16U;
        FrameCacheSnapshot snapshots[kMaximumCaptureFrames] = {};
        const size_t limited = capacity < kMaximumCaptureFrames ? capacity : kMaximumCaptureFrames;
        const size_t count = frame_cache.snapshotMutated(snapshots, limited);
        for (size_t i = 0U; i < count; ++i) {
            frames[i].can_id = snapshots[i].can_id;
            frames[i].direction = snapshots[i].direction;
            frames[i].dlc = snapshots[i].dlc;
            std::memcpy(frames[i].data, snapshots[i].data, sizeof(frames[i].data));
            std::memcpy(frames[i].input_data, snapshots[i].input_data, sizeof(frames[i].input_data));
            frames[i].has_input = snapshots[i].has_input;
            frames[i].mutated = snapshots[i].mutated;
            frames[i].timestamp_us = snapshots[i].last_timestamp_us;
            frames[i].total_frames = snapshots[i].total_frames;
        }
        return count;
    };
    application_services.publishRuntimeValue = [](const char* name, float value) {
        return runtime_values.publish(name, value) >= 0;
    };
    application_services.readRuntimeValue = [](const char* name, float* value) {
        if (name == nullptr || value == nullptr) return false;
        const int32_t index = runtime_values.find(name);
        return index >= 0 && runtime_values.read(static_cast<uint16_t>(index), *value);
    };
    application_services.publishRuntimeTable = [](const char* name, const float* values, size_t count, bool valid) {
        return runtime_tables.publish(name, values, count, valid) >= 0;
    };
    application_services.requestResourceReload = []() {
        application_resource_reload_requested.store(1U, std::memory_order_release);
        return true;
    };
    application_services.submitDiagnostic = [](const DiagnosticRequest* request, uint32_t* job_id) {
        return request != nullptr && diagnosticTransportSubmit(*request, job_id);
    };
    application_services.cancelDiagnostic = [](uint32_t job_id) {
        return diagnosticTransportCancel(job_id);
    };
    application_services.readDiagnosticResult = [](uint32_t job_id, DiagnosticResult* result) {
        return diagnosticTransportReadResult(job_id, result);
    };
    application_services.sessionLogActive = []() {
        SessionLogLockGuard log_lock(pdMS_TO_TICKS(5));
        return log_lock.locked() && session_log.recording();
    };
    application_services.appendSessionLogAnnotation = [](
        const char* kind, const char* json, size_t json_length) {
        const ApplicationExtension* app = application_registry.extension();
        const char* source = app == nullptr || app->id == nullptr ? "application" : app->id;
        SessionLogLockGuard log_lock(pdMS_TO_TICKS(5));
        return log_lock.locked() &&
            session_log.appendAnnotation(source, kind, json, json_length, micros());
    };
    application_services.setParkedPowerPolicy = [](const ParkedPowerPolicy* policy) {
        if (policy == nullptr) return false;
        parked_power.updatePolicy(*policy, millis());
        return true;
    };
    application_services.readParkedPowerStatus = [](ParkedPowerStatus* status) {
        if (status == nullptr) return false;
        parked_power.snapshot(millis(), *status);
        return true;
    };
    if (const ApplicationExtension* app = application_registry.extension();
        app != nullptr && app->attachServices != nullptr) {
        app->attachServices(&application_services);
    }

    const bool mounted = LittleFS.begin(false, "/littlefs", 10, "littlefs")
        || LittleFS.begin(true, "/littlefs", 10, "littlefs");
    fs_mounted.store(mounted ? 1U : 0U, std::memory_order_release);
    if (mounted) {
        resolveUiIndexPath();
        static_cast<void>(session_log.begin());
        if (!autoLoadDbcFromLittleFs()) {
            Serial.println("[dbc] auto-load skipped: no valid /dbc/*.dbc found");
        }
    }
    if (const ApplicationExtension* app = application_registry.extension(); app != nullptr && app->begin != nullptr) {
        app->begin();
    }
    if (mounted) {
        const ApplicationExtension* const app = application_registry.extension();
        if (app == nullptr) {
            // Standalone projects can persist their proven rules as
            // /rules/active.ssrules. Installed applications instead own the
            // DBC/rule pair through the transactional extension callbacks.
            if (!autoLoadRulePackageFromLittleFs()) {
                Serial.println("[rules] auto-load skipped: no active/default package found");
            }
        } else {
            const ApplicationResourceApplyResult startup_resources =
                applyApplicationResourceTransaction();
            if (startup_resources == ApplicationResourceApplyResult::DATABASE_FAILED) {
                Serial.println("[app] requested DBC could not be loaded");
            } else if (startup_resources == ApplicationResourceApplyResult::RULE_PACKAGE_FAILED) {
                Serial.println("[app] requested rule package could not be loaded");
            }
        }
    }
    // The physical gateway has already been forwarding since the CAN task
    // started.  Enable synthetic replay/mutation only after persisted app
    // state and its resource package have had their first load attempt.
    gateway.setActiveCanWritesAllowed(true);
    application_config_ready_us.store(micros(), std::memory_order_release);

    startAccessPoint();
    configureHttpServer();
    const BaseType_t application_ok = application_mutex == nullptr ? pdFAIL : xTaskCreatePinnedToCore(
        applicationRuntimeTask,
        "ss_app",
        kApplicationTaskStackBytes,
        nullptr,
        2,
        &application_task_handle,
        ui_core);
    const BaseType_t ui_ok = xTaskCreatePinnedToCore(
        uiRuntimeTask,
        "ss_ui",
        kUiTaskStackBytes,
        nullptr,
        1,
        &ui_task_handle,
        ui_core);

    Serial.printf("[runtime] mode=%s hw_cores=%d pref_dual=%d | CAN core=%d created=%d | APP core=%d created=%d | UI core=%d created=%d\n",
        dual_runtime ? "dual-core" : "single-core",
        static_cast<int>(portNUM_PROCESSORS),
        kUseDualCore ? 1 : 0,
        static_cast<int>(can_core),
        static_cast<int>(can_ok == pdPASS),
        static_cast<int>(ui_core),
        static_cast<int>(application_ok == pdPASS),
        static_cast<int>(ui_core),
        static_cast<int>(ui_ok == pdPASS));
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

namespace {

void handleRuleStage() {
    if (!server.hasArg("rule_kind") && server.hasArg("operation")) {
        SignalMutation mutation{};
        if (!tryParseUIntArg("can_id", mutation.can_id) || mutation.can_id > 0x1FFFFFFFU) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_can_id\"}");
            return;
        }
        mutation.direction = parseDirectionFromText(server.arg("direction"), Direction::A_TO_B);

        uint32_t start_bit = 0U;
        uint32_t length = 8U;
        const bool start_valid = !server.hasArg("start_bit") || tryParseUIntArg("start_bit", start_bit);
        const bool length_valid = !server.hasArg("length") || tryParseUIntArg("length", length);
        if (!start_valid || !length_valid || start_bit > 63U || length < 1U || length > 64U) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_bit_range\"}");
            return;
        }
        mutation.start_bit = static_cast<uint16_t>(start_bit);
        mutation.length = static_cast<uint8_t>(length);

        mutation.little_endian = parseBoolText(server.arg("little_endian"), true);
        mutation.is_signed = parseBoolText(server.arg("is_signed"), false);
        mutation.factor = parseFloatArg("factor", 1.0F);
        mutation.offset = parseFloatArg("offset", 0.0F);

        const String operation_text = server.arg("operation");
        if (operation_text == "REPLACE") {
            mutation.operation = MutationOperation::REPLACE;
        } else if (operation_text == "PASS_THROUGH") {
            mutation.operation = MutationOperation::PASS_THROUGH;
        } else if (operation_text == "ADD_OFFSET") {
            mutation.operation = MutationOperation::ADD_OFFSET;
        } else if (operation_text == "MULTIPLY") {
            mutation.operation = MutationOperation::MULTIPLY;
        } else if (operation_text == "CLAMP") {
            mutation.operation = MutationOperation::CLAMP;
        } else {
            mutation.operation = MutationOperation::REPLACE;
        }

        mutation.op_value1 = parseFloatArg("op_value1", 0.0F);
        mutation.op_value2 = parseFloatArg("op_value2", 0.0F);
        mutation.enabled = parseBoolText(server.arg("enabled"), true);

        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        if (!mutation_engine.stageMutation(mutation)) {
            app_lock.release();
            server.send(507, "application/json", "{\"ok\":false,\"error\":\"stage_failed\"}");
            return;
        }
        const String json = "{\"ok\":true,\"rule_epoch\":" + String(mutation_engine.ruleEpoch()) +
            ",\"staging_count\":" + String(static_cast<uint32_t>(mutation_engine.stagingCount())) + "}";
        app_lock.release();
        server.send(200, "application/json", json);
        return;
    }

    RuleStageRequest request{};
    const String kind_text = server.hasArg("rule_kind")
        ? server.arg("rule_kind")
        : (server.hasArg("kind") ? server.arg("kind") : "BIT_RANGE");

    if (kind_text != "BIT_RANGE" && kind_text != "RAW_MASK") {
        // Advanced COUNTER/SEQUENCE/checksum/source rules have additional
        // fields and are compiled through the transactional .ssrules loader.
        // Silently treating a misspelled or package-only kind as BIT_RANGE
        // would stage a valid-looking but entirely different mutation.
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"unsupported_staged_rule_kind\"}");
        return;
    }
    request.kind = (kind_text == "RAW_MASK") ? RuleKind::RAW_MASK : RuleKind::BIT_RANGE;
    if (!tryParseUIntArg("can_id", request.can_id) || request.can_id > 0x1FFFFFFFU) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_can_id\"}");
        return;
    }
    request.direction = parseDirectionFromText(server.arg("direction"), Direction::A_TO_B);
    request.enabled = parseBoolText(server.arg("enabled"), true);

    if (request.kind == RuleKind::RAW_MASK) {
        if (!parseHexBytes(server.arg("mask"), request.mask) || !parseHexBytes(server.arg("value"), request.value)) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_mask_or_value\"}");
            return;
        }
    } else {
        uint32_t start_bit = 0U;
        uint32_t length = 8U;
        const bool start_valid = !server.hasArg("start_bit") || tryParseUIntArg("start_bit", start_bit);
        const bool length_valid = !server.hasArg("length") || tryParseUIntArg("length", length);
        if (!start_valid || !length_valid || start_bit > 63U || length < 1U || length > 64U) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_bit_range\"}");
            return;
        }
        request.start_bit = static_cast<uint16_t>(start_bit);
        request.bit_length = static_cast<uint8_t>(length);
        request.little_endian = parseBoolText(server.arg("little_endian"), true);
        request.dynamic_value = parseBoolText(server.arg("dynamic"), false);
        if (server.hasArg("replace_value")) {
            if (!tryParseUInt64Arg("replace_value", request.replace_value)) {
                server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_replace_value\"}");
                return;
            }
        } else if (server.hasArg("op_value1") && !tryParseUInt64Arg("op_value1", request.replace_value)) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_replace_value\"}");
            return;
        }
    }

    uint16_t rule_id = 0U;
    ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
    if (!app_lock.locked()) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
        return;
    }
    if (!mutation_engine.stageRule(request, &rule_id)) {
        app_lock.release();
        server.send(507, "application/json", "{\"ok\":false,\"error\":\"stage_failed\"}");
        return;
    }
    String json = "{\"ok\":true,\"rule_id\":" + String(rule_id) +
        ",\"rule_epoch\":" + String(mutation_engine.ruleEpoch()) +
        ",\"staging_count\":" + String(static_cast<uint32_t>(mutation_engine.stagingCount())) + "}";
    app_lock.release();
    server.send(200, "application/json", json);
}

String requestedRulePackagePath() {
    String path = server.hasArg("path") ? server.arg("path") : "";
    path.trim();
    if (path.length() == 0U) path = kActiveRulePackagePath;
    if (!path.startsWith("/")) path = String(kRulesDirPath) + "/" + path;
    return path;
}

void handleRulePackageRead() {
    const String path = server.hasArg("path") ? requestedRulePackagePath() :
        (active_rule_package_path.length() == 0U ? String(kActiveRulePackagePath) : active_rule_package_path);
    if (!validRulePackagePath(path)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_rule_package_path\"}");
        return;
    }
    File file = LittleFS.open(path, "r");
    if (!file || file.isDirectory()) {
        if (file) file.close();
        server.send(404, "application/json", "{\"ok\":false,\"error\":\"rule_package_not_found\"}");
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    if (server.hasArg("download")) {
        const String filename = path.substring(path.lastIndexOf('/') + 1);
        server.sendHeader("Content-Disposition", "attachment; filename=" + filename);
    }
    server.streamFile(file, "text/plain");
    file.close();
}

void handleRulePackageWrite() {
    const String path = requestedRulePackagePath();
    const String input = server.arg("plain");
    if (!validRulePackagePath(path)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_rule_package_path\"}");
        return;
    }
    if (input.length() == 0U || input.length() > 64U * 1024U) {
        server.send(413, "application/json", "{\"ok\":false,\"error\":\"rule_package_size_invalid\"}");
        return;
    }

    // Write to a temporary file first. RulePackageLoader commits atomically
    // only after every row validates, so an invalid upload cannot replace the
    // active mutation table or the last known-good startup file.
    static constexpr const char* kUploadPath = "/rules/upload.tmp.ssrules";
    if (!LittleFS.exists(kRulesDirPath)) static_cast<void>(LittleFS.mkdir(kRulesDirPath));
    File file = LittleFS.open(kUploadPath, "w");
    if (!file || file.print(input) != input.length()) {
        if (file) file.close();
        LittleFS.remove(kUploadPath);
        server.send(507, "application/json", "{\"ok\":false,\"error\":\"rule_package_write_failed\"}");
        return;
    }
    file.close();

    ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
    if (!app_lock.locked()) {
        LittleFS.remove(kUploadPath);
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
        return;
    }
    const String previous_active_path = active_rule_package_path;
    // Keep a bounded RAM snapshot in case the validated rules compile but the
    // subsequent LittleFS promote fails. This also covers a live table that
    // was authored only in RAM and therefore has no package file to reload.
    const size_t previous_rule_count = ui_rule_scratch == nullptr
        ? 0U
        : mutation_engine.listRules(ui_rule_scratch, MutationEngine::kMaxRules);
    if (!loadRulePackageFromFilePath(kUploadPath)) {
        LittleFS.remove(kUploadPath);
        server.send(422, "application/json", "{\"ok\":false,\"error\":\"rule_package_invalid\"}");
        return;
    }
    // LittleFS rename is the commit point. Never delete the destination first:
    // an atomic replacement either installs the validated upload or leaves the
    // previous startup package intact.
    if (!LittleFS.rename(kUploadPath, path)) {
        LittleFS.remove(kUploadPath);
        bool restored = false;
        if (previous_active_path.length() > 0U && validRulePackagePath(previous_active_path) &&
            LittleFS.exists(previous_active_path)) {
            restored = loadRulePackageFromFilePath(previous_active_path);
        }
        if (!restored && ui_rule_scratch != nullptr) {
            mutation_engine.clearStaging();
            restored = true;
            for (size_t i = 0U; i < previous_rule_count; ++i) {
                RuleStageRequest request = ui_rule_scratch[i].request;
                request.enabled = ui_rule_scratch[i].active;
                if (!mutation_engine.stageRule(request, nullptr)) {
                    restored = false;
                    break;
                }
            }
            if (restored) restored = mutation_engine.applyCommit();
        }
        if (!restored) mutation_engine.clearRules();
        active_rule_package_path = previous_active_path;
        invalidateApplicationResourcesLocked();
        server.send(507, "application/json", "{\"ok\":false,\"error\":\"rule_package_promote_failed\"}");
        return;
    }
    active_rule_package_path = path;
    invalidateApplicationResourcesLocked();
    const String json = "{\"ok\":true,\"path\":\"" + escapeJsonString(path.c_str()) +
        "\",\"count\":" + String(static_cast<uint32_t>(mutation_engine.activeCount())) +
        ",\"rule_epoch\":" + String(mutation_engine.ruleEpoch()) + "}";
    server.send(200, "application/json", json);
}

void handleRulePackageSelect() {
    const String path = requestedRulePackagePath();
    if (!validRulePackagePath(path)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_rule_package_path\"}");
        return;
    }
    ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
    if (!app_lock.locked()) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
        return;
    }
    if (!loadRulePackageFromFilePath(path)) {
        server.send(422, "application/json", "{\"ok\":false,\"error\":\"rule_package_load_failed\"}");
        return;
    }
    invalidateApplicationResourcesLocked();
    const String json = "{\"ok\":true,\"path\":\"" + escapeJsonString(path.c_str()) +
        "\",\"count\":" + String(static_cast<uint32_t>(mutation_engine.activeCount())) +
        ",\"rule_epoch\":" + String(mutation_engine.ruleEpoch()) + "}";
    server.send(200, "application/json", json);
}

void handleRulesAction() {
    const String body = server.arg("plain");

    if (bodyContains(body, "apply_commit")) {
        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        const bool ok = mutation_engine.applyCommit();
        if (ok) {
            active_rule_package_path = "";
            invalidateApplicationResourcesLocked();
        }
        const String json = ok
            ? "{\"ok\":true,\"action\":\"apply_commit\",\"rule_epoch\":" +
                String(mutation_engine.ruleEpoch()) + "}"
            : "{\"ok\":false}";
        app_lock.release();
        server.send(ok ? 200 : 422, "application/json", json);
        return;
    }
    if (bodyContains(body, "revert")) {
        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        mutation_engine.revertStagingToActive();
        const String json = "{\"ok\":true,\"action\":\"revert\",\"rule_epoch\":" +
            String(mutation_engine.ruleEpoch()) + "}";
        app_lock.release();
        server.send(200, "application/json", json);
        return;
    }
    if (bodyContains(body, "clear_staging")) {
        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        mutation_engine.clearStaging();
        const String json = "{\"ok\":true,\"action\":\"clear_staging\",\"rule_epoch\":" +
            String(mutation_engine.ruleEpoch()) + "}";
        app_lock.release();
        server.send(200, "application/json", json);
        return;
    }
    if (bodyContains(body, "clear_rules")) {
        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        mutation_engine.clearRules();
        active_rule_package_path = "";
        invalidateApplicationResourcesLocked();
        const String json = "{\"ok\":true,\"action\":\"clear_rules\",\"rule_epoch\":" +
            String(mutation_engine.ruleEpoch()) + "}";
        app_lock.release();
        server.send(200, "application/json", json);
        return;
    }

    server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown_action\"}");
}

void handleRulesList() {
    if (ui_rule_scratch == nullptr) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"ui_scratch_unavailable\"}");
        return;
    }
    RuleListEntry* rules = ui_rule_scratch;
    const bool view_supplied = server.hasArg("view");
    const String requested_view = view_supplied ? server.arg("view") : "active";
    if (requested_view != "active" && requested_view != "staging") {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_rules_view\"}");
        return;
    }

    size_t count = 0U;
    uint32_t snapshot_epoch = 0U;
    if (requested_view == "staging") {
        // The active table has its own reader barrier. Candidate rules are
        // ordinary application state, so snapshot them under the same lock
        // used by stage/apply/revert operations.
        ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
        if (!app_lock.locked()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
            return;
        }
        count = mutation_engine.listStagedRules(rules, MutationEngine::kMaxRules);
        snapshot_epoch = mutation_engine.ruleEpoch();
    } else {
        count = mutation_engine.listRules(rules, MutationEngine::kMaxRules);
        snapshot_epoch = count > 0U ? rules[0].epoch : mutation_engine.ruleEpoch();
    }

    String json;
    json.reserve(22000);
    json += "{\"ok\":true,\"view\":\"" + requested_view +
        "\",\"count\":" + String(static_cast<uint32_t>(count)) +
        ",\"candidate_dirty\":" + String(mutation_engine.stagingDirty() ? "true" : "false") +
        ",\"rule_epoch\":" + String(snapshot_epoch) + ",\"rules\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i > 0U) json += ",";
        const RuleListEntry& item = rules[i];
        json += "{";
        json += "\"rule_id\":" + String(item.rule_id) + ",";
        json += "\"rule_epoch\":" + String(item.epoch) + ",";
        json += "\"priority\":" + String(item.priority) + ",";
        json += "\"active\":" + String(item.active ? "true" : "false") + ",";
        json += "\"enabled\":" + String(item.active ? "true" : "false") + ",";
        json += "\"kind\":\"" + String(ruleKindToString(item.request.kind)) + "\",";
        json += "\"can_id\":" + String(item.request.can_id) + ",";
        json += "\"direction\":\"" + String(directionToString(item.request.direction)) + "\",";
        json += "\"start_bit\":" + String(item.request.start_bit) + ",";
        json += "\"length\":" + String(item.request.bit_length) + ",";
        json += "\"little_endian\":" + String(item.request.little_endian ? "true" : "false") + ",";
        json += "\"dynamic\":" + String(item.request.dynamic_value ? "true" : "false") + ",";
        json += "\"manual_dynamic\":" + String(
            item.request.dynamic_value && item.request.value_source[0] == '\0' ? "true" : "false") + ",";
        json += "\"value_source\":\"" + escapeJsonString(item.request.value_source) + "\",";
        const String replace_value_text = uint64ToDecimalString(item.request.replace_value);
        json += "\"replace_value\":" + replace_value_text + ",";
        json += "\"replace_value_text\":\"" + replace_value_text + "\",";
        uint32_t runtime_value = 0U;
        const char* runtime_value_kind = "none";
        if (item.request.kind == RuleKind::BIT_RANGE && item.request.dynamic_value &&
            item.request.value_source[0] == '\0') {
            runtime_value = static_cast<uint32_t>(item.request.replace_value);
            runtime_value_kind = "raw";
        } else if (item.request.kind == RuleKind::COUNTER) {
            runtime_value = item.request.counter_initial;
            runtime_value_kind = "counter_state";
        } else if (item.request.kind == RuleKind::SEQUENCE8) {
            runtime_value = item.request.sequence_initial_index;
            runtime_value_kind = "sequence_index";
        }
        json += "\"runtime_value\":" + String(runtime_value) + ",";
        json += "\"runtime_value_text\":\"" + String(runtime_value) + "\",";
        json += "\"runtime_value_kind\":\"" + String(runtime_value_kind) + "\",";
        json += "\"sequence_count\":" + String(item.request.sequence_count);
        if (item.request.kind == RuleKind::RAW_MASK) {
            json += ",\"mask\":\"";
            for (uint8_t b = 0U; b < 8U; ++b) {
                char byte_text[3] = {};
                std::snprintf(byte_text, sizeof(byte_text), "%02X", item.request.mask[b]);
                json += byte_text;
            }
            json += "\",\"value\":\"";
            for (uint8_t b = 0U; b < 8U; ++b) {
                char byte_text[3] = {};
                std::snprintf(byte_text, sizeof(byte_text), "%02X", item.request.value[b]);
                json += byte_text;
            }
            json += "\"";
        }
        json += "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleRuleValue() {
    ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
    if (!app_lock.locked()) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
        return;
    }
    const int32_t rule_id = parseIntArg("rule_id", -1);
    if (rule_id < 0 || rule_id >= static_cast<int32_t>(MutationEngine::kMaxRules)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_rule_id\"}");
        return;
    }

    uint32_t value = 0U;
    if (!tryParseUIntArg("value", value)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_rule_value\"}");
        return;
    }
    const uint32_t expected_epoch = parseUIntArg("rule_epoch", parseUIntArg("epoch", 0U));
    if (expected_epoch != mutation_engine.ruleEpoch()) {
        server.send(409, "application/json", "{\"ok\":false,\"error\":\"stale_rule_handle\"}");
        return;
    }
    const bool view_supplied = server.hasArg("view");
    const String requested_view = view_supplied ? server.arg("view") : "active";
    if (requested_view != "active" && requested_view != "staging") {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_rules_view\"}");
        return;
    }
    const bool staged_only = requested_view == "staging";
    const bool value_ok = staged_only
        ? mutation_engine.setStagedRuleValue(static_cast<uint16_t>(rule_id), value, expected_epoch)
        : view_supplied
            ? mutation_engine.setActiveRuleValue(static_cast<uint16_t>(rule_id), value, expected_epoch)
            : mutation_engine.setRuleValue(static_cast<uint16_t>(rule_id), value, expected_epoch);
    if (!value_ok) {
        server.send(422, "application/json", "{\"ok\":false,\"error\":\"rule_value_rejected\"}");
        return;
    }

    if (server.hasArg("enabled")) {
        const bool enabled = parseBoolText(server.arg("enabled"), true);
        const bool enable_ok = staged_only
            ? mutation_engine.setStagedRuleEnabled(static_cast<uint16_t>(rule_id), enabled, expected_epoch)
            : view_supplied
                ? mutation_engine.setActiveRuleEnabled(static_cast<uint16_t>(rule_id), enabled, expected_epoch)
                : mutation_engine.enableRule(static_cast<uint16_t>(rule_id), enabled, expected_epoch);
        if (!enable_ok) {
            const bool stale = expected_epoch != mutation_engine.ruleEpoch();
            server.send(stale ? 409 : 404, "application/json",
                stale ? "{\"ok\":false,\"error\":\"stale_rule_handle\"}"
                      : "{\"ok\":false,\"error\":\"rule_not_found\"}");
            return;
        }
    }

    server.send(200, "application/json", "{\"ok\":true}");
}

void handleRuleEnable() {
    ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
    if (!app_lock.locked()) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
        return;
    }
    int32_t rule_id = parseIntArg("rule_id", -1);
    uint32_t expected_epoch = 0U;
    if (rule_id < 0 || rule_id >= static_cast<int32_t>(MutationEngine::kMaxRules)) {
        const uint32_t can_id = parseUIntArg("can_id", 0U);
        const Direction direction = parseDirectionFromText(server.arg("direction"), Direction::A_TO_B);
        const bool is_raw = server.hasArg("kind") && server.arg("kind") == "RAW_MASK";
        uint16_t resolved_rule_id = 0U;

        bool found = false;
        if (is_raw) {
            found = findRuleIdByRawIdentity(can_id, direction, resolved_rule_id);
        } else if (server.hasArg("start_bit") && server.hasArg("length")) {
            const uint16_t start_bit = static_cast<uint16_t>(parseUIntArg("start_bit", 0U));
            const uint8_t bit_length = static_cast<uint8_t>(parseUIntArg("length", 0U));
            found = findRuleIdByIdentity(can_id, direction, start_bit, bit_length, resolved_rule_id);
        }

        if (!found) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_rule_id\"}");
            return;
        }
        rule_id = static_cast<int32_t>(resolved_rule_id);
        expected_epoch = mutation_engine.ruleEpoch();
    } else {
        expected_epoch = parseUIntArg("rule_epoch", parseUIntArg("epoch", 0U));
        if (expected_epoch != mutation_engine.ruleEpoch()) {
            server.send(409, "application/json", "{\"ok\":false,\"error\":\"stale_rule_handle\"}");
            return;
        }
    }
    const bool view_supplied = server.hasArg("view");
    const String requested_view = view_supplied ? server.arg("view") : "active";
    if (requested_view != "active" && requested_view != "staging") {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_rules_view\"}");
        return;
    }
    const bool staged_only = requested_view == "staging";
    const bool enabled = parseBoolText(server.arg("enabled"), true);
    const bool ok = staged_only
        ? mutation_engine.setStagedRuleEnabled(static_cast<uint16_t>(rule_id), enabled, expected_epoch)
        : view_supplied
            ? mutation_engine.setActiveRuleEnabled(static_cast<uint16_t>(rule_id), enabled, expected_epoch)
            : mutation_engine.enableRule(static_cast<uint16_t>(rule_id), enabled, expected_epoch);
    server.send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rule_not_found\"}");
}

void handleReplayLoad() {
    const String csv_text = server.arg("plain");
    if (csv_text.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty_replay_body\"}");
        return;
    }

    if (replay_mutex == nullptr || xSemaphoreTake(replay_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"replay_busy\"}");
        return;
    }
    const Direction replay_direction = parseDirectionFromText(server.arg("direction"), Direction::A_TO_B);
    const bool dry_run = parseBoolText(server.arg("dry_run"), false);
    const ReplayDispatchMode dispatch_mode = dry_run
        ? ReplayDispatchMode::DRY_RUN : ReplayDispatchMode::PHYSICAL;
    replay_engine.stop();
    const bool loaded = replay_engine.loadLogCsv(
        csv_text.c_str(), static_cast<size_t>(csv_text.length()), replay_direction, dispatch_mode);
    const uint32_t frame_count = static_cast<uint32_t>(replay_engine.frameCount());
    xSemaphoreGive(replay_mutex);

    const String json = "{\"ok\":" + String(loaded ? "true" : "false") +
        ",\"frames\":" + String(frame_count) +
        ",\"dry_run\":" + String(dry_run ? "true" : "false") + "}";
    server.send(loaded ? 200 : 422, "application/json", json);
}

void handleReplayControl() {
    String action_text = server.arg("plain");
    if (server.hasArg("action")) {
        action_text = server.arg("action");
    }

    if (bodyContains(action_text, "start")) {
        const bool body_dry_run_true = bodyContains(action_text, "dry_run=true") ||
            bodyContains(action_text, "\"dry_run\":true");
        const bool body_dry_run_false = bodyContains(action_text, "dry_run=false") ||
            bodyContains(action_text, "\"dry_run\":false");
        const bool has_dispatch_override = server.hasArg("dry_run") ||
            body_dry_run_true || body_dry_run_false;
        if (replay_mutex == nullptr || xSemaphoreTake(replay_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"replay_busy\"}");
            return;
        }
        if (replay_engine.frameCount() == 0U) {
            xSemaphoreGive(replay_mutex);
            server.send(409, "application/json", "{\"ok\":false,\"error\":\"replay_empty\"}");
            return;
        }

        ReplayLoopMode loop_mode = ReplayLoopMode::PLAY_ONCE;
        if (server.hasArg("loop_mode")) {
            const String mode_text = server.arg("loop_mode");
            if (mode_text == "LOOP_RAW") {
                loop_mode = ReplayLoopMode::LOOP_RAW;
            } else if (mode_text == "LOOP_WITH_COUNTER_CONTINUATION") {
                loop_mode = ReplayLoopMode::LOOP_WITH_COUNTER_CONTINUATION;
            }
        } else if (bodyContains(action_text, "LOOP_RAW")) {
            loop_mode = ReplayLoopMode::LOOP_RAW;
        } else if (bodyContains(action_text, "LOOP_WITH_COUNTER_CONTINUATION")) {
            loop_mode = ReplayLoopMode::LOOP_WITH_COUNTER_CONTINUATION;
        }

        const uint32_t start_delay_us = parseUIntArg("start_delay_us", 0U);
        if (has_dispatch_override) {
            const bool dry_run = server.hasArg("dry_run")
                ? parseBoolText(server.arg("dry_run"), replay_engine.isDryRun())
                : body_dry_run_true;
            replay_engine.start(
                loop_mode, micros() + start_delay_us,
                dry_run ? ReplayDispatchMode::DRY_RUN : ReplayDispatchMode::PHYSICAL);
        } else {
            // Retain the dispatch mode selected when the replay was loaded.
            replay_engine.start(loop_mode, micros() + start_delay_us);
        }
        const bool dry_run = replay_engine.isDryRun();
        xSemaphoreGive(replay_mutex);
        const String json = "{\"ok\":true,\"action\":\"start\",\"start_delay_us\":" +
            String(start_delay_us) + ",\"dry_run\":" + String(dry_run ? "true" : "false") + "}";
        server.send(200, "application/json", json);
        return;
    }

    if (bodyContains(action_text, "stop")) {
        if (replay_mutex == nullptr || xSemaphoreTake(replay_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"replay_busy\"}");
            return;
        }
        replay_engine.stop();
        xSemaphoreGive(replay_mutex);
        server.send(200, "application/json", "{\"ok\":true,\"action\":\"stop\"}");
        return;
    }

    server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown_replay_action\"}");
}

void handleReplaySend() {
    uint32_t can_id = 0U;
    uint32_t dlc_arg = 8U;
    if (!tryParseUIntArg("can_id", can_id) || can_id > 0x1FFFFFFFU) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_can_id\"}");
        return;
    }
    if ((server.hasArg("dlc") && !tryParseUIntArg("dlc", dlc_arg)) || dlc_arg > 8U) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_dlc\"}");
        return;
    }
    const uint8_t dlc = static_cast<uint8_t>(dlc_arg);
    const Direction direction = parseDirectionFromText(server.arg("direction"), Direction::A_TO_B);

    uint8_t data_bytes[8] = {0};
    if (server.hasArg("data")) {
        if (!parseHexBytes(server.arg("data"), data_bytes)) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_data_bytes\"}");
            return;
        }
    }

    uint32_t repeat = parseUIntArg("repeat", 1U);
    if (repeat < 1U) repeat = 1U;
    if (repeat > 256U) repeat = 256U;

    const uint32_t interval_us = parseUIntArg("interval_us", 0U);
    const uint32_t start_delay_us = parseUIntArg("start_delay_us", 0U);
    const bool auto_start = parseBoolText(server.arg("auto_start"), true);
    const bool dry_run = parseBoolText(server.arg("dry_run"), false);
    const ReplayDispatchMode dispatch_mode = dry_run
        ? ReplayDispatchMode::DRY_RUN : ReplayDispatchMode::PHYSICAL;

    String csv;
    csv.reserve(static_cast<size_t>(repeat) * 48U);

    uint32_t timestamp_us = 0U;
    for (uint32_t i = 0; i < repeat; ++i) {
        csv += String(timestamp_us);
        csv += ",";
        csv += String(can_id);
        csv += ",";
        csv += String(dlc);
        for (uint8_t b = 0; b < 8U; ++b) {
            char byte_text[3] = {0};
            std::snprintf(byte_text, sizeof(byte_text), "%02X", data_bytes[b]);
            csv += ",";
            csv += byte_text;
        }
        csv += ",0,";
        csv += directionToString(direction);
        csv += "\n";

        if (UINT32_MAX - timestamp_us < interval_us) {
            timestamp_us = UINT32_MAX;
        } else {
            timestamp_us += interval_us;
        }
    }

    if (replay_mutex == nullptr || xSemaphoreTake(replay_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"replay_busy\"}");
        return;
    }
    replay_engine.stop();
    if (!replay_engine.loadLogCsv(
            csv.c_str(), static_cast<size_t>(csv.length()), direction, dispatch_mode)) {
        xSemaphoreGive(replay_mutex);
        server.send(422, "application/json", "{\"ok\":false,\"error\":\"replay_send_load_failed\"}");
        return;
    }

    if (auto_start) {
        replay_engine.start(ReplayLoopMode::PLAY_ONCE, micros() + start_delay_us);
    }
    const uint32_t loaded_frame_count = static_cast<uint32_t>(replay_engine.frameCount());
    xSemaphoreGive(replay_mutex);

    const String json = "{\"ok\":true,\"frames\":" + String(loaded_frame_count) +
        ",\"repeat\":" + String(repeat) +
        ",\"interval_us\":" + String(interval_us) +
        ",\"start_delay_us\":" + String(start_delay_us) +
        ",\"dry_run\":" + String(dry_run ? "true" : "false") +
        ",\"started\":" + String(auto_start ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

void handleDbcUpload() {
    String dbc_text = server.arg("plain");
    if (dbc_text.length() == 0 && fs_mounted.load(std::memory_order_acquire) != 0U) {
        if (LittleFS.exists(kActiveDbcPath)) {
            File staged = LittleFS.open(kActiveDbcPath, "r");
            if (staged && !staged.isDirectory()) {
                dbc_text = staged.readString();
            }
            if (staged) staged.close();
        }
    }

    if (dbc_text.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty_dbc_body_or_upload\"}");
        return;
    }

    DbcDatabase candidate_database;
    SignalCache candidate_cache;
    candidate_cache.init();
    if (!candidate_database.parseFromText(dbc_text.c_str(), static_cast<size_t>(dbc_text.length()))) {
        server.send(422, "application/json", "{\"ok\":false,\"error\":\"dbc_parse_failed\"}");
        return;
    }
    if (!candidate_cache.resetForDbc(candidate_database)) {
        server.send(507, "application/json", "{\"ok\":false,\"error\":\"dbc_signal_cache_allocation_failed\"}");
        return;
    }

    if (fs_mounted.load(std::memory_order_acquire) != 0U) {
        if (!LittleFS.exists(kDbcDirPath)) {
            static_cast<void>(LittleFS.mkdir(kDbcDirPath));
        }
        File out = LittleFS.open(kActiveDbcPath, "w");
        if (out && !out.isDirectory()) {
            const size_t written = out.print(dbc_text);
            out.close();
            if (written != dbc_text.length()) {
                server.send(507, "application/json", "{\"ok\":false,\"error\":\"dbc_persist_failed\"}");
                return;
            }
        } else if (out) {
            out.close();
            server.send(507, "application/json", "{\"ok\":false,\"error\":\"dbc_persist_failed\"}");
            return;
        } else {
            server.send(507, "application/json", "{\"ok\":false,\"error\":\"dbc_persist_failed\"}");
            return;
        }
    }

    ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
    if (!app_lock.locked()) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
        return;
    }
    gateway.pauseSignalDecoding();
    const uint32_t wait_started_ms = millis();
    while (!gateway.signalDecodingIdle() && millis() - wait_started_ms < 250U) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!gateway.signalDecodingIdle()) {
        gateway.resumeSignalDecoding();
        app_lock.release();
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"dbc_decoder_busy\"}");
        return;
    }

    dbc_database.swap(candidate_database);
    signal_cache.swap(candidate_cache);
    active_dbc.store(&dbc_database, std::memory_order_release);
    signal_cache.clearSubscriptions();
    observation_manager.clearSpecific();
    observation_manager.clearMandatory();
    observation_manager.setMode(ObservationMode::NONE);
    if (replay_mutex != nullptr && xSemaphoreTake(replay_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        replay_engine.stop();
        xSemaphoreGive(replay_mutex);
    }
    mutation_engine.clearRules();
    active_rule_package_path = "";
    active_dbc_path = kActiveDbcPath;
    if (const ApplicationExtension* app = application_registry.extension(); app != nullptr) {
        if (app->databaseChanged != nullptr) app->databaseChanged();
    }
    invalidateApplicationResourcesLocked();
    gateway.resumeSignalDecoding();
    app_lock.release();

    const String json = "{\"ok\":true,\"messages\":" + String(static_cast<uint32_t>(dbc_database.messageCount())) +
        ",\"signals\":" + String(static_cast<uint32_t>(dbc_database.signalCount())) + "}";
    server.send(200, "application/json", json);
}

void handleDbcAutoload() {
    ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
    if (!app_lock.locked()) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
        return;
    }
    if (!autoLoadDbcFromLittleFs()) {
        app_lock.release();
        server.send(404, "application/json", "{\"ok\":false,\"error\":\"no_valid_dbc_in_folder\"}");
        return;
    }
    invalidateApplicationResourcesLocked();

    const DbcDatabase* dbc = active_dbc.load(std::memory_order_acquire);
    const String json = "{\"ok\":true,\"autoloaded\":true,\"messages\":" +
        String(static_cast<uint32_t>(dbc != nullptr ? dbc->messageCount() : 0U)) +
        ",\"signals\":" + String(static_cast<uint32_t>(dbc != nullptr ? dbc->signalCount() : 0U)) + "}";
    app_lock.release();
    server.send(200, "application/json", json);
}

void handleDbcSelect() {
    String path = server.arg("path");
    path.trim();
    if (!path.startsWith("/")) path = String("/dbc/") + path;

    String lower_path = path;
    lower_path.toLowerCase();
    if (!path.startsWith("/dbc/") || path.indexOf("..") >= 0 || !lower_path.endsWith(".dbc")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_dbc_path\"}");
        return;
    }
    if (!LittleFS.exists(path)) {
        server.send(404, "application/json", "{\"ok\":false,\"error\":\"dbc_not_found\"}");
        return;
    }
    ApplicationLockGuard app_lock(pdMS_TO_TICKS(1000));
    if (!app_lock.locked()) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"application_busy\"}");
        return;
    }
    if (!loadDbcFromFilePath(path)) {
        app_lock.release();
        server.send(422, "application/json", "{\"ok\":false,\"error\":\"dbc_load_failed\"}");
        return;
    }
    invalidateApplicationResourcesLocked();

    const DbcDatabase* dbc = active_dbc.load(std::memory_order_acquire);
    const String json = "{\"ok\":true,\"path\":\"" + escapeJsonString(path.c_str()) +
        "\",\"messages\":" + String(static_cast<uint32_t>(dbc != nullptr ? dbc->messageCount() : 0U)) +
        ",\"signals\":" + String(static_cast<uint32_t>(dbc != nullptr ? dbc->signalCount() : 0U)) +
        ",\"complete\":" + String(dbc != nullptr && dbc->messageCount() == dbc->messageCapacity() &&
        dbc->signalCount() == dbc->signalCapacity() ? "true" : "false") + "}";
    app_lock.release();
    server.send(200, "application/json", json);
}

void handleNotFound() {
    const String uri = server.uri();
    if (uri.startsWith("/api/")) {
        server.send(404, "application/json", "{\"ok\":false,\"error\":\"api_not_found\"}");
        return;
    }
    if (serveStaticFile(uri)) return;
    if (serveStaticFile(ui_index_path)) return;
    server.send(404, "text/plain", "SignalScope file not found");
}

}  // namespace

namespace {

const DbcSignalDef* findSignalByLocation(const DbcDatabase* dbc, uint32_t can_id, uint16_t start_bit, uint8_t bit_length) {
    if (dbc == nullptr) return nullptr;
    const size_t total = dbc->signalCount();
    for (size_t i = 0; i < total; ++i) {
        const DbcSignalDef* signal = dbc->signalAt(i);
        if (signal == nullptr) continue;
        if (signal->can_id == can_id && signal->start_bit == start_bit && signal->length == bit_length) {
            return signal;
        }
    }
    return nullptr;
}

bool findRuleIdByIdentity(uint32_t can_id, Direction direction, uint16_t start_bit, uint8_t bit_length, uint16_t& out_rule_id) {
    if (ui_rule_scratch == nullptr) return false;
    RuleListEntry* rules = ui_rule_scratch;
    const size_t count = mutation_engine.listRules(rules, MutationEngine::kMaxRules);
    for (size_t i = 0; i < count; ++i) {
        const RuleStageRequest& rule = rules[i].request;
        if (rule.kind != RuleKind::BIT_RANGE) continue;
        if (rule.can_id == can_id &&
            rule.direction == direction &&
            rule.start_bit == start_bit &&
            rule.bit_length == bit_length) {
            out_rule_id = rules[i].rule_id;
            return true;
        }
    }
    return false;
}
bool findRuleIdByRawIdentity(uint32_t can_id, Direction direction, uint16_t& out_rule_id) {
    if (ui_rule_scratch == nullptr) return false;
    RuleListEntry* rules = ui_rule_scratch;
    const size_t count = mutation_engine.listRules(rules, MutationEngine::kMaxRules);
    for (size_t i = 0; i < count; ++i) {
        const RuleStageRequest& rule = rules[i].request;
        if (rule.kind != RuleKind::RAW_MASK) continue;
        if (rule.can_id == can_id && rule.direction == direction) {
            out_rule_id = rules[i].rule_id;
            return true;
        }
    }
    return false;
}

void appendDecodedSignalsJson(String& json, const FrameCacheSnapshot& frame) {
    const DbcDatabase* dbc = active_dbc.load(std::memory_order_acquire);
    const DbcMessageDef* message = (dbc == nullptr) ? nullptr : dbc->findMessage(frame.can_id);

    json += "\"message_name\":";
    if (message != nullptr && message->name[0] != '\0') {
        json += "\"" + escapeJsonString(message->name) + "\"";
    } else {
        json += "null";
    }
    json += ",";

    json += "\"decoded_signals\":[";
    if (message != nullptr && message->signal_count > 0U) {
        const size_t message_signal_count = static_cast<size_t>(message->signal_count);
        const size_t max_signals = (message_signal_count > kMaxDecodedSignalsPerFrame)
            ? kMaxDecodedSignalsPerFrame
            : message_signal_count;

        const bool mutated = frame.mutated;
        bool first = true;
        for (size_t i = 0; i < max_signals; ++i) {
            const size_t signal_index = static_cast<size_t>(message->signal_start) + i;
            const DbcSignalDef* signal = dbc->signalAt(signal_index);
            if (signal == nullptr) continue;

            CanFrame displayed{};
            displayed.id = frame.can_id;
            displayed.direction = frame.direction;
            displayed.dlc = frame.dlc;
            std::memcpy(displayed.data, frame.data, sizeof(displayed.data));
            float value = 0.0F;
            if (!decodeSignal(displayed, *signal, value)) continue;

            if (!first) json += ",";
            first = false;

            json += "{";
            json += "\"index\":" + String(static_cast<uint32_t>(signal_index)) + ",";
            json += "\"name\":\"" + escapeJsonString(signal->name) + "\",";
            json += "\"value\":" + String(value, 3) + ",";
            json += "\"start_bit\":" + String(signal->start_bit) + ",";
            json += "\"length\":" + String(signal->length) + ",";
            json += "\"little_endian\":" + String(signal->little_endian ? "true" : "false") + ",";
            json += "\"is_signed\":" + String(signal->is_signed ? "true" : "false") + ",";
            json += "\"factor\":" + String(signal->factor, 6) + ",";
            json += "\"offset\":" + String(signal->offset, 6) + ",";
            json += "\"mutated\":" + String(mutated ? "true" : "false");
            json += "}";
        }
    }
    json += "]";
}

void appendActiveRulesJson(String& json) {
    if (ui_rule_scratch == nullptr) {
        json += "\"active_mutation_items\":[]";
        return;
    }
    RuleListEntry* rules = ui_rule_scratch;
    const size_t count = mutation_engine.listRules(rules, kStatusRuleLimit);
    const DbcDatabase* dbc = active_dbc.load(std::memory_order_acquire);

    json += "\"active_mutation_items\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i > 0U) json += ",";
        const RuleListEntry& item = rules[i];
        char can_id_text[11] = {0};
        std::snprintf(can_id_text, sizeof(can_id_text), "0x%03lX", static_cast<unsigned long>(item.request.can_id));

        json += "{";
        json += "\"rule_id\":" + String(item.rule_id) + ",";
        json += "\"rule_epoch\":" + String(item.epoch) + ",";
        json += "\"priority\":" + String(item.priority) + ",";
        json += "\"active\":" + String(item.active ? "true" : "false") + ",";
        json += "\"kind\":\"" + String(ruleKindToString(item.request.kind)) + "\",";
        json += "\"can_id\":\"" + String(can_id_text) + "\",";
        json += "\"direction\":\"" + String(directionToString(item.request.direction)) + "\",";
        json += "\"enabled\":" + String(item.active ? "true" : "false") + ",";
        json += "\"dynamic\":" + String(item.request.dynamic_value ? "true" : "false") + ",";
        json += "\"manual_dynamic\":" + String(
            item.request.dynamic_value && item.request.value_source[0] == '\0' ? "true" : "false") + ",";
        json += "\"value_source\":\"" + escapeJsonString(item.request.value_source) + "\",";
        json += "\"start_bit\":" + String(item.request.start_bit) + ",";
        json += "\"length\":" + String(item.request.bit_length) + ",";
        json += "\"little_endian\":" + String(item.request.little_endian ? "true" : "false") + ",";
        json += "\"operation\":\"" + String(item.request.kind == RuleKind::RAW_MASK ? "RAW_MASK" : "REPLACE") + "\",";
        const String replace_value_text = uint64ToDecimalString(item.request.replace_value);
        json += "\"replace_value\":" + replace_value_text + ",";
        json += "\"replace_value_text\":\"" + replace_value_text + "\",";
        uint32_t runtime_value = 0U;
        const char* runtime_value_kind = "none";
        if (item.request.kind == RuleKind::BIT_RANGE && item.request.dynamic_value &&
            item.request.value_source[0] == '\0') {
            runtime_value = static_cast<uint32_t>(item.request.replace_value);
            runtime_value_kind = "raw";
        } else if (item.request.kind == RuleKind::COUNTER) {
            runtime_value = item.request.counter_initial;
            runtime_value_kind = "counter_state";
        } else if (item.request.kind == RuleKind::SEQUENCE8) {
            runtime_value = item.request.sequence_initial_index;
            runtime_value_kind = "sequence_index";
        }
        json += "\"runtime_value\":" + String(runtime_value) + ",";
        json += "\"runtime_value_text\":\"" + String(runtime_value) + "\",";
        json += "\"runtime_value_kind\":\"" + String(runtime_value_kind) + "\",";
        json += "\"sequence_count\":" + String(item.request.sequence_count);

        if (item.request.kind == RuleKind::RAW_MASK) {
            json += ",\"mask\":\"";
            for (uint8_t b = 0; b < 8U; ++b) {
                char t[3] = {0};
                std::snprintf(t, sizeof(t), "%02X", item.request.mask[b]);
                json += t;
            }
            json += "\",\"value\":\"";
            for (uint8_t b = 0; b < 8U; ++b) {
                char t[3] = {0};
                std::snprintf(t, sizeof(t), "%02X", item.request.value[b]);
                json += t;
            }
            json += "\"";
        } else {
            json += ",\"signal_name\":";
            const DbcSignalDef* signal = findSignalByLocation(
                dbc,
                item.request.can_id,
                item.request.start_bit,
                item.request.bit_length);
            if (signal != nullptr && signal->name[0] != '\0') {
                json += "\"" + escapeJsonString(signal->name) + "\"";
            } else {
                json += "null";
            }
        }

        json += "}";
    }
    json += "]";
}

void handleStatus() {
    const GatewayStats stats = gateway.snapshotStats();
    const DbcDatabase* dbc = active_dbc.load(std::memory_order_acquire);
    uint32_t replay_frame_count = 0U;
    bool replay_playing = false;
    bool replay_dry_run = false;
    if (replay_mutex != nullptr && xSemaphoreTake(replay_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        replay_frame_count = static_cast<uint32_t>(replay_engine.frameCount());
        replay_playing = replay_engine.isPlaying();
        replay_dry_run = replay_engine.isDryRun();
        xSemaphoreGive(replay_mutex);
    }

    if (ui_frame_scratch == nullptr) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"ui_scratch_unavailable\"}");
        return;
    }
    FrameCacheSnapshot* frames = ui_frame_scratch;
    const size_t frame_count = frame_cache.snapshotRecent(frames, kStatusFrameLimit);
    size_t dbc_matched_frames = 0U;
    if (dbc != nullptr) {
        for (size_t i = 0U; i < frame_count; ++i) {
            if (dbc->findMessage(frames[i].can_id) != nullptr) ++dbc_matched_frames;
        }
    }

    const uint16_t fps = frame_rate_fps.load(std::memory_order_acquire);
    const uint16_t bus_a = (fps > 1000U) ? 100U : static_cast<uint16_t>(fps / 10U);
    const uint16_t bus_b = bus_a;
    const uint16_t bus_total = (bus_a + bus_b > 100U) ? 100U : static_cast<uint16_t>(bus_a + bus_b);
    const uint32_t can_started_us = can_runtime_started_us.load(std::memory_order_acquire);
    const uint32_t app_ready_us = application_config_ready_us.load(std::memory_order_acquire);
    const uint32_t configuration_after_can_us =
        can_started_us != 0U && app_ready_us != 0U ? app_ready_us - can_started_us : 0U;

    String json;
    json.reserve(36000);
    json += "{";
    json += "\"cpu_load_pct\":5,";
    json += "\"bus_a_util_pct\":" + String(bus_a) + ",";
    json += "\"bus_b_util_pct\":" + String(bus_b) + ",";
    json += "\"bus_total_util_pct\":" + String(bus_total) + ",";
    json += "\"bus_a_ready\":" + String(bus_a_ready.load(std::memory_order_acquire) ? "true" : "false") + ",";
    json += "\"bus_b_ready\":" + String(bus_b_ready.load(std::memory_order_acquire) ? "true" : "false") + ",";
    const uint8_t twai_state = twai_runtime_state.load(std::memory_order_acquire);
    json += "\"twai_state\":\"" + String(twaiStateName(twai_state)) + "\",";
    json += "\"twai_recovery_attempts\":" + String(twai_recovery_attempts.load(std::memory_order_relaxed)) + ",";
    json += "\"twai_restarts\":" + String(twai_restarts.load(std::memory_order_relaxed)) + ",";
    json += "\"twai_rx_missed\":" + String(twai_rx_missed.load(std::memory_order_relaxed)) + ",";
    json += "\"twai_rx_overruns\":" + String(twai_rx_overruns.load(std::memory_order_relaxed)) + ",";
    json += "\"mcp_error_flags\":" + String(mcp_error_flags.load(std::memory_order_relaxed)) + ",";
    json += "\"mcp_recoveries\":" + String(mcp_recoveries.load(std::memory_order_relaxed)) + ",";
    json += "\"mcp_rx_overruns\":" + String(mcp_rx_overruns.load(std::memory_order_relaxed)) + ",";
    json += "\"can_runtime_started_us\":" + String(can_started_us) + ",";
    json += "\"application_ready_us\":" + String(app_ready_us) + ",";
    json += "\"configuration_after_can_us\":" + String(configuration_after_can_us) + ",";
    json += "\"ingress_a_frames\":" + String(ingress_a_frames.load(std::memory_order_relaxed)) + ",";
    json += "\"ingress_b_frames\":" + String(ingress_b_frames.load(std::memory_order_relaxed)) + ",";
    json += "\"can_stack_min_free\":" + String(can_stack_min_free.load(std::memory_order_acquire)) + ",";
    json += "\"application_stack_min_free\":" +
        String(application_stack_min_free.load(std::memory_order_acquire)) + ",";
    json += "\"ui_stack_min_free\":" + String(ui_stack_min_free.load(std::memory_order_acquire)) + ",";
    json += "\"rx_queue_depth\":" + String(stats.rx_queue_depth) + ",";
    json += "\"egress_queue_depth_a_to_b\":" + String(stats.egress_queue_depth_a_to_b) + ",";
    json += "\"egress_queue_depth_b_to_a\":" + String(stats.egress_queue_depth_b_to_a) + ",";
    json += "\"egress_deferred_frames_a_to_b\":" + String(stats.egress_deferred_frames_a_to_b) + ",";
    json += "\"egress_deferred_frames_b_to_a\":" + String(stats.egress_deferred_frames_b_to_a) + ",";
    json += "\"egress_retry_attempts_a_to_b\":" + String(stats.egress_retry_attempts_a_to_b) + ",";
    json += "\"egress_retry_attempts_b_to_a\":" + String(stats.egress_retry_attempts_b_to_a) + ",";
    json += "\"egress_full_stalls_a_to_b\":" + String(stats.egress_full_stalls_a_to_b) + ",";
    json += "\"egress_full_stalls_b_to_a\":" + String(stats.egress_full_stalls_b_to_a) + ",";
    json += "\"stale_frames_purged_a_to_b\":" + String(stats.stale_frames_purged_a_to_b) + ",";
    json += "\"stale_frames_purged_b_to_a\":" + String(stats.stale_frames_purged_b_to_a) + ",";
    const bool physical_backlog_pending = stats.rx_queue_depth != 0U ||
        stats.egress_queue_depth_a_to_b != 0U || stats.egress_queue_depth_b_to_a != 0U;
    json += "\"physical_backlog_pending\":" + String(physical_backlog_pending ? "true" : "false") + ",";
    json += "\"rx_drops_boot\":" + String(stats.rx_drops_boot) + ",";
    json += "\"rx_drops_run\":" + String(stats.rx_drops_run) + ",";
    json += "\"dropped_frames\":" + String(stats.rx_drops_run) + ",";
    json += "\"tx_failures_a_to_b\":" + String(stats.tx_failures_a_to_b) + ",";
    json += "\"tx_failures_b_to_a\":" + String(stats.tx_failures_b_to_a) + ",";
    json += "\"tx_failed_frames\":" +
        String(stats.tx_failures_a_to_b + stats.tx_failures_b_to_a) + ",";
    json += "\"forwarded_frames\":" + String(stats.forwarded_frames) + ",";
    json += "\"replay_injected_frames\":" + String(stats.replay_injected_frames) + ",";
    json += "\"replay_dry_run_frames\":" + String(stats.replay_dry_run_frames) + ",";
    json += "\"replay_refused_frames\":" + String(stats.replay_refused_frames) + ",";
    json += "\"passive_fast_path_frames\":" + String(stats.passive_fast_path_frames) + ",";
    json += "\"observed_decoded_frames\":" + String(stats.observed_decoded_frames) + ",";
    json += "\"active_can_writes_allowed\":" +
        String(gateway.activeCanWritesAllowed() ? "true" : "false") + ",";
    json += "\"mutation_direction_mask\":" + String(gateway.mutationDirectionMask()) + ",";
    json += "\"mutations_a_to_b_allowed\":" +
        String(gateway.mutationDirectionAllowed(Direction::A_TO_B) ? "true" : "false") + ",";
    json += "\"mutations_b_to_a_allowed\":" +
        String(gateway.mutationDirectionAllowed(Direction::B_TO_A) ? "true" : "false") + ",";
    json += "\"active_mutations\":" + String(static_cast<uint32_t>(mutation_engine.activeCount())) + ",";
    json += "\"staging_mutations\":" + String(static_cast<uint32_t>(mutation_engine.stagingCount())) + ",";
    json += "\"candidate_dirty\":" + String(mutation_engine.stagingDirty() ? "true" : "false") + ",";
    json += "\"rule_epoch\":" + String(mutation_engine.ruleEpoch()) + ",";
    json += "\"dbc_loaded\":" + String(dbc != nullptr ? "true" : "false") + ",";
    json += "\"dbc_path\":\"" + escapeJsonString(active_dbc_path.c_str()) + "\",";
    json += "\"rule_package_path\":\"" + escapeJsonString(active_rule_package_path.c_str()) + "\",";
    json += "\"dbc_message_count\":" + String(static_cast<uint32_t>(dbc != nullptr ? dbc->messageCount() : 0U)) + ",";
    json += "\"dbc_signal_count\":" + String(static_cast<uint32_t>(dbc != nullptr ? dbc->signalCount() : 0U)) + ",";
    json += "\"dbc_message_capacity\":" + String(static_cast<uint32_t>(dbc != nullptr ? dbc->messageCapacity() : 0U)) + ",";
    json += "\"dbc_signal_capacity\":" + String(static_cast<uint32_t>(dbc != nullptr ? dbc->signalCapacity() : 0U)) + ",";
    json += "\"dbc_complete\":" + String(dbc != nullptr && dbc->messageCount() == dbc->messageCapacity() &&
        dbc->signalCount() == dbc->signalCapacity() ? "true" : "false") + ",";
    json += "\"dbc_matched_recent_frames\":" + String(static_cast<uint32_t>(dbc_matched_frames)) + ",";
    json += "\"dbc_recent_frame_count\":" + String(static_cast<uint32_t>(frame_count)) + ",";
    json += "\"replay_frame_count\":" + String(replay_frame_count) + ",";
    json += "\"replay_playing\":" + String(replay_playing ? "true" : "false") + ",";
    json += "\"replay_dry_run\":" + String(replay_dry_run ? "true" : "false") + ",";
    json += "\"frame_rate_fps\":" + String(fps) + ",";
    json += "\"observation_mode\":\"" + String(observationModeToString(observation_manager.mode())) + "\",";
    json += "\"decode_all\":" + String(signal_cache.decodeAll() ? "true" : "false") + ",";
    json += "\"fast_path_avg_us\":" + String(stats.fast_path_latency_avg_us) + ",";
    json += "\"active_path_avg_us\":" + String(stats.active_path_latency_avg_us) + ",";
    json += "\"fast_path_samples\":" + String(stats.fast_path_latency_samples) + ",";
    json += "\"active_path_samples\":" + String(stats.active_path_latency_samples) + ",";
    appendActiveRulesJson(json);
    json += ",\"recent_frames\":[";

    for (size_t i = 0; i < frame_count; ++i) {
        if (i > 0U) json += ",";
        char id_text[11] = {0};
        std::snprintf(id_text, sizeof(id_text), "0x%03lX", static_cast<unsigned long>(frames[i].can_id));

        json += "{";
        json += "\"id\":\"" + String(id_text) + "\",";
        json += "\"can_id\":" + String(frames[i].can_id) + ",";
        json += "\"dlc\":" + String(frames[i].dlc) + ",";
        json += "\"direction\":\"" + String(directionToString(frames[i].direction)) + "\",";
        json += "\"timestamp_us\":" + String(frames[i].last_timestamp_us) + ",";
        json += "\"data\":\"" + frameDataHex(frames[i]) + "\",";
        json += "\"rate_hz\":" + String(frames[i].rate_hz) + ",";
        json += "\"total_frames\":" + String(frames[i].total_frames) + ",";
        json += "\"mutated\":" + String(frames[i].mutated ? "true" : "false") + ",";
        appendDecodedSignalsJson(json, frames[i]);
        json += "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleFrameCache() {
    const uint32_t requested_limit = parseUIntArg("limit", kStatusFrameLimit);
    const size_t limit = (requested_limit > kStatusFrameLimit) ? kStatusFrameLimit : static_cast<size_t>(requested_limit);

    if (ui_frame_scratch == nullptr) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"ui_scratch_unavailable\"}");
        return;
    }
    FrameCacheSnapshot* frames = ui_frame_scratch;
    const size_t count = frame_cache.snapshot(frames, limit);

    String json;
    json.reserve(12000);
    json += "{\"ok\":true,\"count\":" + String(static_cast<uint32_t>(count)) + ",\"frames\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i > 0U) json += ",";
        json += "{";
        json += "\"can_id\":" + String(frames[i].can_id) + ",";
        json += "\"direction\":\"" + String(directionToString(frames[i].direction)) + "\",";
        json += "\"dlc\":" + String(frames[i].dlc) + ",";
        json += "\"timestamp_us\":" + String(frames[i].last_timestamp_us) + ",";
        json += "\"rate_hz\":" + String(frames[i].rate_hz) + ",";
        json += "\"mutated\":" + String(frames[i].mutated ? "true" : "false") + ",";
        json += "\"data\":\"" + frameDataHex(frames[i]) + "\"";
        json += "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleSignalCache() {
    if (ui_signal_index_scratch == nullptr || ui_signal_scratch == nullptr) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"ui_scratch_unavailable\"}");
        return;
    }
    uint16_t* indexes = ui_signal_index_scratch;
    size_t index_count = 0U;
    if (server.hasArg("indexes")) {
        index_count = parseU16Csv(server.arg("indexes"), indexes, kSignalSnapshotLimit);
    }

    SignalCacheSnapshot* entries = ui_signal_scratch;
    const size_t count = signal_cache.snapshotByIndexes(
        (index_count > 0U) ? indexes : nullptr,
        index_count,
        entries,
        kSignalSnapshotLimit);

    String json;
    json.reserve(20000);
    json += "{\"ok\":true,\"count\":" + String(static_cast<uint32_t>(count)) + ",\"signals\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i > 0U) json += ",";
        const SignalCacheSnapshot& s = entries[i];
        json += "{";
        json += "\"index\":" + String(s.index) + ",";
        json += "\"can_id\":" + String(s.can_id) + ",";
        json += "\"name\":\"" + escapeJsonString(s.name) + "\",";
        json += "\"value\":" + String(s.value, 4) + ",";
        json += "\"generation\":" + String(s.generation) + ",";
        json += "\"valid\":" + String(s.valid ? "true" : "false") + ",";
        json += "\"subscribed\":" + String(s.subscribed ? "true" : "false");
        json += "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void appendSignalCatalogEntryJson(
    String& json,
    const DbcSignalDef& signal,
    uint16_t index,
    uint32_t now_us,
    bool& first) {

    if (!first) json += ",";
    first = false;

    const SignalCatalogLiveValue sample = readSignalCatalogLiveValue(
        frame_cache, signal, now_us, kSignalCatalogFreshnessUs);
    char can_id_hex[16] = {};
    std::snprintf(can_id_hex, sizeof(can_id_hex), "0x%lX",
                  static_cast<unsigned long>(signal.can_id));

    json += "{";
    json += "\"startBit\":" + String(signal.start_bit) + ",";
    json += "\"length\":" + String(signal.length) + ",";
    json += "\"littleEndian\":" + String(signal.little_endian ? "true" : "false") + ",";
    json += "\"signed\":" + String(signal.is_signed ? "true" : "false") + ",";
    json += "\"factor\":" + String(signal.factor, 6) + ",";
    json += "\"offset\":" + String(signal.offset, 6) + ",";
    json += "\"index\":" + String(index) + ",";
    json += "\"canId\":" + String(signal.can_id) + ",";
    json += "\"canIdHex\":\"" + String(can_id_hex) + "\",";
    json += "\"name\":\"" + escapeJsonString(signal.name) + "\",";
    json += "\"valid\":" + String(sample.valid ? "true" : "false") + ",";
    json += "\"live\":" + String(sample.live ? "true" : "false") + ",";
    json += "\"value\":";
    if (sample.valid) json += String(sample.value, 6);
    else json += "null";
    json += ",\"ageMs\":";
    if (sample.frame_available) json += String(sample.age_ms);
    else json += "null";
    json += ",\"direction\":";
    if (sample.frame_available) {
        json += "\"" + String(directionToString(sample.direction)) + "\"";
    } else {
        json += "null";
    }
    json += "}";
}

void handleSignalCatalog() {
    String query = server.hasArg("q") ? server.arg("q") : "";
    query.trim();
    if (query.length() > 64U) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"signal_catalog_query_too_long\"}");
        return;
    }

    const uint32_t requested_limit = parseUIntArg("limit", kSignalCatalogDefaultLimit);
    const size_t limit = requested_limit == 0U
        ? 1U
        : (requested_limit > kSignalCatalogMaximumLimit
               ? kSignalCatalogMaximumLimit
               : static_cast<size_t>(requested_limit));
    const size_t offset = static_cast<size_t>(parseUIntArg("offset", 0U));
    const bool indexed_batch = server.hasArg("indexes");
    const DbcDatabase* const dbc = active_dbc.load(std::memory_order_acquire);

    uint16_t selected_indexes[kSignalCatalogMaximumLimit] = {};
    size_t total = 0U;
    size_t emitted = 0U;
    if (dbc != nullptr) {
        if (indexed_batch) {
            const size_t index_count = parseU16Csv(
                server.arg("indexes"), selected_indexes, kSignalCatalogMaximumLimit);
            for (size_t i = 0U; i < index_count; ++i) {
                const uint16_t index = selected_indexes[i];
                if (dbc->signalAt(index) == nullptr) continue;
                if (total >= offset && emitted < limit) selected_indexes[emitted++] = index;
                ++total;
            }
        } else {
            const size_t signal_count = dbc->signalCount();
            for (size_t i = 0U; i < signal_count; ++i) {
                const DbcSignalDef* signal = dbc->signalAt(i);
                if (signal == nullptr || !signalCatalogQueryMatches(*signal, query.c_str())) continue;
                if (total >= offset && emitted < limit) {
                    selected_indexes[emitted++] = static_cast<uint16_t>(i);
                }
                ++total;
            }
        }
    }

    String json;
    json.reserve(256U + limit * 176U);
    json += "{\"ok\":true,\"total\":" + String(static_cast<uint32_t>(total)) +
        ",\"offset\":" + String(static_cast<uint32_t>(offset)) +
        ",\"count\":" + String(static_cast<uint32_t>(emitted)) + ",\"signals\":[";
    bool first = true;
    const uint32_t now_us = micros();
    for (size_t i = 0U; dbc != nullptr && i < emitted; ++i) {
        const DbcSignalDef* signal = dbc->signalAt(selected_indexes[i]);
        if (signal == nullptr) continue;
        appendSignalCatalogEntryJson(json, *signal, selected_indexes[i], now_us, first);
    }
    json += "]}";
    static_cast<void>(sendBufferedJson(200, json.c_str(), json.length()));
}

void handleObserve() {
    const ObservationMode mode = parseObservationMode(server.arg("mode"));
    if (mode == ObservationMode::ALL) {
        observation_manager.clearSpecific();
        observation_manager.setMode(ObservationMode::ALL);
        signal_cache.setDecodeAll(true);
    } else if (mode == ObservationMode::SPECIFIC) {
        ObservationKey keys[ObservationManager::kMaxSpecificKeys];
        const size_t count = parseObservationCsv(
            server.hasArg("ids") ? server.arg("ids") : "",
            keys,
            ObservationManager::kMaxSpecificKeys);
        if (!observation_manager.setSpecific(keys, count)) {
            server.send(422, "application/json", "{\"ok\":false,\"error\":\"invalid_specific_subscription\"}");
            return;
        }
        signal_cache.setDecodeAll(false);
    } else {
        observation_manager.clearSpecific();
        observation_manager.setMode(ObservationMode::NONE);
        signal_cache.setDecodeAll(false);
    }

    const String json = "{\"ok\":true,\"mode\":\"" + String(observationModeToString(observation_manager.mode())) + "\"}";
    server.send(200, "application/json", json);
}

}  // namespace














