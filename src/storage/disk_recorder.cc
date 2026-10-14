#include "peripheral/storage/disk_recorder.hpp"
#include "peripheral/storage/cloud_uploader.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace peripheral {

namespace {

uint64_t get_now_monotonic_us() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string get_iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&in_time_t));
    return std::string(buf);
}

std::string gap_to_json(const GapRecord& g) {
    std::ostringstream oss;
    oss << "{\"cameraId\":\"" << g.camera_id << "\","
        << "\"bootId\":" << g.boot_id << ","
        << "\"fromSeq\":" << g.from_seq << ","
        << "\"toSeq\":" << g.to_seq << ","
        << "\"reason\":\"" << g.reason << "\"}";
    return oss.str();
}

std::string recording_to_json(const RecordingSummary& r) {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"recordingId\": \"" << r.recording_id << "\",\n"
        << "  \"state\": \"" << r.state << "\",\n"
        << "  \"startedAtUs\": " << r.started_at_us << ",\n"
        << "  \"startedAtWall\": \"" << r.started_at_wall << "\",\n";
    if (r.stopped_at_us > 0) {
        oss << "  \"stoppedAtUs\": " << r.stopped_at_us << ",\n";
    } else {
        oss << "  \"stoppedAtUs\": null,\n";
    }
    oss << "  \"frames\": " << r.frames << ",\n"
        << "  \"bytes\": " << r.bytes << ",\n"
        << "  \"complete\": " << (r.complete ? "true" : "false") << ",\n"
        << "  \"upload\": \"" << r.upload << "\",\n"
        << "  \"gaps\": [";
    for (size_t i = 0; i < r.gaps.size(); ++i) {
        if (i > 0) oss << ",";
        oss << gap_to_json(r.gaps[i]);
    }
    oss << "]\n}";
    return oss.str();
}

} // namespace

DiskRecorder::DiskRecorder(DiskRecorderConfig config)
    : config_(std::move(config)) {
    try {
        std::filesystem::create_directories(config_.root_dir);
    } catch (...) {}
    scan_existing_recordings();
}

DiskRecorder::~DiskRecorder() {
    stop_recording();
}

bool DiskRecorder::start_recording(std::string_view recording_id) {
    std::scoped_lock lock(mutex_);
    if (is_recording_) {
        return true;
    }

    std::string id;
    if (!recording_id.empty()) {
        id = std::string(recording_id);
    } else {
        uint32_t next_num = static_cast<uint32_t>(completed_recordings_.size() + 1);
        std::ostringstream oss;
        oss << "rec-" << std::setw(3) << std::setfill('0') << next_num;
        id = oss.str();
    }

    current_rec_ = RecordingSummary{};
    current_rec_.recording_id = id;
    current_rec_.state = "recording";
    current_rec_.started_at_us = get_now_monotonic_us();
    current_rec_.started_at_wall = get_iso_timestamp();
    current_rec_.frames = 0;
    current_rec_.bytes = 0;
    current_rec_.upload = "pending";
    current_rec_.complete = true;

    cam_tracks_.clear();
    current_part_seq_ = 0;

    current_rec_dir_ = config_.root_dir / id;
    try {
        std::filesystem::create_directories(current_rec_dir_);
    } catch (...) {
        return false;
    }

    open_next_chunk_locked();
    is_recording_ = true;
    write_manifest_locked();

    std::cout << "[DiskRecorder] Started recording " << id << " in " << current_rec_dir_ << "\n";
    return true;
}

bool DiskRecorder::stop_recording() {
    std::scoped_lock lock(mutex_);
    if (!is_recording_) {
        return false;
    }

    close_current_chunk_locked();
    current_rec_.state = "stopped";
    current_rec_.stopped_at_us = get_now_monotonic_us();
    current_rec_.complete = true;

    write_manifest_locked();

    if (uploader_ && !current_rec_dir_.empty()) {
        uploader_->enqueue(current_rec_.recording_id,
                           current_rec_.recording_id + "/manifest.json",
                           current_rec_dir_ / "manifest.json",
                           true);
    }

    completed_recordings_.push_back(current_rec_);
    is_recording_ = false;

    std::cout << "[DiskRecorder] Stopped recording " << current_rec_.recording_id
              << ": " << current_rec_.frames << " frames (" << current_rec_.bytes << " bytes)\n";
    return true;
}

bool DiskRecorder::is_recording() const {
    std::scoped_lock lock(mutex_);
    return is_recording_;
}

void DiskRecorder::on_frame(std::string_view camera_id, const VideoFrame& frame) {
    std::scoped_lock lock(mutex_);
    if (!is_recording_ || !chunk_file_.is_open()) {
        return;
    }

    std::string cam_str(camera_id);
    auto& track = cam_tracks_[cam_str];

    // Gap detection
    if (track.has_seen_frame && track.last_boot_id == frame.header.boot_id) {
        if (frame.header.frame_seq > track.last_seq + 1) {
            GapRecord gap;
            gap.camera_id = cam_str;
            gap.boot_id = frame.header.boot_id;
            gap.from_seq = track.last_seq + 1;
            gap.to_seq = frame.header.frame_seq - 1;
            gap.reason = track.is_connected ? "dropped_by_camera" : "link_down";
            current_rec_.gaps.push_back(gap);
        }
    }

    if (frame.fate == FrameFate::Corrupt) {
        GapRecord gap;
        gap.camera_id = cam_str;
        gap.boot_id = frame.header.boot_id;
        gap.from_seq = frame.header.frame_seq;
        gap.to_seq = frame.header.frame_seq;
        gap.reason = "damaged";
        current_rec_.gaps.push_back(gap);
    }

    track.last_seq = frame.header.frame_seq;
    track.last_boot_id = frame.header.boot_id;
    track.has_seen_frame = true;
    track.is_connected = true;

    size_t frame_bytes = sizeof(VideoFrameHeader) + frame.pixels().size();

    // Check emergency disk quota (95% / 1.9 GB) against total disk used across record/
    uint64_t current_disk_used = total_disk_used_bytes_locked();
    if (current_disk_used + frame_bytes >= config_.emergency_quota_bytes) {
        reclaim_disk_space_locked();
        current_disk_used = total_disk_used_bytes_locked();
    }

    if (current_disk_used + frame_bytes >= config_.emergency_quota_bytes) {
        GapRecord gap;
        gap.camera_id = cam_str;
        gap.boot_id = frame.header.boot_id;
        gap.from_seq = frame.header.frame_seq;
        gap.to_seq = frame.header.frame_seq;
        gap.reason = "disk_full";
        current_rec_.gaps.push_back(gap);
        return;
    }

    // Chunk size threshold check
    if (chunk_bytes_written_ >= config_.chunk_max_bytes) {
        open_next_chunk_locked();
    }

    // Write wire header and payload
    chunk_file_.write(reinterpret_cast<const char*>(&frame.header), sizeof(VideoFrameHeader));
    auto pixels = frame.pixels();
    if (!pixels.empty()) {
        chunk_file_.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    }

    chunk_bytes_written_ += frame_bytes;
    current_rec_.bytes += frame_bytes;
    current_rec_.frames++;

    // Throughput window accounting
    uint64_t now_us = get_now_monotonic_us();
    if (window_start_us_ == 0) {
        window_start_us_ = now_us;
    }
    window_bytes_ += frame_bytes;
    if (now_us > window_start_us_ + 500000) { // update every 0.5 s
        double dt_sec = (now_us - window_start_us_) / 1e6;
        cached_write_rate_ = (dt_sec > 0) ? (window_bytes_ / dt_sec) : 0.0;
        window_bytes_ = 0;
        window_start_us_ = now_us;
    }
}

void DiskRecorder::on_connection_state(std::string_view camera_id, bool connected) {
    std::scoped_lock lock(mutex_);
    cam_tracks_[std::string(camera_id)].is_connected = connected;
}

std::optional<RecordingSummary> DiskRecorder::active_recording() const {
    std::scoped_lock lock(mutex_);
    if (!is_recording_) {
        return std::nullopt;
    }
    auto rec = current_rec_;
    rec.upload = determine_upload_status(config_.root_dir / rec.recording_id);
    return rec;
}

std::vector<RecordingSummary> DiskRecorder::list_recordings() const {
    std::scoped_lock lock(mutex_);
    std::vector<RecordingSummary> list = completed_recordings_;
    for (auto& r : list) {
        r.upload = determine_upload_status(config_.root_dir / r.recording_id);
    }
    if (is_recording_) {
        auto rec = current_rec_;
        rec.upload = determine_upload_status(config_.root_dir / rec.recording_id);
        list.push_back(rec);
    }
    return list;
}

std::string DiskRecorder::list_recordings_json() const {
    std::vector<RecordingSummary> list = list_recordings();

    std::ostringstream oss;
    oss << "[\n";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) oss << ",\n";
        oss << recording_to_json(list[i]);
    }
    oss << "\n]";
    return oss.str();
}

std::string DiskRecorder::determine_upload_status(const std::filesystem::path& rec_dir) const {
    std::error_code ec;
    if (!std::filesystem::exists(rec_dir, ec)) return "pending";

    bool has_manifest_uploaded = std::filesystem::exists(rec_dir / "manifest.json.uploaded", ec);
    std::set<std::string> parts_seen;
    std::set<std::string> parts_uploaded;

    for (const auto& entry : std::filesystem::directory_iterator(rec_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto name = entry.path().filename().string();
        if (name.starts_with("part-") && name.ends_with(".bin")) {
            parts_seen.insert(name);
            std::filesystem::path marker = entry.path();
            marker += ".uploaded";
            if (std::filesystem::exists(marker, ec)) {
                parts_uploaded.insert(name);
            }
        } else if (name.starts_with("part-") && name.ends_with(".bin.uploaded")) {
            std::string base = name.substr(0, name.size() - 9);
            parts_seen.insert(base);
            parts_uploaded.insert(base);
        }
    }

    if (has_manifest_uploaded && !parts_seen.empty() && parts_uploaded.size() == parts_seen.size()) {
        return "uploaded";
    }
    if (!parts_uploaded.empty() || has_manifest_uploaded) {
        return "partial";
    }
    return "pending";
}

uint64_t DiskRecorder::total_disk_used_bytes() const {
    std::scoped_lock lock(mutex_);
    return total_disk_used_bytes_locked();
}

uint64_t DiskRecorder::total_disk_used_bytes_locked() const {
    uint64_t total = 0;
    std::error_code ec;
    try {
        if (std::filesystem::exists(config_.root_dir, ec)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(config_.root_dir, ec)) {
                if (entry.is_regular_file(ec)) {
                    total += entry.file_size(ec);
                }
            }
        }
    } catch (...) {}
    return total;
}

uint64_t DiskRecorder::reclaim_disk_space() {
    std::scoped_lock lock(mutex_);
    return reclaim_disk_space_locked();
}

uint64_t DiskRecorder::reclaim_disk_space_locked() {
    uint64_t current_used = total_disk_used_bytes_locked();
    if (current_used < config_.reclamation_threshold_bytes) {
        return 0;
    }

    std::error_code ec;
    if (!std::filesystem::exists(config_.root_dir, ec)) {
        return 0;
    }

    // 1. Collect all recording session directories
    std::vector<std::filesystem::path> rec_dirs;
    for (const auto& entry : std::filesystem::directory_iterator(config_.root_dir, ec)) {
        if (entry.is_directory(ec)) {
            rec_dirs.push_back(entry.path());
        }
    }

    // 2. Sort chronologically so oldest sessions are pruned first
    std::sort(rec_dirs.begin(), rec_dirs.end());

    uint64_t bytes_reclaimed = 0;

    for (const auto& rdir : rec_dirs) {
        bool is_active_dir = (is_recording_ && rdir == current_rec_dir_);

        for (const auto& f_entry : std::filesystem::directory_iterator(rdir, ec)) {
            if (!f_entry.is_regular_file(ec)) continue;
            auto fname = f_entry.path().filename().string();

            if (fname.starts_with("part-") && fname.ends_with(".bin")) {
                // Never delete the chunk actively being written to
                if (is_active_dir && fname == current_chunk_filename_) {
                    continue;
                }

                // Check for confirmed .uploaded receipt marker
                std::filesystem::path marker = f_entry.path();
                marker += ".uploaded";

                if (std::filesystem::exists(marker, ec)) {
                    uint64_t fsz = f_entry.file_size(ec);
                    if (!ec && std::filesystem::remove(f_entry.path(), ec)) {
                        bytes_reclaimed += fsz;
                        current_used = (current_used >= fsz) ? (current_used - fsz) : 0;
                        if (current_used < config_.reclamation_threshold_bytes) {
                            break;
                        }
                    }
                }
            }
        }

        if (current_used < config_.reclamation_threshold_bytes) {
            break;
        }
    }

    total_bytes_reclaimed_ += bytes_reclaimed;
    if (bytes_reclaimed > 0) {
        std::cout << "[DiskRecorder] Reclaimed " << bytes_reclaimed << " bytes from uploaded chunks. "
                  << "Total disk used now: " << current_used << " bytes.\n";
    }
    return bytes_reclaimed;
}

double DiskRecorder::write_bytes_per_second() const {
    std::scoped_lock lock(mutex_);
    if (!is_recording_) return 0.0;
    return cached_write_rate_;
}

void DiskRecorder::open_next_chunk_locked() {
    close_current_chunk_locked();

    // Trigger automatic disk reclamation on chunk rotation
    reclaim_disk_space_locked();

    current_part_seq_++;

    std::ostringstream oss;
    oss << "part-" << std::setw(4) << std::setfill('0') << current_part_seq_ << ".bin";
    current_chunk_filename_ = oss.str();
    std::filesystem::path chunk_path = current_rec_dir_ / current_chunk_filename_;

    chunk_file_.open(chunk_path, std::ios::binary | std::ios::trunc);
    chunk_bytes_written_ = 0;
}

void DiskRecorder::close_current_chunk_locked() {
    if (chunk_file_.is_open()) {
        chunk_file_.flush();
        chunk_file_.close();
        if (uploader_ && !current_chunk_filename_.empty() && !current_rec_dir_.empty()) {
            uploader_->enqueue(current_rec_.recording_id,
                               current_rec_.recording_id + "/" + current_chunk_filename_,
                               current_rec_dir_ / current_chunk_filename_,
                               false);
        }
    }
    chunk_bytes_written_ = 0;
}

void DiskRecorder::write_manifest_locked() {
    if (current_rec_dir_.empty()) return;
    std::filesystem::path manifest_path = current_rec_dir_ / "manifest.json";
    std::ofstream mf(manifest_path, std::ios::trunc);
    if (mf.is_open()) {
        mf << recording_to_json(current_rec_);
    }
}

void DiskRecorder::scan_existing_recordings() {
    std::scoped_lock lock(mutex_);
    completed_recordings_.clear();
    try {
        if (!std::filesystem::exists(config_.root_dir)) return;
        for (const auto& dir_entry : std::filesystem::directory_iterator(config_.root_dir)) {
            if (dir_entry.is_directory()) {
                auto mf = dir_entry.path() / "manifest.json";
                if (std::filesystem::exists(mf)) {
                    std::ifstream f(mf);
                    if (f) {
                        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                        // Parse basic recording info
                        RecordingSummary s{};
                        s.recording_id = dir_entry.path().filename().string();
                        s.state = "stopped";
                        s.complete = true;
                        s.upload = "pending";

                        auto pos = content.find("\"frames\":");
                        if (pos != std::string::npos) s.frames = std::stoull(content.substr(pos + 9));
                        pos = content.find("\"bytes\":");
                        if (pos != std::string::npos) s.bytes = std::stoull(content.substr(pos + 8));
                        pos = content.find("\"startedAtWall\": \"");
                        if (pos != std::string::npos) {
                            auto end = content.find('"', pos + 18);
                            if (end != std::string::npos) s.started_at_wall = content.substr(pos + 18, end - (pos + 18));
                        }
                        pos = content.find("\"startedAtUs\":");
                        if (pos != std::string::npos) s.started_at_us = std::stoull(content.substr(pos + 14));
                        completed_recordings_.push_back(s);
                    }
                }
            }
        }
    } catch (...) {}
}

} // namespace peripheral
