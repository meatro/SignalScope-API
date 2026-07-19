#pragma once

#include <atomic>
#include <cstdint>

namespace bored::signalscope {

// Application-owned policy passed to the generic SignalScope host. The host
// owns the physical CAN/Wi-Fi shutdown; an application only decides whether
// parked power management is available and decodes its ignition signal.
struct ParkedPowerPolicy {
    bool available = false;
    bool enabled = false;
    uint32_t sleep_delay_ms = 30000U;
    uint32_t wake_timer_seconds = 300U;
    uint32_t boot_probe_ms = 1200U;
    // A cold boot or explicit reset must leave enough time to join the local
    // maintenance AP. Deep-sleep CAN/timer wakes do not receive this grace.
    uint32_t power_on_grace_ms = 300000U;
};

enum class ParkedPowerWakeCause : uint8_t {
    PowerOnOrReset = 0,
    CanRx,
    Timer,
    Gpio,
    Other,
};

enum class ParkedPowerDecision : uint8_t {
    StayAwake = 0,
    SleepIgnitionOff,
    SleepIgnitionUnknown,
    SleepBootProbe,
};

struct ParkedPowerStatus {
    bool available = false;
    bool enabled = false;
    bool active = false;
    bool maintenance_override = false;
    bool ignition_seen = false;
    bool ignition_on = false;
    bool ignition_raw_on = false;
    bool boot_probe_pending = false;
    bool entering_sleep = false;
    uint8_t ignition_source = 0U;
    ParkedPowerWakeCause wake_cause = ParkedPowerWakeCause::PowerOnOrReset;
    uint32_t last_primary_age_ms = 0U;
    uint32_t last_ignition_on_age_ms = 0U;
    uint32_t last_ignition_off_age_ms = 0U;
    uint32_t sleep_delay_ms = 30000U;
    uint32_t wake_timer_seconds = 300U;
    uint32_t boot_probe_ms = 1200U;
    uint32_t deep_sleep_entries = 0U;
};

// Lock-free state shared by the CAN owner and the lower-priority application
// task. CAN ingress only publishes timestamps/decoded KL15 state; it never
// performs shutdown work or waits on another core.
class ParkedPowerController {
public:
    void initialize(const ParkedPowerPolicy& policy, bool maintenance_override,
                    ParkedPowerWakeCause wake_cause, uint32_t deep_sleep_entries,
                    uint32_t now_ms);
    void updatePolicy(const ParkedPowerPolicy& policy, uint32_t now_ms);

    void observePrimaryFrame(uint32_t now_ms);
    void observeIgnition(bool ignition_on, uint8_t source, uint32_t now_ms);
    void noteBusy(uint32_t now_ms);

    bool active() const;
    bool bootProbeRequired() const;
    uint32_t bootProbeDurationMs() const;
    bool ignitionRecentlyOn(uint32_t now_ms) const;
    ParkedPowerDecision finishBootProbe(uint32_t now_ms);
    ParkedPowerDecision tick(uint32_t now_ms, bool busy);

    bool beginSleep();
    void deferSleep(uint32_t now_ms);
    void setBootProbePending(bool pending);
    void snapshot(uint32_t now_ms, ParkedPowerStatus& status) const;

private:
    bool powerOnGraceActive(uint32_t now_ms) const;
    void resetIgnitionState();

    std::atomic<uint8_t> available_{0U};
    std::atomic<uint8_t> enabled_{0U};
    std::atomic<uint8_t> maintenance_override_{0U};
    std::atomic<uint8_t> ignition_seen_{0U};
    std::atomic<uint8_t> ignition_raw_on_{0U};
    std::atomic<uint8_t> ignition_source_{0U};
    std::atomic<uint8_t> boot_probe_pending_{0U};
    std::atomic<uint8_t> entering_sleep_{0U};
    std::atomic<uint8_t> wake_cause_{static_cast<uint8_t>(ParkedPowerWakeCause::PowerOnOrReset)};
    std::atomic<uint32_t> sleep_delay_ms_{30000U};
    std::atomic<uint32_t> wake_timer_seconds_{300U};
    std::atomic<uint32_t> boot_probe_ms_{1200U};
    std::atomic<uint32_t> power_on_grace_ms_{300000U};
    std::atomic<uint32_t> boot_started_ms_{0U};
    std::atomic<uint32_t> last_awake_ms_{0U};
    std::atomic<uint32_t> last_primary_ms_{0U};
    std::atomic<uint32_t> last_ignition_on_ms_{0U};
    std::atomic<uint32_t> last_ignition_off_ms_{0U};
    std::atomic<uint32_t> deep_sleep_entries_{0U};
};

const char* parkedPowerWakeCauseName(ParkedPowerWakeCause cause);
const char* parkedPowerDecisionName(ParkedPowerDecision decision);

}  // namespace bored::signalscope
