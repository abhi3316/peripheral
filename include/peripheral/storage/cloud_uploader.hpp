#pragma once

#include "peripheral/crypto/sha256.hpp"
#include "peripheral/transport/socket.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace peripheral {

struct CloudUploadTelemetry {
    std::string storage{"disconnected"}; // "connected" | "disconnected" | "unknown"
    std::optional<uint64_t> last_contact_us{std::nullopt};
    uint64_t backlog_objects{0};
    uint64_t backlog_bytes{0};
    std::optional<uint64_t> oldest_pending_us{std::nullopt};
};

struct CloudUploaderConfig {
    std::string host{"127.0.0.1"};
    uint16_t port{7100};
    std::string server_id{"peripheral-1"};
    std::string auth_token{"dev-token"};
    std::filesystem::path record_dir{"record"};
    std::chrono::milliseconds connect_timeout{2000};
    std::chrono::milliseconds put_timeout{10000};
    std::chrono::milliseconds min_retry_delay{100};
    std::chrono::milliseconds max_retry_delay{3000};
};

struct UploadTask {
    std::string recording_id;
    std::string object_key; // e.g. "rec-1/part-0001.bin"
    std::filesystem::path file_path;
    uint64_t file_size{0};
    uint64_t created_at_us{0};
    int attempts{0};
    bool is_manifest{false};
};

/**
 * @brief Background worker streaming completed chunk files to Cloud Storage on port 7100.
 *
 * Implements:
 * - Thread-safe FIFO upload queue
 * - Streaming SHA-256 validation
 * - HTTP PUT client with keep-alive
 * - Exponential backoff and retry on 503 or link disconnect
 * - Startup crash recovery scanning un-uploaded items
 * - Telemetry reporting for the operator viewer
 */
class CloudUploader {
public:
    using OnUploadCompletedCallback = std::function<void(const std::string& rec_id, const std::string& key, bool is_manifest)>;

    explicit CloudUploader(CloudUploaderConfig config = {});
    ~CloudUploader();

    CloudUploader(const CloudUploader&) = delete;
    CloudUploader& operator=(const CloudUploader&) = delete;

    /// Start the background uploader thread and scan existing recordings
    void start();

    /// Stop the background uploader
    void stop();

    /// Enqueue a newly finalized chunk or manifest for upload
    void enqueue(std::string_view recording_id,
                 std::string_view object_key,
                 const std::filesystem::path& file_path,
                 bool is_manifest = false);

    /// Check if a specific file has been successfully uploaded
    [[nodiscard]] bool is_uploaded(const std::filesystem::path& file_path) const;

    /// Get current live telemetry snapshot for the viewer /api/state
    [[nodiscard]] CloudUploadTelemetry get_telemetry() const;

    /// Register callback invoked when an object upload completes successfully
    void set_on_completed_callback(OnUploadCompletedCallback cb) {
        on_completed_cb_ = std::move(cb);
    }

    /// Helper to test or execute a single upload synchronously (useful in tests)
    bool upload_file_sync(const UploadTask& task);

private:
    void worker_loop();
    void scan_recordings_locked();
    bool execute_http_put(const UploadTask& task, const std::string& sha256_hex);
    bool ensure_connected();

    CloudUploaderConfig config_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<UploadTask> queue_;
    std::set<std::string> uploaded_keys_;

    // Telemetry state
    std::atomic<bool> is_storage_connected_{false};
    std::atomic<uint64_t> last_contact_us_{0};

    TcpSocket socket_;
    OnUploadCompletedCallback on_completed_cb_{nullptr};
};

} // namespace peripheral
