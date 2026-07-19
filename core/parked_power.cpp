#include "parked_power.hpp"

namespace bored::signalscope {

namespace {

uint32_t clampValue(uint32_t value, uint32_t minimum, uint32_t maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

uint32_t ageOrZero(uint32_t now_ms, uint32_t timestamp_ms) {
    return timestamp_ms == 0U ? 0U : now_ms - timestamp_ms;
}

}  // namespace

void ParkedPowerController::initialize(
    const ParkedPowerPolicy& policy,
    bool maintenance_override,
    ParkedPowerWakeCause wake_cause,
    uint32_t deep_sleep_entries,
    uint32_t now_ms) {
    available_.store(0U, std::memory_order_relaxed);
    enabled_.store(0U, std::memory_order_relaxed);
    maintenance_override_.store(maintenance_override ? 1U : 0U, std::memory_order_release);
    wake_cause_.store(static_cast<uint8_t>(wake_cause), std::memory_order_release);
    deep_sleep_entries_.store(deep_sleep_entries, std::memory_order_release);
    boot_probe_pending_.store(0U, std::memory_order_release);
    entering_sleep_.store(0U, std::memory_order_release);
    boot_started_ms_.store(now_ms, std::memory_order_release);
    last_awake_ms_.store(now_ms, std::memory_order_release);
    last_primary_ms_.store(0U, std::memory_order_release);
    resetIgnitionState();
    updatePolicy(policy, now_ms);
}

void ParkedPowerController::updatePolicy(const ParkedPowerPolicy& policy, uint32_t now_ms) {
    const bool was_active = active();
    const bool was_available = available_.load(std::memory_order_acquire) != 0U;
    const bool next_available = policy.available;
    const bool next_enabled = next_available && policy.enabled;

    sleep_delay_ms_.store(
        clampValue(policy.sleep_delay_ms, 5000U, 600000U), std::memory_order_release);
    wake_timer_seconds_.store(
        clampValue(policy.wake_timer_seconds, 30U, 86400U), std::memory_order_release);
    boot_probe_ms_.store(
        clampValue(policy.boot_probe_ms, 100U, 5000U), std::memory_order_release);
    power_on_grace_ms_.store(
        clampValue(policy.power_on_grace_ms, 30000U, 600000U), std::memory_order_release);
    available_.store(next_available ? 1U : 0U, std::memory_order_release);
    enabled_.store(next_enabled ? 1U : 0U, std::memory_order_release);

    const bool now_active = active();
    if (!now_active || !was_active) {
        last_awake_ms_.store(now_ms, std::memory_order_release);
    }
    if (was_available != next_available || was_active != now_active) {
        resetIgnitionState();
    }
    if (!now_active) {
        entering_sleep_.store(0U, std::memory_order_release);
        boot_probe_pending_.store(0U, std::memory_order_release);
    }
}

void ParkedPowerController::observePrimaryFrame(uint32_t now_ms) {
    last_primary_ms_.store(now_ms, std::memory_order_release);
}

void ParkedPowerController::observeIgnition(bool ignition_on, uint8_t source, uint32_t now_ms) {
    ignition_seen_.store(1U, std::memory_order_release);
    ignition_raw_on_.store(ignition_on ? 1U : 0U, std::memory_order_release);
    ignition_source_.store(source, std::memory_order_release);
    if (ignition_on) {
        last_ignition_on_ms_.store(now_ms, std::memory_order_release);
        last_awake_ms_.store(now_ms, std::memory_order_release);
    } else {
        last_ignition_off_ms_.store(now_ms, std::memory_order_release);
    }
}

void ParkedPowerController::noteBusy(uint32_t now_ms) {
    last_awake_ms_.store(now_ms, std::memory_order_release);
}

bool ParkedPowerController::active() const {
    return available_.load(std::memory_order_acquire) != 0U &&
        enabled_.load(std::memory_order_acquire) != 0U &&
        maintenance_override_.load(std::memory_order_acquire) == 0U;
}

bool ParkedPowerController::bootProbeRequired() const {
    return active() && entering_sleep_.load(std::memory_order_acquire) == 0U;
}

uint32_t ParkedPowerController::bootProbeDurationMs() const {
    return boot_probe_ms_.load(std::memory_order_acquire);
}

bool ParkedPowerController::ignitionRecentlyOn(uint32_t now_ms) const {
    if (ignition_raw_on_.load(std::memory_order_acquire) == 0U) return false;
    const uint32_t last_on = last_ignition_on_ms_.load(std::memory_order_acquire);
    if (last_on == 0U) return false;
    return now_ms - last_on <= sleep_delay_ms_.load(std::memory_order_acquire);
}

bool ParkedPowerController::powerOnGraceActive(uint32_t now_ms) const {
    const auto wake_cause = static_cast<ParkedPowerWakeCause>(
        wake_cause_.load(std::memory_order_acquire));
    if (wake_cause != ParkedPowerWakeCause::PowerOnOrReset) return false;
    const uint32_t started_ms = boot_started_ms_.load(std::memory_order_acquire);
    const uint32_t grace_ms = power_on_grace_ms_.load(std::memory_order_acquire);
    // Unsigned elapsed-time arithmetic intentionally preserves millis() wrap.
    return now_ms - started_ms < grace_ms;
}

ParkedPowerDecision ParkedPowerController::finishBootProbe(uint32_t now_ms) {
    boot_probe_pending_.store(0U, std::memory_order_release);
    if (!active() || ignitionRecentlyOn(now_ms) || powerOnGraceActive(now_ms)) {
        return ParkedPowerDecision::StayAwake;
    }
    return ParkedPowerDecision::SleepBootProbe;
}

ParkedPowerDecision ParkedPowerController::tick(uint32_t now_ms, bool busy) {
    if (entering_sleep_.load(std::memory_order_acquire) != 0U) {
        return ParkedPowerDecision::StayAwake;
    }
    if (!active()) {
        last_awake_ms_.store(now_ms, std::memory_order_release);
        return ParkedPowerDecision::StayAwake;
    }
    if (powerOnGraceActive(now_ms)) return ParkedPowerDecision::StayAwake;
    if (busy) {
        noteBusy(now_ms);
        return ParkedPowerDecision::StayAwake;
    }
    if (ignitionRecentlyOn(now_ms)) return ParkedPowerDecision::StayAwake;

    const uint32_t last_awake = last_awake_ms_.load(std::memory_order_acquire);
    if (now_ms - last_awake < sleep_delay_ms_.load(std::memory_order_acquire)) {
        return ParkedPowerDecision::StayAwake;
    }
    return ignition_seen_.load(std::memory_order_acquire) != 0U
        ? ParkedPowerDecision::SleepIgnitionOff
        : ParkedPowerDecision::SleepIgnitionUnknown;
}

bool ParkedPowerController::beginSleep() {
    uint8_t expected = 0U;
    return entering_sleep_.compare_exchange_strong(
        expected, 1U, std::memory_order_acq_rel, std::memory_order_acquire);
}

void ParkedPowerController::deferSleep(uint32_t now_ms) {
    last_awake_ms_.store(now_ms, std::memory_order_release);
    entering_sleep_.store(0U, std::memory_order_release);
}

void ParkedPowerController::setBootProbePending(bool pending) {
    boot_probe_pending_.store(pending ? 1U : 0U, std::memory_order_release);
}

void ParkedPowerController::snapshot(uint32_t now_ms, ParkedPowerStatus& status) const {
    status.available = available_.load(std::memory_order_acquire) != 0U;
    status.enabled = enabled_.load(std::memory_order_acquire) != 0U;
    status.active = active();
    status.maintenance_override = maintenance_override_.load(std::memory_order_acquire) != 0U;
    status.ignition_seen = ignition_seen_.load(std::memory_order_acquire) != 0U;
    status.ignition_raw_on = ignition_raw_on_.load(std::memory_order_acquire) != 0U;
    status.ignition_on = ignitionRecentlyOn(now_ms);
    status.boot_probe_pending = boot_probe_pending_.load(std::memory_order_acquire) != 0U;
    status.entering_sleep = entering_sleep_.load(std::memory_order_acquire) != 0U;
    status.ignition_source = ignition_source_.load(std::memory_order_acquire);
    status.wake_cause = static_cast<ParkedPowerWakeCause>(
        wake_cause_.load(std::memory_order_acquire));
    status.last_primary_age_ms = ageOrZero(
        now_ms, last_primary_ms_.load(std::memory_order_acquire));
    status.last_ignition_on_age_ms = ageOrZero(
        now_ms, last_ignition_on_ms_.load(std::memory_order_acquire));
    status.last_ignition_off_age_ms = ageOrZero(
        now_ms, last_ignition_off_ms_.load(std::memory_order_acquire));
    status.sleep_delay_ms = sleep_delay_ms_.load(std::memory_order_acquire);
    status.wake_timer_seconds = wake_timer_seconds_.load(std::memory_order_acquire);
    status.boot_probe_ms = boot_probe_ms_.load(std::memory_order_acquire);
    status.deep_sleep_entries = deep_sleep_entries_.load(std::memory_order_acquire);
}

void ParkedPowerController::resetIgnitionState() {
    ignition_seen_.store(0U, std::memory_order_release);
    ignition_raw_on_.store(0U, std::memory_order_release);
    ignition_source_.store(0U, std::memory_order_release);
    last_ignition_on_ms_.store(0U, std::memory_order_release);
    last_ignition_off_ms_.store(0U, std::memory_order_release);
}

const char* parkedPowerWakeCauseName(ParkedPowerWakeCause cause) {
    switch (cause) {
    case ParkedPowerWakeCause::CanRx: return "can_rx";
    case ParkedPowerWakeCause::Timer: return "timer";
    case ParkedPowerWakeCause::Gpio: return "gpio";
    case ParkedPowerWakeCause::Other: return "other";
    case ParkedPowerWakeCause::PowerOnOrReset:
    default: return "power_on_or_reset";
    }
}

const char* parkedPowerDecisionName(ParkedPowerDecision decision) {
    switch (decision) {
    case ParkedPowerDecision::SleepIgnitionOff: return "ignition_off";
    case ParkedPowerDecision::SleepIgnitionUnknown: return "ignition_unknown";
    case ParkedPowerDecision::SleepBootProbe: return "boot_probe_no_ignition";
    case ParkedPowerDecision::StayAwake:
    default: return "stay_awake";
    }
}

}  // namespace bored::signalscope
