// Illustrative SignalScope native application.
//
// This file is not compiled by the default build. Add its path explicitly when
// you want to install it. SignalScope already owns CAN, the DBC, and HTTP; this
// app only subscribes to one decoded value and reports application state.

#include "core/application_extension.hpp"

#include <cstdio>

namespace bored::signalscope {
namespace oil_temperature_example {

constexpr uint32_t kOilFrameId = 0x321U;
constexpr const char* kOilSignalName = "OilTemperature";

const ApplicationServices* services = nullptr;
int32_t oil_signal_index = -1;
float oil_temperature_c = 0.0F;
uint32_t sample_generation = 0U;
uint32_t sample_timestamp_us = 0U;
Direction sample_direction = Direction::A_TO_B;
bool sample_valid = false;

// Signal indexes are handles for one active DBC generation. Resolve the stable
// CAN-ID/name identity each time the database changes.
void bindSignals() {
    oil_signal_index = -1;
    sample_valid = false;
    if (services == nullptr || services->findSignalByIdentity == nullptr ||
        services->subscribeSignal == nullptr) {
        return;
    }

    const int32_t candidate = services->findSignalByIdentity(kOilFrameId, kOilSignalName);
    if (candidate < 0 || candidate > 0xFFFF) return;
    if (!services->subscribeSignal(static_cast<uint16_t>(candidate), true)) return;
    oil_signal_index = candidate;
}

void attachServices(const ApplicationServices* attached) {
    // The host owns this table for its entire process lifetime.
    services = attached;
}

void begin() {
    // begin() runs after the host's first ordinary DBC autoload attempt.
    bindSignals();
}

void databaseChanged() {
    // The host clears subscriptions during a DBC swap, so rebind deliberately.
    bindSignals();
}

void tick() {
    if (services == nullptr || services->readSignalState == nullptr || oil_signal_index < 0) return;

    float value = 0.0F;
    uint32_t generation = 0U;
    bool valid = false;
    uint32_t timestamp_us = 0U;
    Direction direction = Direction::A_TO_B;
    if (!services->readSignalState(
            static_cast<uint16_t>(oil_signal_index), &value, &generation, &valid,
            &timestamp_us, &direction)) {
        sample_valid = false;
        return;
    }

    oil_temperature_c = value;
    sample_generation = generation;
    sample_timestamp_us = timestamp_us;
    sample_direction = direction;
    sample_valid = valid;
}

bool writeStatusJson(char* output, size_t capacity) {
    if (output == nullptr || capacity == 0U) return false;
    const char* direction = sample_direction == Direction::A_TO_B ? "A_TO_B" : "B_TO_A";
    const int written = std::snprintf(
        output,
        capacity,
        "{\"ok\":true,\"bound\":%s,\"valid\":%s,\"oilTemperatureC\":%.2f,"
        "\"generation\":%lu,\"timestampUs\":%lu,\"direction\":\"%s\"}",
        oil_signal_index >= 0 ? "true" : "false",
        sample_valid ? "true" : "false",
        static_cast<double>(oil_temperature_c),
        static_cast<unsigned long>(sample_generation),
        static_cast<unsigned long>(sample_timestamp_us),
        direction);
    return written >= 0 && static_cast<size_t>(written) < capacity;
}

}  // namespace oil_temperature_example

// This strong definition replaces SignalScope's weak standalone no-op hook.
void registerSignalScopeApplication(ApplicationExtensionRegistry& registry) {
    ApplicationExtension app{};
    app.id = "oil-temperature-example";
    app.begin = oil_temperature_example::begin;
    app.tick = oil_temperature_example::tick;
    app.writeStatusJson = oil_temperature_example::writeStatusJson;
    app.attachServices = oil_temperature_example::attachServices;
    app.databaseChanged = oil_temperature_example::databaseChanged;

    // Standalone behavior is bidirectional. A real application can narrow this
    // immutable boundary with kMutationDirectionAtoB/BtoA when its design calls
    // for it; this passive example does not need to.
    static_cast<void>(registry.registerExtension(app));
}

}  // namespace bored::signalscope
