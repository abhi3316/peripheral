#include "peripheral/storage/cloud_uploader.hpp"
#include "peripheral/events/event_logger.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

namespace peripheral {

namespace {

uint64_t now_microseconds() noexcept {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

} // namespace

CloudUploader::CloudUploader(CloudUploaderConfig config)
    : config_(std::move(config)) {
}

CloudUploader::~CloudUploader() {
    stop();
}

void CloudUploader::start() {
    if (running_.exchange(true)) {
        return; // Already started
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        scan_recordings_locked();
    }

    worker_thread_ = std::thread(&CloudUploader::worker_loop, this);
}

void CloudUploader::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    socket_.close();
    is_storage_connected_ = false;
}

void CloudUploader::enqueue(std::string_view recording_id,
                            std::string_view object_key,
                            const std::filesystem::path& file_path,
                            bool is_manifest) {
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(file_path, ec);
    if (ec) {
        size = 0;
    }

    UploadTask task{
        .recording_id = std::string(recording_id),
        .object_key = std::string(object_key),
        .file_path = file_path,
        .file_size = size,
        .created_at_us = now_microseconds(),
        .attempts = 0,
        .is_manifest = is_manifest
    };

    std::lock_guard<std::mutex> lock(mutex_);
    // Avoid duplicate queueing if already uploaded or already queued
    if (uploaded_keys_.contains(task.object_key)) {
        return;
    }
    for (const auto& item : queue_) {
        if (item.object_key == task.object_key) {
            return;
        }
    }

    queue_.push_back(std::move(task));
    cv_.notify_one();
}

bool CloudUploader::is_uploaded(const std::filesystem::path& file_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::filesystem::path marker = file_path;
    marker += ".uploaded";
    if (std::filesystem::exists(marker)) {
        return true;
    }
    // Also check relative key in set
    std::string key = file_path.filename().string();
    return uploaded_keys_.contains(key);
}

CloudUploadTelemetry CloudUploader::get_telemetry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CloudUploadTelemetry t;
    t.storage = is_storage_connected_ ? "connected" : "disconnected";
    uint64_t last = last_contact_us_.load(std::memory_order_relaxed);
    if (last > 0) {
        t.last_contact_us = last;
    }
    t.backlog_objects = queue_.size();
    uint64_t bytes = 0;
    for (const auto& item : queue_) {
        bytes += item.file_size;
    }
    t.backlog_bytes = bytes;
    if (!queue_.empty()) {
        t.oldest_pending_us = queue_.front().created_at_us;
    }
    return t;
}

void CloudUploader::scan_recordings_locked() {
    std::error_code ec;
    if (!std::filesystem::exists(config_.record_dir, ec)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(config_.record_dir, ec)) {
        if (!entry.is_directory()) continue;
        std::string rec_id = entry.path().filename().string();
        if (!rec_id.starts_with("rec-")) continue;

        // Check each part-XXXX.bin and manifest.json
        std::vector<std::filesystem::path> parts;
        std::filesystem::path manifest_path;

        for (const auto& file_entry : std::filesystem::directory_iterator(entry.path(), ec)) {
            if (!file_entry.is_regular_file()) continue;
            auto fname = file_entry.path().filename().string();
            if (fname.ends_with(".uploaded")) {
                std::string base = fname.substr(0, fname.size() - 9);
                uploaded_keys_.insert(rec_id + "/" + base);
                continue;
            }
            if (fname.starts_with("part-") && fname.ends_with(".bin")) {
                parts.push_back(file_entry.path());
            } else if (fname == "manifest.json") {
                manifest_path = file_entry.path();
            }
        }

        // Sort parts sequentially
        std::sort(parts.begin(), parts.end());

        for (const auto& p : parts) {
            std::string key = rec_id + "/" + p.filename().string();
            if (!uploaded_keys_.contains(key)) {
                std::filesystem::path marker = p;
                marker += ".uploaded";
                if (std::filesystem::exists(marker)) {
                    uploaded_keys_.insert(key);
                } else {
                    uint64_t sz = std::filesystem::file_size(p, ec);
                    queue_.push_back(UploadTask{
                        .recording_id = rec_id,
                        .object_key = key,
                        .file_path = p,
                        .file_size = ec ? 0 : sz,
                        .created_at_us = now_microseconds(),
                        .attempts = 0,
                        .is_manifest = false
                    });
                }
            }
        }

        if (!manifest_path.empty()) {
            std::string key = rec_id + "/manifest.json";
            if (!uploaded_keys_.contains(key)) {
                std::filesystem::path marker = manifest_path;
                marker += ".uploaded";
                if (std::filesystem::exists(marker)) {
                    uploaded_keys_.insert(key);
                } else {
                    uint64_t sz = std::filesystem::file_size(manifest_path, ec);
                    queue_.push_back(UploadTask{
                        .recording_id = rec_id,
                        .object_key = key,
                        .file_path = manifest_path,
                        .file_size = ec ? 0 : sz,
                        .created_at_us = now_microseconds(),
                        .attempts = 0,
                        .is_manifest = true
                    });
                }
            }
        }
    }
}

void CloudUploader::worker_loop() {
    while (running_) {
        UploadTask current_task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !running_ || !queue_.empty();
            });

            if (!running_) break;
            current_task = queue_.front();
        }

        // Check if file exists
        if (!std::filesystem::exists(current_task.file_path)) {
            // File removed or invalid, pop it
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queue_.empty() && queue_.front().object_key == current_task.object_key) {
                queue_.pop_front();
            }
            continue;
        }

        // Compute streaming SHA-256
        std::string sha256 = Sha256::hash_file(current_task.file_path);
        if (sha256.empty()) {
            // Error reading file, delay and retry
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Attempt HTTP PUT
        bool ok = execute_http_put(current_task, sha256);

        if (ok) {
            // Success! Pop from queue and mark uploaded
            {
                std::lock_guard<std::mutex> lock(mutex_);
                uploaded_keys_.insert(current_task.object_key);
                if (!queue_.empty() && queue_.front().object_key == current_task.object_key) {
                    queue_.pop_front();
                }
            }

            // Write .uploaded marker file on disk for restart resilience
            std::filesystem::path marker = current_task.file_path;
            marker += ".uploaded";
            std::ofstream marker_file(marker);
            if (marker_file.is_open()) {
                marker_file << sha256 << "\n";
            }

            EventLogger::instance().info("system", "chunk_uploaded", "Confirmed upload of " + current_task.object_key + " to cloud storage");

            // Notify callback
            if (on_completed_cb_) {
                on_completed_cb_(current_task.recording_id, current_task.object_key, current_task.is_manifest);
            }
        } else {
            // Failure: calculate exponential backoff delay
            current_task.attempts++;
            uint64_t delay_ms = config_.min_retry_delay.count() * (1ULL << std::min(current_task.attempts, 6));
            delay_ms = std::min(delay_ms, static_cast<uint64_t>(config_.max_retry_delay.count()));

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!queue_.empty() && queue_.front().object_key == current_task.object_key) {
                    queue_.front().attempts = current_task.attempts;
                }
            }

            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(delay_ms), [this] {
                return !running_;
            });
        }
    }
}

bool CloudUploader::ensure_connected() {
    if (socket_.is_connected()) {
        return true;
    }
    bool connected = socket_.connect_to(config_.host, config_.port);
    is_storage_connected_ = connected;
    return connected;
}

bool CloudUploader::execute_http_put(const UploadTask& task, const std::string& sha256_hex) {
    if (!ensure_connected()) {
        return false;
    }

    std::error_code ec;
    uint64_t file_size = std::filesystem::file_size(task.file_path, ec);
    if (ec) {
        return false;
    }

    // Build HTTP request header
    std::string request_headers =
        "PUT /v1/servers/" + config_.server_id + "/objects/" + task.object_key + " HTTP/1.1\r\n" +
        "Host: " + config_.host + ":" + std::to_string(config_.port) + "\r\n" +
        "Authorization: Bearer " + config_.auth_token + "\r\n" +
        "Content-Length: " + std::to_string(file_size) + "\r\n" +
        "X-Content-SHA256: " + sha256_hex + "\r\n" +
        "Connection: keep-alive\r\n\r\n";

    // Send headers
    auto status = socket_.send_all(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(request_headers.data()), request_headers.size()),
        config_.put_timeout
    );
    if (status != SocketStatus::Ok) {
        socket_.close();
        is_storage_connected_ = false;
        return false;
    }

    // Stream file contents in 64 KB chunks
    std::ifstream file(task.file_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    std::array<uint8_t, 65536> buffer{};
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        std::streamsize bytes_read = file.gcount();
        if (bytes_read > 0) {
            status = socket_.send_all(
                std::span<const uint8_t>(buffer.data(), static_cast<size_t>(bytes_read)),
                config_.put_timeout
            );
            if (status != SocketStatus::Ok) {
                socket_.close();
                is_storage_connected_ = false;
                return false;
            }
        }
    }

    // Read HTTP response headers
    std::string response;
    response.reserve(1024);
    std::array<uint8_t, 512> recv_buf{};
    size_t header_end = std::string::npos;

    while (header_end == std::string::npos) {
        size_t n = 0;
        status = socket_.recv_some(recv_buf, n, config_.put_timeout);
        if (status != SocketStatus::Ok || n == 0) {
            socket_.close();
            is_storage_connected_ = false;
            return false;
        }
        response.append(reinterpret_cast<char*>(recv_buf.data()), n);
        header_end = response.find("\r\n\r\n");
        if (response.size() > 8192) {
            socket_.close();
            is_storage_connected_ = false;
            return false;
        }
    }

    // Parse status code from HTTP/1.1 <status> ...
    int http_status = 0;
    if (response.starts_with("HTTP/1.")) {
        size_t first_space = response.find(' ');
        if (first_space != std::string::npos && first_space + 4 <= response.size()) {
            std::from_chars(response.data() + first_space + 1, response.data() + first_space + 4, http_status);
        }
    }

    // Check Content-Length to drain any response body
    size_t cl_pos = response.find("Content-Length: ");
    if (cl_pos == std::string::npos) {
        cl_pos = response.find("content-length: ");
    }
    size_t content_length = 0;
    if (cl_pos != std::string::npos) {
        size_t num_start = cl_pos + 16;
        size_t num_end = response.find("\r\n", num_start);
        if (num_end != std::string::npos) {
            std::from_chars(response.data() + num_start, response.data() + num_end, content_length);
        }
    }

    size_t body_already_received = response.size() - (header_end + 4);
    if (content_length > body_already_received) {
        size_t remaining = content_length - body_already_received;
        while (remaining > 0) {
            size_t n = 0;
            size_t chunk_to_read = std::min(remaining, recv_buf.size());
            status = socket_.recv_some(std::span<uint8_t>(recv_buf.data(), chunk_to_read), n, config_.put_timeout);
            if (status != SocketStatus::Ok || n == 0) {
                socket_.close();
                is_storage_connected_ = false;
                break;
            }
            remaining -= n;
        }
    }

    // Check if server requested Connection: close
    if (response.find("Connection: close") != std::string::npos || response.find("connection: close") != std::string::npos) {
        socket_.close();
    }

    // Evaluate result
    if (http_status == 201 || http_status == 200) {
        last_contact_us_.store(now_microseconds(), std::memory_order_relaxed);
        is_storage_connected_ = true;
        return true;
    }

    if (http_status == 409) {
        // Conflict: object already stored with different content; cloud storage keeps first version
        last_contact_us_.store(now_microseconds(), std::memory_order_relaxed);
        is_storage_connected_ = true;
        return true;
    }

    if (http_status == 503) {
        // Service unavailable / busy
        last_contact_us_.store(now_microseconds(), std::memory_order_relaxed);
        is_storage_connected_ = true;
        return false; // will retry with backoff
    }

    if (http_status == 422) {
        // Digest mismatch
        last_contact_us_.store(now_microseconds(), std::memory_order_relaxed);
        is_storage_connected_ = true;
        return false;
    }

    // Other errors (e.g. 401 unauthorized or unknown)
    last_contact_us_.store(now_microseconds(), std::memory_order_relaxed);
    return false;
}

bool CloudUploader::upload_file_sync(const UploadTask& task) {
    if (!std::filesystem::exists(task.file_path)) {
        return false;
    }
    std::string sha256 = Sha256::hash_file(task.file_path);
    if (sha256.empty()) {
        return false;
    }
    return execute_http_put(task, sha256);
}

} // namespace peripheral
