#include "session_log.hpp"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace bored::signalscope {

namespace {

#pragma pack(push, 1)
struct FileHeader {
    char magic[5];
    uint8_t version;
    uint16_t header_size;
    uint32_t started_ms;
    uint32_t reset_reason;
    uint32_t capacity_bytes;
    uint32_t reserved[3];
};

struct RecordHeader {
    uint8_t type;
    uint8_t flags;
    uint16_t payload_length;
    uint32_t timestamp_us;
    uint32_t sequence;
};

struct FramePayload {
    uint32_t can_id;
    uint8_t direction;
    uint8_t dlc;
    uint8_t stage;
    uint8_t frame_flags;
    uint8_t data[8];
    uint8_t input_data[8];
};

struct AnnotationPrefix {
    uint8_t source_length;
    uint8_t kind_length;
    uint16_t json_length;
};

struct LegacyCheckpointManifest {
    char magic[5];
    uint8_t version;
    uint16_t manifest_size;
    uint32_t committed_bytes;
    uint32_t prefix_crc32;
    uint32_t generation;
    uint32_t manifest_crc32;
};

struct CheckpointManifest {
    char magic[5];
    uint8_t version;
    uint16_t manifest_size;
    uint32_t committed_bytes;
    uint32_t prefix_crc32;
    uint32_t generation;
    uint32_t recovery_reset_reason;
    uint32_t manifest_crc32;
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 32U, "session log header must remain stable");
static_assert(sizeof(RecordHeader) == 12U, "session log record header must remain stable");
static_assert(sizeof(FramePayload) == 24U, "session log frame payload must remain stable");
static_assert(sizeof(LegacyCheckpointManifest) == 24U,
              "legacy checkpoint manifest must remain readable");
static_assert(sizeof(CheckpointManifest) == 28U, "checkpoint manifest must remain stable");

constexpr char kMagic[5] = {'S', 'S', 'L', 'O', 'G'};
constexpr char kCheckpointMagic[5] = {'S', 'S', 'C', 'P', 'K'};
constexpr uint8_t kFormatVersion = 1U;
constexpr uint32_t kEndCrcRequiredMarker = 0x454E4443U;
constexpr size_t kFooterReserve = 512U;
constexpr size_t kMinimumCaptureBytes = 64U * 1024U;
constexpr size_t kPsramReserveBytes = 512U * 1024U;
constexpr size_t kInternalReserveBytes = 64U * 1024U;
constexpr size_t kFilesystemReserveBytes = 384U * 1024U;
constexpr size_t kSaveChunkBytes = 4096U;
constexpr size_t kMaximumDrainPerTick = 1024U;
constexpr uint32_t kCheckpointIntervalMs = 500U;

bool sameMagic(const FileHeader& header) {
    return std::memcmp(header.magic, kMagic, sizeof(kMagic)) == 0 &&
        header.version == kFormatVersion && header.header_size == sizeof(FileHeader);
}

uint32_t updateCrc32(uint32_t crc, const uint8_t* data, size_t length) {
    for (size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

uint32_t crc32(const uint8_t* data, size_t length) {
    return ~updateCrc32(0xFFFFFFFFU, data, length);
}

bool filePrefixCrc32(const char* path, size_t length, uint32_t& result) {
    result = 0U;
    File file = LittleFS.open(path, "r");
    if (!file || file.isDirectory() || static_cast<size_t>(file.size()) < length) {
        if (file) file.close();
        return false;
    }
    uint8_t buffer[512] = {};
    size_t remaining = length;
    size_t bytes_since_yield = 0U;
    uint32_t state = 0xFFFFFFFFU;
    while (remaining > 0U) {
        const size_t chunk = std::min(remaining, sizeof(buffer));
        const size_t read = file.read(buffer, chunk);
        if (read != chunk) {
            file.close();
            return false;
        }
        state = updateCrc32(state, buffer, read);
        remaining -= read;
        bytes_since_yield += read;
        if (bytes_since_yield >= 16U * 1024U) {
            delay(1);
            bytes_since_yield = 0U;
        }
    }
    file.close();
    result = ~state;
    return true;
}

size_t appendEndJsonCrc(char* json, size_t capacity, int base_count) {
    if (json == nullptr || base_count <= 0 || static_cast<size_t>(base_count) >= capacity ||
        json[base_count - 1] != '}') {
        return 0U;
    }
    const uint32_t end_crc = crc32(
        reinterpret_cast<const uint8_t*>(json), static_cast<size_t>(base_count));
    const int suffix = std::snprintf(
        json + base_count - 1, capacity - static_cast<size_t>(base_count - 1),
        ",\"endCrc32\":\"%08lX\"}", static_cast<unsigned long>(end_crc));
    if (suffix <= 0) return 0U;
    const size_t total = static_cast<size_t>(base_count - 1) + static_cast<size_t>(suffix);
    return total < capacity ? total : 0U;
}

enum class SessionFileState : uint8_t {
    Invalid = 0U,
    Checkpoint = 1U,
    Complete = 2U,
    Recovered = 3U,
    CompleteLegacy = 4U,
};

SessionFileState inspectSessionPrefix(const char* path, size_t limit = 0U) {
    if (path == nullptr || !LittleFS.exists(path)) return SessionFileState::Invalid;
    File file = LittleFS.open(path, "r");
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return SessionFileState::Invalid;
    }
    const size_t file_size = static_cast<size_t>(file.size());
    if (limit == 0U) limit = file_size;
    if (limit < sizeof(FileHeader) || limit > file_size) {
        file.close();
        return SessionFileState::Invalid;
    }
    FileHeader header{};
    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
        !sameMagic(header)) {
        file.close();
        return SessionFileState::Invalid;
    }
    const bool end_crc_required = header.reserved[2] == kEndCrcRequiredMarker;

    size_t offset = sizeof(FileHeader);
    size_t next_yield_offset = offset + 16U * 1024U;
    while (offset < limit) {
        if (limit - offset < sizeof(RecordHeader) || !file.seek(offset)) {
            file.close();
            return SessionFileState::Invalid;
        }
        RecordHeader record{};
        if (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record)) {
            file.close();
            return SessionFileState::Invalid;
        }
        const size_t payload_offset = offset + sizeof(record);
        const size_t next = payload_offset + record.payload_length;
        if (next < payload_offset || next > limit) {
            file.close();
            return SessionFileState::Invalid;
        }
        if (offset >= next_yield_offset) {
            delay(1);
            next_yield_offset = offset + 16U * 1024U;
        }

        if (record.type == 1U) {
            if (record.payload_length != offsetof(FramePayload, input_data) &&
                record.payload_length != sizeof(FramePayload)) {
                file.close();
                return SessionFileState::Invalid;
            }
        } else if (record.type == 2U) {
            if (record.payload_length < sizeof(AnnotationPrefix) || !file.seek(payload_offset)) {
                file.close();
                return SessionFileState::Invalid;
            }
            AnnotationPrefix prefix{};
            if (file.read(reinterpret_cast<uint8_t*>(&prefix), sizeof(prefix)) != sizeof(prefix) ||
                prefix.source_length == 0U || prefix.source_length > 31U ||
                prefix.kind_length == 0U || prefix.kind_length > 31U ||
                prefix.json_length == 0U || prefix.json_length > 1024U ||
                sizeof(prefix) + prefix.source_length + prefix.kind_length + prefix.json_length !=
                    record.payload_length) {
                file.close();
                return SessionFileState::Invalid;
            }
        } else if (record.type == 3U) {
            if (next != limit || record.payload_length == 0U || record.payload_length > 512U ||
                !file.seek(payload_offset)) {
                file.close();
                return SessionFileState::Invalid;
            }
            char payload[513] = {};
            if (file.read(reinterpret_cast<uint8_t*>(payload), record.payload_length) !=
                record.payload_length) {
                file.close();
                return SessionFileState::Invalid;
            }
            JsonDocument document;
            if (deserializeJson(document, payload, record.payload_length) ||
                !document["dataBytes"].is<uint32_t>()) {
                file.close();
                return SessionFileState::Invalid;
            }
            const size_t data_bytes = document["dataBytes"].as<uint32_t>();
            const char* crc_text = document["crc32"] | "";
            const char* end_crc_text = document["endCrc32"] | "";
            const char* end_crc_marker = std::strstr(payload, ",\"endCrc32\":\"");
            const bool has_end_crc = end_crc_text[0] != '\0';
            if (data_bytes != offset || std::strlen(crc_text) != 8U ||
                (end_crc_required && !has_end_crc) ||
                (has_end_crc && (std::strlen(end_crc_text) != 8U ||
                                 end_crc_marker == nullptr))) {
                file.close();
                return SessionFileState::Invalid;
            }
            char* crc_end = nullptr;
            const unsigned long expected_crc = std::strtoul(crc_text, &crc_end, 16);
            if (crc_end != crc_text + 8 || *crc_end != '\0') {
                file.close();
                return SessionFileState::Invalid;
            }
            if (has_end_crc) {
                char* end_metadata_crc_end = nullptr;
                const unsigned long expected_end_crc = std::strtoul(
                    end_crc_text, &end_metadata_crc_end, 16);
                uint32_t end_crc_state = updateCrc32(
                    0xFFFFFFFFU, reinterpret_cast<const uint8_t*>(payload),
                    static_cast<size_t>(end_crc_marker - payload));
                const uint8_t closing_brace = static_cast<uint8_t>('}');
                end_crc_state = updateCrc32(end_crc_state, &closing_brace, 1U);
                if (end_metadata_crc_end != end_crc_text + 8 ||
                    *end_metadata_crc_end != '\0' ||
                    ~end_crc_state != static_cast<uint32_t>(expected_end_crc)) {
                    file.close();
                    return SessionFileState::Invalid;
                }
            }
            const bool recovered = std::strcmp(
                document["reason"] | "", "recovered_checkpoint") == 0;
            file.close();
            uint32_t actual_crc = 0U;
            if (!filePrefixCrc32(path, data_bytes, actual_crc) ||
                actual_crc != static_cast<uint32_t>(expected_crc)) {
                return SessionFileState::Invalid;
            }
            if (recovered && has_end_crc) return SessionFileState::Recovered;
            return has_end_crc ? SessionFileState::Complete : SessionFileState::CompleteLegacy;
        } else {
            file.close();
            return SessionFileState::Invalid;
        }
        offset = next;
    }
    file.close();
    return offset == limit ? SessionFileState::Checkpoint : SessionFileState::Invalid;
}

bool writeCheckpointManifestFile(const char* path, size_t committed_bytes,
                                 uint32_t prefix_crc, uint32_t generation,
                                 uint32_t recovery_reset_reason) {
    CheckpointManifest manifest{};
    std::memcpy(manifest.magic, kCheckpointMagic, sizeof(kCheckpointMagic));
    manifest.version = kFormatVersion;
    manifest.manifest_size = sizeof(manifest);
    manifest.committed_bytes = static_cast<uint32_t>(committed_bytes);
    manifest.prefix_crc32 = prefix_crc;
    manifest.generation = generation;
    manifest.recovery_reset_reason = recovery_reset_reason;
    manifest.manifest_crc32 = crc32(
        reinterpret_cast<const uint8_t*>(&manifest),
        offsetof(CheckpointManifest, manifest_crc32));
    File file = LittleFS.open(path, "w");
    if (!file || file.write(reinterpret_cast<const uint8_t*>(&manifest), sizeof(manifest)) !=
            sizeof(manifest)) {
        if (file) file.close();
        return false;
    }
    file.flush();
    file.close();
    File verify = LittleFS.open(path, "r");
    CheckpointManifest persisted{};
    const bool valid = verify && !verify.isDirectory() &&
        static_cast<size_t>(verify.size()) == sizeof(persisted) &&
        verify.read(reinterpret_cast<uint8_t*>(&persisted), sizeof(persisted)) ==
            sizeof(persisted) &&
        std::memcmp(&persisted, &manifest, sizeof(manifest)) == 0;
    if (verify) verify.close();
    return valid;
}

bool readCheckpointManifest(const char* manifest_path, const char* session_path,
                            size_t& committed_bytes, uint32_t& generation,
                            uint32_t& recovery_reset_reason, uint32_t& prefix_crc) {
    committed_bytes = 0U;
    generation = 0U;
    recovery_reset_reason = 0U;
    prefix_crc = 0U;
    if (!LittleFS.exists(manifest_path) || !LittleFS.exists(session_path)) return false;
    File file = LittleFS.open(manifest_path, "r");
    CheckpointManifest manifest{};
    bool read_ok = false;
    if (file && !file.isDirectory() && static_cast<size_t>(file.size()) == sizeof(manifest)) {
        read_ok = file.read(reinterpret_cast<uint8_t*>(&manifest), sizeof(manifest)) ==
            sizeof(manifest) &&
            std::memcmp(manifest.magic, kCheckpointMagic, sizeof(kCheckpointMagic)) == 0 &&
            manifest.version == kFormatVersion && manifest.manifest_size == sizeof(manifest) &&
            crc32(reinterpret_cast<const uint8_t*>(&manifest),
                  offsetof(CheckpointManifest, manifest_crc32)) == manifest.manifest_crc32;
    } else if (file && !file.isDirectory() &&
               static_cast<size_t>(file.size()) == sizeof(LegacyCheckpointManifest)) {
        LegacyCheckpointManifest legacy{};
        read_ok = file.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy)) ==
            sizeof(legacy) &&
            std::memcmp(legacy.magic, kCheckpointMagic, sizeof(kCheckpointMagic)) == 0 &&
            legacy.version == kFormatVersion && legacy.manifest_size == sizeof(legacy) &&
            crc32(reinterpret_cast<const uint8_t*>(&legacy),
                  offsetof(LegacyCheckpointManifest, manifest_crc32)) == legacy.manifest_crc32;
        if (read_ok) {
            std::memcpy(manifest.magic, legacy.magic, sizeof(manifest.magic));
            manifest.version = legacy.version;
            manifest.manifest_size = sizeof(manifest);
            manifest.committed_bytes = legacy.committed_bytes;
            manifest.prefix_crc32 = legacy.prefix_crc32;
            manifest.generation = legacy.generation;
            manifest.recovery_reset_reason = 0U;
        }
    }
    if (file) file.close();
    if (!read_ok || manifest.committed_bytes < sizeof(FileHeader)) {
        return false;
    }
    uint32_t actual_crc = 0U;
    if (!filePrefixCrc32(session_path, manifest.committed_bytes, actual_crc) ||
        actual_crc != manifest.prefix_crc32 ||
        inspectSessionPrefix(session_path, manifest.committed_bytes) !=
            SessionFileState::Checkpoint) {
        return false;
    }
    committed_bytes = manifest.committed_bytes;
    generation = manifest.generation;
    recovery_reset_reason = manifest.recovery_reset_reason;
    prefix_crc = manifest.prefix_crc32;
    return true;
}

bool finalizeRecoveredCheckpoint(const char* path, size_t length, uint32_t reset_reason) {
    if (path == nullptr || length < sizeof(FileHeader) ||
        inspectSessionPrefix(path, length) != SessionFileState::Checkpoint) {
        return false;
    }

    // Truncate through the mounted LittleFS VFS. LittleFS commits the metadata
    // operation transactionally: a reset leaves either the prior temp or the
    // committed prefix, and the alternating manifest remains available to
    // validate/retry both. No filesystem-sized duplicate or destructive rewrite
    // is required.
    char vfs_path[96] = {};
    const int vfs_length = std::snprintf(vfs_path, sizeof(vfs_path), "/littlefs%s", path);
    if (vfs_length <= 0 || static_cast<size_t>(vfs_length) >= sizeof(vfs_path) ||
        ::truncate(vfs_path, static_cast<off_t>(length)) != 0 ||
        inspectSessionPrefix(path) != SessionFileState::Checkpoint) {
        return false;
    }

    File input = LittleFS.open(path, "r");
    if (!input || input.isDirectory()) {
        if (input) input.close();
        return false;
    }
    uint32_t frames = 0U;
    uint32_t annotations = 0U;
    size_t record_offset = sizeof(FileHeader);
    while (record_offset + sizeof(RecordHeader) <= length) {
        RecordHeader record{};
        if (!input.seek(record_offset) ||
            input.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record)) {
            input.close();
            return false;
        }
        if (record.type == 1U) ++frames;
        else if (record.type == 2U) ++annotations;
        record_offset += sizeof(record) + record.payload_length;
    }
    input.close();

    uint32_t prefix_crc = 0U;
    if (!filePrefixCrc32(path, length, prefix_crc)) return false;
    char end_json[512] = {};
    const int base_count = std::snprintf(
        end_json, sizeof(end_json),
        "{\"reason\":\"recovered_checkpoint\",\"durationMs\":0,\"frames\":%lu,"
        "\"annotations\":%lu,\"annotationRejected\":0,\"traceQueueDrops\":0,"
        "\"recordDrops\":0,\"traceDrops\":0,\"truncated\":true,"
        "\"lossUnknown\":true,\"recoveryResetCode\":%lu,\"dataBytes\":%u,"
        "\"crc32\":\"%08lX\"}",
        static_cast<unsigned long>(frames), static_cast<unsigned long>(annotations),
        static_cast<unsigned long>(reset_reason), static_cast<unsigned>(length),
        static_cast<unsigned long>(prefix_crc));
    const size_t end_length = appendEndJsonCrc(end_json, sizeof(end_json), base_count);
    if (end_length == 0U || end_length > 0xFFFFU) return false;

    File output = LittleFS.open(path, "a");
    bool write_ok = static_cast<bool>(output);
    RecordHeader end_record{};
    end_record.type = 3U;
    end_record.payload_length = static_cast<uint16_t>(end_length);
    end_record.timestamp_us = micros();
    if (write_ok && output.write(reinterpret_cast<const uint8_t*>(&end_record),
                                 sizeof(end_record)) != sizeof(end_record)) {
        write_ok = false;
    }
    if (write_ok && output.write(reinterpret_cast<const uint8_t*>(end_json), end_length) !=
            end_length) {
        write_ok = false;
    }
    if (output) {
        output.flush();
        output.close();
    }
    // Never delete on failure: the committed manifest still authenticates the
    // prefix, and the next boot can truncate a partial End and retry safely.
    return write_ok && inspectSessionPrefix(path) == SessionFileState::Recovered;
}

}  // namespace

bool SessionLogRecorder::begin() {
    SessionFileState primary_state = inspectSessionPrefix(kLogPath);
    SessionFileState backup_state = inspectSessionPrefix(kBackupPath);
    if (primary_state == SessionFileState::Invalid) {
        if (LittleFS.exists(kLogPath)) static_cast<void>(LittleFS.remove(kLogPath));
        if (backup_state != SessionFileState::Invalid &&
            LittleFS.rename(kBackupPath, kLogPath)) {
            primary_state = inspectSessionPrefix(kLogPath);
            backup_state = SessionFileState::Invalid;
        }
    }
    // A checksum-verified primary is authoritative. Only then is it safe to
    // discard a leftover transaction backup without losing the last good log.
    if (primary_state == SessionFileState::Complete && LittleFS.exists(kBackupPath)) {
        static_cast<void>(LittleFS.remove(kBackupPath));
        backup_state = SessionFileState::Invalid;
    } else if (backup_state == SessionFileState::Invalid && LittleFS.exists(kBackupPath)) {
        static_cast<void>(LittleFS.remove(kBackupPath));
    }

    const char* candidate_path = nullptr;
    SessionFileState candidate_state = inspectSessionPrefix(kTemporaryPath);
    bool recoverable_manifest = false;
    if (candidate_state == SessionFileState::Complete ||
        candidate_state == SessionFileState::Recovered ||
        candidate_state == SessionFileState::CompleteLegacy) {
        candidate_path = kTemporaryPath;
    } else if (LittleFS.exists(kTemporaryPath)) {
        size_t bytes0 = 0U;
        size_t bytes1 = 0U;
        uint32_t generation0 = 0U;
        uint32_t generation1 = 0U;
        uint32_t reset0 = 0U;
        uint32_t reset1 = 0U;
        uint32_t prefix_crc0 = 0U;
        uint32_t prefix_crc1 = 0U;
        const bool valid0 = readCheckpointManifest(
            kCheckpointPath0, kTemporaryPath, bytes0, generation0, reset0, prefix_crc0);
        const bool valid1 = readCheckpointManifest(
            kCheckpointPath1, kTemporaryPath, bytes1, generation1, reset1, prefix_crc1);
        size_t committed_bytes = 0U;
        uint32_t committed_generation = 0U;
        uint32_t committed_reset = 0U;
        uint32_t committed_crc = 0U;
        const char* committed_manifest = nullptr;
        if (valid0 && (!valid1 || generation0 > generation1 ||
                       (generation0 == generation1 && bytes0 >= bytes1))) {
            committed_bytes = bytes0;
            committed_generation = generation0;
            committed_reset = reset0;
            committed_crc = prefix_crc0;
            committed_manifest = kCheckpointPath0;
        } else if (valid1) {
            committed_bytes = bytes1;
            committed_generation = generation1;
            committed_reset = reset1;
            committed_crc = prefix_crc1;
            committed_manifest = kCheckpointPath1;
        }
        recoverable_manifest = committed_bytes > 0U;
        bool recovery_provenance_ready = recoverable_manifest;
        if (recovery_provenance_ready && committed_reset == 0U) {
            committed_reset = static_cast<uint32_t>(esp_reset_reason());
            const char* alternate_manifest = committed_manifest == kCheckpointPath0
                ? kCheckpointPath1
                : kCheckpointPath0;
            recovery_provenance_ready = writeCheckpointManifestFile(
                alternate_manifest, committed_bytes, committed_crc,
                committed_generation + 1U, committed_reset);
        }
        if (recovery_provenance_ready && finalizeRecoveredCheckpoint(
                kTemporaryPath, committed_bytes, committed_reset)) {
            candidate_path = kTemporaryPath;
            candidate_state = SessionFileState::Recovered;
        }
    }

    if (candidate_path != nullptr) {
        // Preserve a known-good primary until the candidate has been renamed
        // and independently revalidated. If a valid backup already exists it
        // serves that role; otherwise create one transactionally.
        bool primary_preserved = !LittleFS.exists(kLogPath);
        if (LittleFS.exists(kLogPath)) {
            backup_state = inspectSessionPrefix(kBackupPath);
            if (backup_state != SessionFileState::Invalid) {
                primary_preserved = LittleFS.remove(kLogPath);
            } else {
                if (LittleFS.exists(kBackupPath)) static_cast<void>(LittleFS.remove(kBackupPath));
                primary_preserved = LittleFS.rename(kLogPath, kBackupPath);
            }
        }
        bool promoted = false;
        if (primary_preserved && !LittleFS.exists(kLogPath) &&
            LittleFS.rename(candidate_path, kLogPath)) {
            promoted = inspectSessionPrefix(kLogPath) == candidate_state;
        }
        if (promoted) {
            // Keep the prior completed session behind recovered/truncated
            // evidence. A normal completed candidate can replace it fully.
            if (candidate_state == SessionFileState::Complete &&
                LittleFS.exists(kBackupPath)) {
                static_cast<void>(LittleFS.remove(kBackupPath));
            }
            removeCheckpointArtifacts(true);
        } else {
            if (LittleFS.exists(kLogPath)) static_cast<void>(LittleFS.remove(kLogPath));
            if (LittleFS.exists(kBackupPath)) {
                static_cast<void>(LittleFS.rename(kBackupPath, kLogPath));
            }
        }
    } else if (!recoverable_manifest && LittleFS.exists(kTemporaryPath)) {
        removeCheckpointArtifacts(true);
    } else if (!LittleFS.exists(kTemporaryPath)) {
        // Stale manifests without their data file cannot recover anything.
        removeCheckpointArtifacts(false);
    }

    inspectExisting();
    return true;
}

bool SessionLogRecorder::allocateCapture(size_t& capacity, const char*& error) {
    error = nullptr;
    capacity = 0U;
    releaseCapture();

    const size_t fs_total = LittleFS.totalBytes();
    const size_t fs_used = LittleFS.usedBytes();
    const size_t fs_free = fs_total > fs_used ? fs_total - fs_used : 0U;
    if (fs_free <= kFilesystemReserveBytes + kMinimumCaptureBytes) {
        error = "filesystem_space_low";
        return false;
    }
    size_t requested = std::min(kMaximumCaptureBytes, fs_free - kFilesystemReserveBytes);

    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free > kPsramReserveBytes + kMinimumCaptureBytes) {
        size_t candidate = std::min(requested, psram_free - kPsramReserveBytes);
        candidate = std::min(
            candidate, heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        while (capture_ == nullptr && candidate >= kMinimumCaptureBytes) {
            capture_ = static_cast<uint8_t*>(
                heap_caps_malloc(candidate, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (capture_ == nullptr) candidate /= 2U;
        }
        if (capture_ != nullptr) requested = candidate;
    }

    // SignalScope remains usable on a target without PSRAM, but the fallback
    // capture is deliberately small so logging cannot starve the control heap.
    if (capture_ == nullptr) {
        const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (internal_free <= kInternalReserveBytes + kMinimumCaptureBytes) {
            error = "recording_memory_unavailable";
            return false;
        }
        size_t candidate = std::min(requested, internal_free - kInternalReserveBytes);
        candidate = std::min(candidate, static_cast<size_t>(128U * 1024U));
        candidate = std::min(candidate, heap_caps_get_largest_free_block(
            MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        while (capture_ == nullptr && candidate >= kMinimumCaptureBytes) {
            capture_ = static_cast<uint8_t*>(
                heap_caps_malloc(candidate, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (capture_ == nullptr) candidate /= 2U;
        }
        if (capture_ != nullptr) requested = candidate;
    }
    if (capture_ == nullptr || requested < kMinimumCaptureBytes) {
        releaseCapture();
        error = "recording_memory_unavailable";
        return false;
    }
    capture_capacity_ = requested;
    session_capacity_ = requested;
    capacity = requested;
    return true;
}

bool SessionLogRecorder::start(CanTraceQueue& trace, CanTraceScope scope, bool durable,
                               const char* application_id, const char*& error) {
    error = nullptr;
    if (recording_ || stopping_ || saving_) {
        error = "recording_busy";
        return false;
    }
    if (save_failed_) {
        error = "unsaved_capture_pending";
        return false;
    }
    if (!trace.available()) {
        error = "trace_queue_unavailable";
        return false;
    }
    if (LittleFS.exists(kTemporaryPath)) {
        error = "recording_recovery_pending";
        return false;
    }
    removeCheckpointArtifacts(false);
    size_t capacity = 0U;
    if (!allocateCapture(capacity, error)) return false;

    recording_ = false;
    stopping_ = false;
    saving_ = false;
    complete_ = false;
    recovered_ = false;
    truncated_ = false;
    save_failed_ = false;
    durable_ = durable;
    checkpoint_failed_ = false;
    scope_ = scope;
    started_ms_ = millis();
    stopped_ms_ = 0U;
    frame_records_ = 0U;
    annotation_records_ = 0U;
    annotation_rejected_ = 0U;
    annotation_bytes_ = 0U;
    trace_queue_drops_ = 0U;
    record_drops_ = 0U;
    trace_drops_ = 0U;
    capture_size_ = 0U;
    save_offset_ = 0U;
    checkpoint_offset_ = 0U;
    checkpoint_target_ = 0U;
    capture_crc_state_ = 0xFFFFFFFFU;
    checkpoint_crc_state_ = 0xFFFFFFFFU;
    checkpoint_generation_ = 0U;
    checkpoint_manifest_slot_ = 0U;
    next_checkpoint_ms_ = started_ms_ + kCheckpointIntervalMs;
    recovery_reset_reason_ = 0U;
    final_crc32_ = 0U;
    final_prefix_bytes_ = 0U;
    stop_reason_[0] = '\0';
    last_error_[0] = '\0';

    FileHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kFormatVersion;
    header.header_size = sizeof(FileHeader);
    header.started_ms = started_ms_;
    header.reset_reason = static_cast<uint32_t>(esp_reset_reason());
    header.capacity_bytes = static_cast<uint32_t>(capacity);
    header.reserved[0] = static_cast<uint32_t>(scope);
    header.reserved[1] = durable ? 1U : 0U;
    header.reserved[2] = kEndCrcRequiredMarker;
    if (!appendBytes(&header, sizeof(header), false)) {
        releaseCapture();
        error = "recording_header_failed";
        return false;
    }
    if (!trace.start(scope)) {
        releaseCapture();
        error = "trace_start_failed";
        return false;
    }
    recording_ = true;

    char start_json[256] = {};
    const int count = std::snprintf(
        start_json, sizeof(start_json),
        "{\"application\":\"%s\",\"scope\":\"%s\",\"durable\":%s,"
        "\"capacityBytes\":%u,"
        "\"resetReason\":\"%s\",\"resetCode\":%u}",
        application_id == nullptr ? "" : application_id, CanTraceQueue::scopeName(scope),
        durable ? "true" : "false", static_cast<unsigned>(capacity),
        resetReasonName(header.reset_reason),
        static_cast<unsigned>(header.reset_reason));
    if (count > 0 && static_cast<size_t>(count) < sizeof(start_json)) {
        static_cast<void>(appendAnnotation(
            "signalscope", "session_start", start_json, static_cast<size_t>(count), micros()));
    }
    Serial.printf("[session-log] started scope=%s capacity=%u\n",
                  CanTraceQueue::scopeName(scope), static_cast<unsigned>(capacity));
    return true;
}

bool SessionLogRecorder::requestStop(CanTraceQueue& trace, const char* reason) {
    if (!recording_ || stopping_ || saving_) return false;
    beginStopping(trace, reason == nullptr ? "user" : reason, false);
    return true;
}

void SessionLogRecorder::beginStopping(CanTraceQueue& trace, const char* reason, bool truncated) {
    trace.stop();
    closeCheckpoint();
    recording_ = false;
    stopping_ = true;
    truncated_ = truncated_ || truncated;
    std::snprintf(stop_reason_, sizeof(stop_reason_), "%s", reason == nullptr ? "stopped" : reason);
}

void SessionLogRecorder::tick(CanTraceQueue& trace, uint32_t now_ms) {
    if (saving_) {
        processSaveChunk();
        return;
    }
    if (!recording_ && !stopping_) return;

    CanTraceEvent event{};
    size_t drained = 0U;
    while (drained < kMaximumDrainPerTick && trace.pop(event)) {
        if (!appendFrame(event)) {
            ++record_drops_;
            beginStopping(trace, "capacity", true);
            // The capture is full, so no queued record can fit. Drain the
            // stopped producer immediately and account for every omitted event
            // instead of spending one application tick per discard.
            while (trace.pop(event)) ++record_drops_;
            break;
        }
        ++drained;
    }
    trace_queue_drops_ = trace.dropped();
    trace_drops_ = trace_queue_drops_ + record_drops_;
    if (recording_ && capture_size_ + kFooterReserve >= capture_capacity_) {
        beginStopping(trace, "capacity", true);
    }
    if (recording_) processCheckpoint(now_ms);
    if (stopping_ && trace.pending() == 0U && trace.producerIdle()) finalizeMemory(now_ms);
}

bool SessionLogRecorder::appendFrame(const CanTraceEvent& event) {
    FramePayload payload{};
    payload.can_id = event.can_id;
    payload.direction = static_cast<uint8_t>(event.direction);
    payload.dlc = event.dlc <= 8U ? event.dlc : 8U;
    payload.stage = static_cast<uint8_t>(event.stage);
    payload.frame_flags = event.flags;
    std::memcpy(payload.data, event.data, sizeof(payload.data));
    const bool has_input = (event.flags & CanTraceHasInput) != 0U;
    if (has_input) std::memcpy(payload.input_data, event.input_data, sizeof(payload.input_data));
    const size_t payload_length = has_input ? sizeof(payload) : offsetof(FramePayload, input_data);
    if (!appendRecord(RecordType::Frame, 0U, event.timestamp_us, event.sequence,
                      &payload, payload_length)) return false;
    ++frame_records_;
    return true;
}

bool SessionLogRecorder::appendAnnotation(const char* source, const char* kind,
                                          const char* json, size_t json_length,
                                          uint32_t timestamp_us) {
    if ((!recording_ && !stopping_) || source == nullptr || kind == nullptr || json == nullptr) {
        return false;
    }
    const size_t source_length = std::strlen(source);
    const size_t kind_length = std::strlen(kind);
    if (source_length == 0U || source_length > kMaximumSourceBytes ||
        kind_length == 0U || kind_length > kMaximumKindBytes ||
        json_length == 0U || json_length > kMaximumAnnotationJsonBytes) {
        ++annotation_rejected_;
        return false;
    }
    const size_t payload_length = sizeof(AnnotationPrefix) + source_length + kind_length + json_length;
    const size_t record_length = sizeof(RecordHeader) + payload_length;
    // An application is a guest in the framework-owned capture. Keep at least
    // three quarters of live capacity available to authoritative CAN records.
    const size_t annotation_budget = capture_capacity_ / 4U;
    if (annotation_bytes_ + record_length > annotation_budget) {
        ++annotation_rejected_;
        return false;
    }
    uint8_t payload[sizeof(AnnotationPrefix) + kMaximumSourceBytes + kMaximumKindBytes +
                    kMaximumAnnotationJsonBytes] = {};
    AnnotationPrefix prefix{};
    prefix.source_length = static_cast<uint8_t>(source_length);
    prefix.kind_length = static_cast<uint8_t>(kind_length);
    prefix.json_length = static_cast<uint16_t>(json_length);
    std::memcpy(payload, &prefix, sizeof(prefix));
    size_t used = sizeof(prefix);
    std::memcpy(payload + used, source, source_length);
    used += source_length;
    std::memcpy(payload + used, kind, kind_length);
    used += kind_length;
    std::memcpy(payload + used, json, json_length);
    if (!appendRecord(RecordType::Annotation, 0U, timestamp_us, 0U, payload, payload_length)) {
        ++annotation_rejected_;
        return false;
    }
    ++annotation_records_;
    annotation_bytes_ += record_length;
    return true;
}

bool SessionLogRecorder::appendRecord(RecordType type, uint8_t flags, uint32_t timestamp_us,
                                      uint32_t sequence, const void* payload,
                                      size_t payload_length, bool use_footer_reserve) {
    if (payload_length > 0xFFFFU || (payload_length > 0U && payload == nullptr)) return false;
    RecordHeader header{};
    header.type = static_cast<uint8_t>(type);
    header.flags = flags;
    header.payload_length = static_cast<uint16_t>(payload_length);
    header.timestamp_us = timestamp_us;
    header.sequence = sequence;
    const size_t total = sizeof(header) + payload_length;
    const size_t reserve = use_footer_reserve ? kFooterReserve : 0U;
    if (capture_ == nullptr || capture_size_ + total + reserve > capture_capacity_) return false;
    std::memcpy(capture_ + capture_size_, &header, sizeof(header));
    capture_size_ += sizeof(header);
    capture_crc_state_ = updateCrc32(
        capture_crc_state_, reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    if (payload_length > 0U) {
        std::memcpy(capture_ + capture_size_, payload, payload_length);
        capture_size_ += payload_length;
        capture_crc_state_ = updateCrc32(
            capture_crc_state_, static_cast<const uint8_t*>(payload), payload_length);
    }
    return true;
}

bool SessionLogRecorder::appendBytes(const void* data, size_t length, bool use_footer_reserve) {
    if (data == nullptr || length == 0U || capture_ == nullptr) return false;
    const size_t reserve = use_footer_reserve ? kFooterReserve : 0U;
    if (capture_size_ + length + reserve > capture_capacity_) return false;
    std::memcpy(capture_ + capture_size_, data, length);
    capture_size_ += length;
    capture_crc_state_ = updateCrc32(
        capture_crc_state_, static_cast<const uint8_t*>(data), length);
    return true;
}

void SessionLogRecorder::finalizeMemory(uint32_t now_ms) {
    stopped_ms_ = now_ms;
    final_prefix_bytes_ = capture_size_;
    // Every append updates the live state, so finalization is constant-time
    // instead of scanning several megabytes of PSRAM at higher task priority.
    final_crc32_ = ~capture_crc_state_;
    char end_json[384] = {};
    const int base_count = std::snprintf(
        end_json, sizeof(end_json),
        "{\"reason\":\"%s\",\"durationMs\":%lu,\"frames\":%lu,"
        "\"annotations\":%lu,\"annotationRejected\":%lu,"
        "\"traceQueueDrops\":%lu,\"recordDrops\":%lu,\"traceDrops\":%lu,"
        "\"truncated\":%s,\"dataBytes\":%u,\"crc32\":\"%08lX\"}",
        stop_reason_, static_cast<unsigned long>(stopped_ms_ - started_ms_),
        static_cast<unsigned long>(frame_records_),
        static_cast<unsigned long>(annotation_records_),
        static_cast<unsigned long>(annotation_rejected_),
        static_cast<unsigned long>(trace_queue_drops_),
        static_cast<unsigned long>(record_drops_),
        static_cast<unsigned long>(trace_drops_), truncated_ ? "true" : "false",
        static_cast<unsigned>(final_prefix_bytes_), static_cast<unsigned long>(final_crc32_));
    const size_t count = appendEndJsonCrc(end_json, sizeof(end_json), base_count);
    const bool end_written = count > 0U &&
        appendRecord(RecordType::End, 0U, micros(), 0U, end_json,
                     count, false);
    stopping_ = false;
    if (!end_written) {
        complete_ = false;
        saving_ = false;
        save_failed_ = true;
        std::snprintf(last_error_, sizeof(last_error_), "end_record_failed");
        return;
    }
    saving_ = true;
    complete_ = true;
    save_failed_ = false;
    save_offset_ = 0U;
    Serial.printf("[session-log] stopped reason=%s frames=%lu bytes=%u drops=%lu\n",
                  stop_reason_, static_cast<unsigned long>(frame_records_),
                  static_cast<unsigned>(capture_size_), static_cast<unsigned long>(trace_drops_));
}

void SessionLogRecorder::closeCheckpoint() {
    if (!checkpoint_file_) return;
    checkpoint_file_.flush();
    checkpoint_file_.close();
}

bool SessionLogRecorder::commitCheckpoint(size_t committed_bytes) {
    if (capture_ == nullptr || committed_bytes < sizeof(FileHeader) ||
        committed_bytes > checkpoint_offset_) {
        return false;
    }
    const char* path = checkpoint_manifest_slot_ == 0U
        ? kCheckpointPath0
        : kCheckpointPath1;
    const bool valid = writeCheckpointManifestFile(
        path, committed_bytes, ~checkpoint_crc_state_, ++checkpoint_generation_, 0U);
    if (valid) checkpoint_manifest_slot_ ^= 1U;
    return valid;
}

void SessionLogRecorder::removeCheckpointArtifacts(bool remove_temporary) {
    closeCheckpoint();
    if (remove_temporary && LittleFS.exists(kTemporaryPath)) {
        static_cast<void>(LittleFS.remove(kTemporaryPath));
    }
    if (LittleFS.exists(kCheckpointPath0)) static_cast<void>(LittleFS.remove(kCheckpointPath0));
    if (LittleFS.exists(kCheckpointPath1)) static_cast<void>(LittleFS.remove(kCheckpointPath1));
}

void SessionLogRecorder::processCheckpoint(uint32_t now_ms) {
    if (!durable_ || checkpoint_failed_ || !recording_) return;
    if (checkpoint_target_ == 0U) {
        if (static_cast<int32_t>(now_ms - next_checkpoint_ms_) < 0) return;
        checkpoint_target_ = capture_size_;
        if (checkpoint_target_ <= checkpoint_offset_) {
            checkpoint_target_ = 0U;
            next_checkpoint_ms_ = now_ms + kCheckpointIntervalMs;
            return;
        }
    }

    if (!checkpoint_file_) {
        if (checkpoint_offset_ == 0U) {
            if (LittleFS.exists(kTemporaryPath)) static_cast<void>(LittleFS.remove(kTemporaryPath));
            checkpoint_file_ = LittleFS.open(kTemporaryPath, "w");
        } else {
            File existing = LittleFS.open(kTemporaryPath, "r");
            const bool size_matches = existing && !existing.isDirectory() &&
                static_cast<size_t>(existing.size()) == checkpoint_offset_;
            if (existing) existing.close();
            if (size_matches) checkpoint_file_ = LittleFS.open(kTemporaryPath, "a");
        }
        if (!checkpoint_file_) {
            checkpoint_failed_ = true;
            std::snprintf(last_error_, sizeof(last_error_), "live_checkpoint_open_failed");
            return;
        }
    }

    const size_t remaining = checkpoint_target_ - checkpoint_offset_;
    const size_t chunk = std::min(remaining, kSaveChunkBytes);
    if (chunk > 0U) {
        const size_t written = checkpoint_file_.write(capture_ + checkpoint_offset_, chunk);
        if (written != chunk) {
            checkpoint_file_.close();
            checkpoint_failed_ = true;
            std::snprintf(last_error_, sizeof(last_error_), "live_checkpoint_write_failed");
            return;
        }
        checkpoint_crc_state_ = updateCrc32(
            checkpoint_crc_state_, capture_ + checkpoint_offset_, written);
        checkpoint_offset_ += written;
    }
    if (checkpoint_offset_ >= checkpoint_target_) {
        closeCheckpoint();
        if (!commitCheckpoint(checkpoint_offset_)) {
            checkpoint_failed_ = true;
            std::snprintf(last_error_, sizeof(last_error_), "live_checkpoint_commit_failed");
            return;
        }
        checkpoint_target_ = 0U;
        next_checkpoint_ms_ = now_ms + kCheckpointIntervalMs;
    }
}

void SessionLogRecorder::processSaveChunk() {
    if (capture_ == nullptr || capture_size_ == 0U) {
        failSave("capture_missing");
        return;
    }
    if (!save_file_) {
        bool resumed = false;
        if (durable_ && !checkpoint_failed_ && checkpoint_offset_ > 0U &&
            LittleFS.exists(kTemporaryPath)) {
            File existing = LittleFS.open(kTemporaryPath, "r");
            const bool size_matches = existing && !existing.isDirectory() &&
                static_cast<size_t>(existing.size()) == checkpoint_offset_;
            if (existing) existing.close();
            if (size_matches) {
                save_file_ = LittleFS.open(kTemporaryPath, "a");
                if (save_file_) {
                    save_offset_ = checkpoint_offset_;
                    resumed = true;
                }
            }
        }
        if (!resumed) {
            if (LittleFS.exists(kTemporaryPath)) static_cast<void>(LittleFS.remove(kTemporaryPath));
            save_file_ = LittleFS.open(kTemporaryPath, "w");
            save_offset_ = 0U;
        }
        if (!save_file_) {
            failSave("temporary_open_failed");
            return;
        }
    }

    const size_t remaining = capture_size_ - save_offset_;
    const size_t chunk = std::min(remaining, kSaveChunkBytes);
    if (chunk > 0U) {
        const size_t written = save_file_.write(capture_ + save_offset_, chunk);
        if (written != chunk) {
            failSave("filesystem_write_failed");
            return;
        }
        save_offset_ += written;
        return;
    }

    save_file_.flush();
    save_file_.close();
    if (inspectSessionPrefix(kTemporaryPath) != SessionFileState::Complete) {
        failSave("temporary_validation_failed");
        return;
    }

    bool previous_preserved = !LittleFS.exists(kLogPath);
    if (LittleFS.exists(kLogPath)) {
        if (inspectSessionPrefix(kBackupPath) != SessionFileState::Invalid) {
            previous_preserved = LittleFS.remove(kLogPath);
        } else {
            if (LittleFS.exists(kBackupPath)) static_cast<void>(LittleFS.remove(kBackupPath));
            previous_preserved = LittleFS.rename(kLogPath, kBackupPath);
        }
    }
    if (!previous_preserved) {
        failSave("backup_rename_failed");
        return;
    }
    const bool renamed = LittleFS.rename(kTemporaryPath, kLogPath);
    File promoted = renamed ? LittleFS.open(kLogPath, "r") : File{};
    const bool promoted_size_matches = promoted && !promoted.isDirectory() &&
        static_cast<size_t>(promoted.size()) == capture_size_;
    if (promoted) promoted.close();
    if (!renamed || !promoted_size_matches) {
        if (LittleFS.exists(kLogPath)) static_cast<void>(LittleFS.remove(kLogPath));
        if (LittleFS.exists(kBackupPath)) {
            static_cast<void>(LittleFS.rename(kBackupPath, kLogPath));
        }
        failSave("log_rename_failed");
        return;
    }
    if (LittleFS.exists(kBackupPath)) static_cast<void>(LittleFS.remove(kBackupPath));
    removeCheckpointArtifacts(false);

    saved_bytes_ = capture_size_;
    available_ = true;
    recovered_ = false;
    saving_ = false;
    save_failed_ = false;
    last_error_[0] = '\0';
    releaseCapture();
    Serial.printf("[session-log] saved bytes=%u\n", static_cast<unsigned>(saved_bytes_));
}

void SessionLogRecorder::failSave(const char* error) {
    if (save_file_) save_file_.close();
    saving_ = false;
    save_failed_ = true;
    std::snprintf(last_error_, sizeof(last_error_), "%s", error == nullptr ? "save_failed" : error);
    Serial.printf("[session-log] save failed: %s\n", last_error_);
}

bool SessionLogRecorder::retrySave(const char*& error) {
    error = nullptr;
    if (recording_ || stopping_ || saving_) {
        error = "recording_busy";
        return false;
    }
    if (!save_failed_ || capture_ == nullptr || capture_size_ == 0U) {
        error = "no_failed_save";
        return false;
    }
    removeCheckpointArtifacts(true);
    save_offset_ = 0U;
    checkpoint_offset_ = 0U;
    checkpoint_target_ = 0U;
    capture_crc_state_ = 0xFFFFFFFFU;
    checkpoint_crc_state_ = 0xFFFFFFFFU;
    checkpoint_generation_ = 0U;
    checkpoint_manifest_slot_ = 0U;
    saving_ = true;
    save_failed_ = false;
    last_error_[0] = '\0';
    return true;
}

bool SessionLogRecorder::clear() {
    if (recording_ || stopping_ || saving_) return false;
    closeCheckpoint();
    bool ok = true;
    if (LittleFS.exists(kLogPath)) ok = LittleFS.remove(kLogPath) && ok;
    if (LittleFS.exists(kTemporaryPath)) ok = LittleFS.remove(kTemporaryPath) && ok;
    if (LittleFS.exists(kBackupPath)) ok = LittleFS.remove(kBackupPath) && ok;
    if (LittleFS.exists(kCheckpointPath0)) ok = LittleFS.remove(kCheckpointPath0) && ok;
    if (LittleFS.exists(kCheckpointPath1)) ok = LittleFS.remove(kCheckpointPath1) && ok;
    if (!ok) return false;
    releaseCapture();
    available_ = false;
    complete_ = false;
    recovered_ = false;
    truncated_ = false;
    save_failed_ = false;
    durable_ = false;
    checkpoint_failed_ = false;
    saved_bytes_ = 0U;
    session_capacity_ = 0U;
    started_ms_ = 0U;
    stopped_ms_ = 0U;
    frame_records_ = 0U;
    annotation_records_ = 0U;
    annotation_rejected_ = 0U;
    annotation_bytes_ = 0U;
    trace_queue_drops_ = 0U;
    record_drops_ = 0U;
    trace_drops_ = 0U;
    checkpoint_offset_ = 0U;
    checkpoint_target_ = 0U;
    next_checkpoint_ms_ = 0U;
    recovery_reset_reason_ = 0U;
    capture_crc_state_ = 0xFFFFFFFFU;
    final_crc32_ = 0U;
    final_prefix_bytes_ = 0U;
    stop_reason_[0] = '\0';
    last_error_[0] = '\0';
    return true;
}

void SessionLogRecorder::releaseCapture() {
    closeCheckpoint();
    if (capture_ != nullptr) heap_caps_free(capture_);
    capture_ = nullptr;
    capture_capacity_ = 0U;
    capture_size_ = 0U;
    save_offset_ = 0U;
    checkpoint_offset_ = 0U;
    checkpoint_target_ = 0U;
    capture_crc_state_ = 0xFFFFFFFFU;
    checkpoint_crc_state_ = 0xFFFFFFFFU;
    checkpoint_generation_ = 0U;
    checkpoint_manifest_slot_ = 0U;
    next_checkpoint_ms_ = 0U;
}

void SessionLogRecorder::inspectExisting() {
    available_ = false;
    complete_ = false;
    recovered_ = false;
    truncated_ = false;
    durable_ = false;
    checkpoint_failed_ = false;
    recovery_reset_reason_ = 0U;
    capture_crc_state_ = 0xFFFFFFFFU;
    final_crc32_ = 0U;
    final_prefix_bytes_ = 0U;
    saved_bytes_ = 0U;
    session_capacity_ = 0U;
    frame_records_ = 0U;
    annotation_records_ = 0U;
    annotation_rejected_ = 0U;
    annotation_bytes_ = 0U;
    trace_queue_drops_ = 0U;
    record_drops_ = 0U;
    trace_drops_ = 0U;
    stopped_ms_ = 0U;
    stop_reason_[0] = '\0';
    last_error_[0] = '\0';
    const SessionFileState file_state = inspectSessionPrefix(kLogPath);
    if (file_state == SessionFileState::Invalid) return;

    File file = LittleFS.open(kLogPath, "r");
    if (!file) return;
    FileHeader header{};
    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
        !sameMagic(header)) {
        file.close();
        return;
    }
    available_ = true;
    complete_ = file_state == SessionFileState::Complete ||
        file_state == SessionFileState::Recovered ||
        file_state == SessionFileState::CompleteLegacy;
    truncated_ = file_state == SessionFileState::Checkpoint ||
        file_state == SessionFileState::Recovered;
    saved_bytes_ = static_cast<size_t>(file.size());
    started_ms_ = header.started_ms;
    session_capacity_ = header.capacity_bytes;
    if (header.reserved[0] <= static_cast<uint32_t>(CanTraceScope::Mutated)) {
        scope_ = static_cast<CanTraceScope>(header.reserved[0]);
    }
    durable_ = header.reserved[1] != 0U;
    recovery_reset_reason_ = header.reserved[2] == kEndCrcRequiredMarker
        ? 0U
        : header.reserved[2];

    while (file.available()) {
        RecordHeader record{};
        const size_t read = file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record));
        if (read == 0U) break;
        if (read != sizeof(record) || record.payload_length > 4096U ||
            static_cast<size_t>(file.position()) + record.payload_length > saved_bytes_) {
            truncated_ = true;
            break;
        }
        const uint32_t payload_end = static_cast<uint32_t>(file.position() + record.payload_length);
        bool payload_consumed = false;
        if (record.type == static_cast<uint8_t>(RecordType::Frame)) ++frame_records_;
        else if (record.type == static_cast<uint8_t>(RecordType::Annotation)) {
            ++annotation_records_;
            annotation_bytes_ += sizeof(record) + record.payload_length;
        }
        else if (record.type == static_cast<uint8_t>(RecordType::End)) {
            if (record.payload_length > 0U && record.payload_length <= 512U) {
                char payload[513] = {};
                if (file.read(reinterpret_cast<uint8_t*>(payload), record.payload_length) ==
                    record.payload_length) {
                    payload_consumed = true;
                    JsonDocument document;
                    if (!deserializeJson(document, payload, record.payload_length)) {
                        const uint32_t duration_ms = document["durationMs"] | 0U;
                        stopped_ms_ = started_ms_ + duration_ms;
                        std::snprintf(stop_reason_, sizeof(stop_reason_), "%s",
                                      document["reason"] | "");
                        trace_queue_drops_ = document["traceQueueDrops"] | 0U;
                        record_drops_ = document["recordDrops"] | 0U;
                        trace_drops_ = document["traceDrops"] |
                            static_cast<uint32_t>(trace_queue_drops_ + record_drops_);
                        annotation_rejected_ = document["annotationRejected"] | 0U;
                        truncated_ = document["truncated"] | truncated_;
                        recovery_reset_reason_ = document["recoveryResetCode"] |
                            recovery_reset_reason_;
                        final_prefix_bytes_ = document["dataBytes"] | 0U;
                        const char* crc_text = document["crc32"] | "";
                        final_crc32_ = static_cast<uint32_t>(std::strtoul(crc_text, nullptr, 16));
                    }
                }
            }
        }
        if (!payload_consumed && !file.seek(payload_end)) {
            truncated_ = true;
            break;
        }
    }
    file.close();
    complete_ = file_state == SessionFileState::Complete ||
        file_state == SessionFileState::Recovered ||
        file_state == SessionFileState::CompleteLegacy;
    recovered_ = recovery_reset_reason_ != 0U ||
        file_state == SessionFileState::Checkpoint ||
        file_state == SessionFileState::Recovered;
}

bool SessionLogRecorder::writeStatusJson(char* output, size_t capacity) const {
    if (output == nullptr || capacity == 0U) return false;
    const uint32_t now_ms = millis();
    const uint32_t duration_ms = (recording_ || stopping_)
        ? now_ms - started_ms_
        : (stopped_ms_ != 0U ? stopped_ms_ - started_ms_ : 0U);
    const int count = std::snprintf(
        output, capacity,
        "{\"ok\":true,\"recording\":%s,\"stopping\":%s,\"saving\":%s,"
        "\"available\":%s,\"complete\":%s,\"recovered\":%s,"
        "\"truncated\":%s,\"saveFailed\":%s,\"scope\":\"%s\","
        "\"durable\":%s,\"checkpointFailed\":%s,\"checkpointBytes\":%u,"
        "\"recoveryResetCode\":%lu,\"recoveryResetReason\":\"%s\","
        "\"durationMs\":%lu,\"captureBytes\":%u,\"capacityBytes\":%u,"
        "\"savedBytes\":%u,\"frameRecords\":%lu,\"annotationRecords\":%lu,"
        "\"annotationRejected\":%lu,\"traceQueueDrops\":%lu,"
        "\"recordDrops\":%lu,\"traceDrops\":%lu,"
        "\"stopReason\":\"%s\",\"error\":\"%s\"}",
        recording_ ? "true" : "false", stopping_ ? "true" : "false",
        saving_ ? "true" : "false", available_ ? "true" : "false",
        complete_ ? "true" : "false", recovered_ ? "true" : "false",
        truncated_ ? "true" : "false", save_failed_ ? "true" : "false",
        CanTraceQueue::scopeName(scope_), durable_ ? "true" : "false",
        checkpoint_failed_ ? "true" : "false", static_cast<unsigned>(checkpoint_offset_),
        static_cast<unsigned long>(recovery_reset_reason_),
        recovery_reset_reason_ == 0U ? "" : resetReasonName(recovery_reset_reason_),
        static_cast<unsigned long>(duration_ms),
        static_cast<unsigned>(capture_size_), static_cast<unsigned>(session_capacity_),
        static_cast<unsigned>(saved_bytes_), static_cast<unsigned long>(frame_records_),
        static_cast<unsigned long>(annotation_records_),
        static_cast<unsigned long>(annotation_rejected_),
        static_cast<unsigned long>(trace_queue_drops_),
        static_cast<unsigned long>(record_drops_),
        static_cast<unsigned long>(trace_drops_), stop_reason_, last_error_);
    return count >= 0 && static_cast<size_t>(count) < capacity;
}

const char* SessionLogRecorder::resetReasonName(uint32_t reason) {
    switch (static_cast<esp_reset_reason_t>(reason)) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        case ESP_RST_UNKNOWN:
        default: return "unknown";
    }
}

}  // namespace bored::signalscope
