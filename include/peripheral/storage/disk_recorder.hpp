#pragma once

#include "peripheral/protocol/video_frame.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace peripheral {

class CloudUploader;

struct GapRecord {
    std::string camera_id;
    uint32_t boot_id{0};
    uint32_t from_seq{0};
    uint32_t to_seq{0};
    std::string reason; // "dropped_by_camera", "damaged", "link_down", "dropped_in_server", "disk_full"
};

struct RecordingSummary {
    std::string recording_id;
    std::string state{"stopped"}; // "recording" or "stopped"
    uint64_t started_at_us{0};
    std::string started_at_wall;
    uint64_t stopped_at_us{0};
    uint64_t frames{0};
    uint64_t bytes{0};
    std::vector<GapRecord> gaps;
    bool complete{true};
    std::string upload{"pending"}; // "pending", "partial", "uploaded"
};

struct DiskRecorderConfig {
    std::filesystem::path root_dir{"record"};
    uint64_t chunk_max_bytes{10 * 1024 * 1024};          // ~10 MB per chunk
    uint64_t disk_quota_bytes{2000000000ULL};             // 2 GB quota
    uint64_t reclamation_threshold_bytes{1600000000ULL};  // 1.6 GB (80% of quota)
    uint64_t emergency_quota_bytes{1900000000ULL};        // 1.9 GB (95% of quota)
};

class DiskRecorder {
public:
    explicit DiskRecorder(DiskRecorderConfig config = {});
    ~DiskRecorder();

    DiskRecorder(const DiskRecorder&) = delete;
    DiskRecorder& operator=(const DiskRecorder&) = delete;

    /// Start a synchronized dual-camera recording
    bool start_recording(std::string_view recording_id = "");

    /// Stop the active recording session
    bool stop_recording();

    [[nodiscard]] bool is_recording() const;

    /// Ingest a frame from either camera into the active recording
    void on_frame(std::string_view camera_id, const VideoFrame& frame);

    /// Record connection drop / reconnect for gap tracking
    void on_connection_state(std::string_view camera_id, bool connected);

    /// Snapshot of the currently active recording (nullopt if stopped)
    [[nodiscard]] std::optional<RecordingSummary> active_recording() const;

    /// List of all recordings (active and historical)
    [[nodiscard]] std::vector<RecordingSummary> list_recordings() const;

    /// Serialized JSON string for /api/recordings
    [[nodiscard]] std::string list_recordings_json() const;

    /// Associate cloud uploader for automatic background streaming of chunks
    void set_cloud_uploader(CloudUploader* uploader) noexcept {
        uploader_ = uploader;
    }

    /// Total bytes used by all recordings in the root directory
    [[nodiscard]] uint64_t total_disk_used_bytes() const;

    /// Current measured disk write rate in bytes/sec
    [[nodiscard]] double write_bytes_per_second() const;

    /// Configured disk quota in bytes
    [[nodiscard]] uint64_t disk_quota_bytes() const noexcept {
        return config_.disk_quota_bytes;
    }

    /// Reclaim disk space by evicting uploaded chunks from historical recording sessions
    /// when total usage exceeds reclamation_threshold_bytes (80% / 1.6 GB).
    /// Returns total bytes freed.
    uint64_t reclaim_disk_space();

    /// Cumulative bytes reclaimed across all evictions
    [[nodiscard]] uint64_t total_bytes_reclaimed() const noexcept {
        return total_bytes_reclaimed_;
    }

    /// Threshold at which reclamation starts (e.g. 1.6 GB)
    [[nodiscard]] uint64_t reclamation_threshold_bytes() const noexcept {
        return config_.reclamation_threshold_bytes;
    }

    /// Emergency threshold at which frames are dropped to prevent disk corruption (e.g. 1.9 GB)
    [[nodiscard]] uint64_t emergency_quota_bytes() const noexcept {
        return config_.emergency_quota_bytes;
    }

private:
    void open_next_chunk_locked();
    void close_current_chunk_locked();
    void write_manifest_locked();
    void scan_existing_recordings();
    std::string determine_upload_status(const std::filesystem::path& rec_dir) const;
    uint64_t reclaim_disk_space_locked();
    uint64_t total_disk_used_bytes_locked() const;

    DiskRecorderConfig config_;
    mutable std::mutex mutex_;
    CloudUploader* uploader_{nullptr};
    uint64_t total_bytes_reclaimed_{0};

    bool is_recording_{false};
    RecordingSummary current_rec_{};
    uint32_t current_part_seq_{0};
    std::string current_chunk_filename_{};
    std::filesystem::path current_rec_dir_{};
    std::ofstream chunk_file_{};
    uint64_t chunk_bytes_written_{0};

    // Camera sequence tracking for gap detection
    struct CamTrack {
        uint32_t last_boot_id{0};
        uint32_t last_seq{0};
        bool has_seen_frame{false};
        bool is_connected{true};
    };
    std::map<std::string, CamTrack> cam_tracks_;

    std::vector<RecordingSummary> completed_recordings_;

    // Write speed measurement
    mutable uint64_t window_bytes_{0};
    mutable uint64_t window_start_us_{0};
    mutable double cached_write_rate_{0.0};
};

} // namespace peripheral
