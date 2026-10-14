#include "peripheral/camera/pose_interpolator.hpp"
#include "peripheral/camera/time_sync.hpp"
#include "peripheral/protocol/control_framer.hpp"
#include "peripheral/protocol/protocol.hpp"
#include "peripheral/protocol/video_framer.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace peripheral;

namespace {

// Locate capture dataset directory
fs::path find_capture_dir() {
    std::vector<fs::path> candidates = {
        "utils/data/capture",
        "../utils/data/capture",
        "../../utils/data/capture"
    };
    for (const auto& p : candidates) {
        if (fs::exists(p / "truth.csv")) {
            return p;
        }
    }
    std::cerr << "Fatal: Could not locate utils/data/capture directory!\n";
    std::exit(1);
}

// Data structures matching dataset CSV files
struct ReadRecord {
    enum class Type { Video, Control } type;
    int connection{0};
    size_t offset{0};
    size_t bytes{0};
    uint64_t rx_us{0};
};

struct TruthRecord {
    uint16_t boot_id{0};
    uint32_t frame_seq{0};
    std::string fate;
    uint64_t exposure_start_us{0};
    uint64_t exposure_end_us{0};
    double pan_deg{0.0};
    double tilt_deg{0.0};
    uint32_t payload_crc32{0};
};

struct EventRecord {
    uint64_t server_us{0};
    std::string event;
};

std::vector<ReadRecord> load_reads_csv(const fs::path& path, ReadRecord::Type type) {
    std::ifstream file(path);
    assert(file.is_open() && "Failed to open reads CSV");
    std::string line;
    std::getline(file, line); // Header: connection,offset,bytes,rx_us

    std::vector<ReadRecord> records;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string s_conn, s_off, s_bytes, s_rx;
        if (std::getline(ss, s_conn, ',') &&
            std::getline(ss, s_off, ',') &&
            std::getline(ss, s_bytes, ',') &&
            std::getline(ss, s_rx, ',')) {
            records.push_back(ReadRecord{
                .type = type,
                .connection = std::stoi(s_conn),
                .offset = static_cast<size_t>(std::stoull(s_off)),
                .bytes = static_cast<size_t>(std::stoull(s_bytes)),
                .rx_us = std::stoull(s_rx)
            });
        }
    }
    return records;
}

std::map<std::pair<uint16_t, uint32_t>, TruthRecord> load_truth_csv(const fs::path& path) {
    std::ifstream file(path);
    assert(file.is_open() && "Failed to open truth CSV");
    std::string line;
    std::getline(file, line); // Header: boot_id,frame_seq,fate,exposure_start_us,exposure_end_us,pan_deg,tilt_deg,payload_crc32

    std::map<std::pair<uint16_t, uint32_t>, TruthRecord> truth;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string s_boot, s_seq, s_fate, s_start, s_end, s_pan, s_tilt, s_crc;
        if (std::getline(ss, s_boot, ',') &&
            std::getline(ss, s_seq, ',') &&
            std::getline(ss, s_fate, ',') &&
            std::getline(ss, s_start, ',') &&
            std::getline(ss, s_end, ',') &&
            std::getline(ss, s_pan, ',') &&
            std::getline(ss, s_tilt, ',') &&
            std::getline(ss, s_crc, ',')) {
            uint16_t boot = static_cast<uint16_t>(std::stoi(s_boot));
            uint32_t seq = static_cast<uint32_t>(std::stoul(s_seq));
            truth[{boot, seq}] = TruthRecord{
                .boot_id = boot,
                .frame_seq = seq,
                .fate = s_fate,
                .exposure_start_us = std::stoull(s_start),
                .exposure_end_us = std::stoull(s_end),
                .pan_deg = std::stod(s_pan),
                .tilt_deg = std::stod(s_tilt),
                .payload_crc32 = static_cast<uint32_t>(std::stoul(s_crc))
            };
        }
    }
    return truth;
}

std::vector<EventRecord> load_events_csv(const fs::path& path) {
    std::ifstream file(path);
    assert(file.is_open() && "Failed to open events CSV");
    std::string line;
    std::getline(file, line); // Header: server_us,event

    std::vector<EventRecord> events;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        size_t comma = line.find(',');
        if (comma != std::string::npos) {
            uint64_t t = std::stoull(line.substr(0, comma));
            std::string desc = line.substr(comma + 1);
            if (desc.size() >= 2 && desc.front() == '"' && desc.back() == '"') {
                desc = desc.substr(1, desc.size() - 2);
            }
            events.push_back(EventRecord{.server_us = t, .event = desc});
        }
    }
    return events;
}

std::vector<uint8_t> read_binary_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file.is_open() && "Failed to open binary file");
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
}

// Offline Replay Engine & Observer State
struct ReplayResults {
    std::map<std::pair<uint16_t, uint32_t>, std::string> classified_fates;
    std::vector<double> clock_errors_us;
    std::vector<double> pan_errors_deg;
    std::vector<double> tilt_errors_deg;
    size_t truth_stamp_matches{0};
    size_t corrupt_frames_detected{0};
    size_t garbage_bytes_recovered{0};
    bool clock_wrap_observed{false};
    bool wire_drop_observed{false};
    bool reboot_observed{false};
    bool homing_ack_observed{false};
};

ReplayResults run_offline_capture_replay(const fs::path& capture_dir) {
    std::cout << "Loading offline capture files from: " << capture_dir << "...\n";
    auto video_bin = read_binary_file(capture_dir / "video.bin");
    auto control_bin = read_binary_file(capture_dir / "control.bin");
    auto video_reads = load_reads_csv(capture_dir / "video-reads.csv", ReadRecord::Type::Video);
    auto control_reads = load_reads_csv(capture_dir / "control-reads.csv", ReadRecord::Type::Control);
    auto truth = load_truth_csv(capture_dir / "truth.csv");

    // Merge and chronologically sort reads across both streams
    std::vector<ReadRecord> all_reads;
    all_reads.reserve(video_reads.size() + control_reads.size());
    all_reads.insert(all_reads.end(), video_reads.begin(), video_reads.end());
    all_reads.insert(all_reads.end(), control_reads.begin(), control_reads.end());
    std::stable_sort(all_reads.begin(), all_reads.end(), [](const ReadRecord& a, const ReadRecord& b) {
        return a.rx_us < b.rx_us;
    });

    std::cout << "  Merged and sorted " << all_reads.size() << " network read events.\n";

    // Initialize Framers & Filters
    VideoFramer video_framer;
    ControlFramer control_framer;
    TimeSyncFilter time_filter;
    PoseInterpolator pose_interpolator;

    uint16_t current_boot_id = 3; // Initial boot ID
    uint32_t exposure_us = 10000;
    uint64_t current_ctrl_rx_us = 0;

    ReplayResults results;

    control_framer.set_hello_callback([&](const HelloPayload& hello) {
        if (hello.boot_id != current_boot_id) {
            results.reboot_observed = true;
            current_boot_id = hello.boot_id;
            time_filter.on_boot_change(hello.boot_id);
            pose_interpolator.reset();
        }
        exposure_us = (hello.exposure_us > 0) ? hello.exposure_us : 10000;
    });

    control_framer.set_pose_callback([&](const PosePayload& pose) {
        pose_interpolator.push_pose(pose);
    });

    control_framer.set_time_resp_callback([&](const TimeRespPayload& resp) {
        time_filter.on_time_resp(resp.echo, resp.t2, resp.t3, current_ctrl_rx_us, current_boot_id);
    });

    control_framer.set_ack_callback([&](const AckPayload& ack) {
        if (ack.cmd_id == 1001 && ack.result == 0) {
            results.homing_ack_observed = true;
        }
    });

    int curr_video_conn = 1;
    bool video_connected = true;
    uint16_t last_boot = 0;
    std::optional<uint32_t> last_seq;
    uint32_t prev_cam_exp = 0;

    video_framer.set_frame_callback([&](const VideoFrame& frame) {
        uint16_t boot = frame.header.boot_id;
        uint32_t seq = frame.header.frame_seq;

        // Check for 32-bit clock rollover
        if (prev_cam_exp > 4290000000U && frame.header.exposure_start < 100000000U) {
            results.clock_wrap_observed = true;
        }
        prev_cam_exp = frame.header.exposure_start;

        if (frame.fate == FrameFate::Cut) {
            results.wire_drop_observed = true;
            results.classified_fates[{boot, seq}] = "cut";
            last_boot = boot;
            last_seq = seq;
            return;
        }

        // Gap classification
        if (!video_connected) {
            // First frame received on newly connected/reconnected session
            if (last_boot == boot && last_seq.has_value()) {
                for (uint32_t m = *last_seq + 1; m < seq; ++m) {
                    results.classified_fates[{boot, m}] = "no_client";
                }
            } else if (boot > last_boot) {
                // Camera rebooted while disconnected
                for (uint32_t m = 0; m < seq; ++m) {
                    results.classified_fates[{boot, m}] = "no_client";
                }
            }
            video_connected = true;
        } else {
            // Continuously connected stream gap: dropped by camera
            if (last_boot == boot && last_seq.has_value()) {
                for (uint32_t m = *last_seq + 1; m < seq; ++m) {
                    results.classified_fates[{boot, m}] = "dropped_by_camera";
                }
            }
        }

        std::string fate_str = (frame.fate == FrameFate::Sent) ? "sent" : "corrupt";
        if (frame.fate == FrameFate::Corrupt) {
            results.corrupt_frames_detected++;
        }
        results.classified_fates[{boot, seq}] = fate_str;
        last_boot = boot;
        last_seq = seq;

        // Check truth values if present
        auto it = truth.find({boot, seq});
        if (it != truth.end()) {
            const auto& tr = it->second;

            // 1. Clock synchronization accuracy
            if (time_filter.is_synchronized()) {
                uint64_t est_exp_start = time_filter.camera_to_server_us(frame.header.exposure_start);
                double err = std::abs(static_cast<double>(est_exp_start) - static_cast<double>(tr.exposure_start_us));
                results.clock_errors_us.push_back(err);
            }

            // 2. Pose Interpolation accuracy at mid-exposure
            uint32_t t_mid_cam = frame.header.exposure_start + (exposure_us / 2);
            auto pose = pose_interpolator.interpolate(t_mid_cam);
            if (pose.valid && pose.homed) {
                double p_err = std::abs(pose.pan_deg - tr.pan_deg);
                double t_err = std::abs(pose.tilt_deg - tr.tilt_deg);
                results.pan_errors_deg.push_back(p_err);
                results.tilt_errors_deg.push_back(t_err);
            }

            // 3. Embedded truth stamp extraction accuracy
            double stamp_pan = frame.truth_stamp.pan_counts / 1000.0;
            double stamp_tilt = frame.truth_stamp.tilt_counts / 1000.0;
            if (std::abs(stamp_pan - tr.pan_deg) < 0.001 &&
                std::abs(stamp_tilt - tr.tilt_deg) < 0.001 &&
                frame.truth_stamp.frame_seq == tr.frame_seq) {
                results.truth_stamp_matches++;
            }
        }
    });

    // Execute sequential replay
    for (const auto& read : all_reads) {
        if (read.type == ReadRecord::Type::Control) {
            current_ctrl_rx_us = read.rx_us;
            control_framer.push_data(std::span<const uint8_t>(control_bin.data() + read.offset, read.bytes));
            control_framer.parse_available();
        } else {
            if (read.connection != curr_video_conn) {
                video_framer.on_connection_lost();
                video_connected = false;
                curr_video_conn = read.connection;
            }
            video_framer.push_data(std::span<const uint8_t>(video_bin.data() + read.offset, read.bytes));
            video_framer.parse_available();
        }
    }

    results.garbage_bytes_recovered = video_framer.garbage_bytes_discarded();
    return results;
}

} // namespace

int main() {
    std::cout << "====================================================================\n";
    std::cout << "TEST SUITE: Offline Capture Dataset Validation (20-Second Flight Log)\n";
    std::cout << "====================================================================\n\n";

    fs::path capture_dir = find_capture_dir();
    auto truth = load_truth_csv(capture_dir / "truth.csv");
    auto events = load_events_csv(capture_dir / "events.csv");

    auto results = run_offline_capture_replay(capture_dir);

    // =========================================================================
    // BENCHMARK 1: Clock Synchronization Accuracy (< 100 µs error)
    // =========================================================================
    std::cout << "\n[Benchmark 1] Clock Synchronization Recovery Benchmark:\n";
    assert(!results.clock_errors_us.empty() && "No synchronized frames evaluated!");
    double max_clock_err = *std::max_element(results.clock_errors_us.begin(), results.clock_errors_us.end());
    double sum_clock_err = 0;
    for (double e : results.clock_errors_us) sum_clock_err += e;
    double mean_clock_err = sum_clock_err / results.clock_errors_us.size();

    std::cout << "  Synchronized Frames Evaluated : " << results.clock_errors_us.size() << "\n";
    std::cout << "  Maximum Clock Error          : " << max_clock_err << " µs (Spec limit: < 100 µs)\n";
    std::cout << "  Mean Clock Error             : " << mean_clock_err << " µs\n";
    assert(max_clock_err < 100.0 && "Clock sync error exceeded 100 µs specification threshold!");
    assert(mean_clock_err < 50.0 && "Mean clock sync error higher than expected!");
    std::cout << "  --> PASS: Clock sync within <100 µs tolerance.\n";

    // =========================================================================
    // BENCHMARK 2: Pose Interpolation Accuracy (< 0.1° error)
    // =========================================================================
    std::cout << "\n[Benchmark 2] Mid-Exposure Pose Interpolation Benchmark:\n";
    assert(!results.pan_errors_deg.empty() && "No homed pose samples evaluated!");
    double max_pan_err = *std::max_element(results.pan_errors_deg.begin(), results.pan_errors_deg.end());
    double max_tilt_err = *std::max_element(results.tilt_errors_deg.begin(), results.tilt_errors_deg.end());
    double sum_pan_err = 0, sum_tilt_err = 0;
    for (double e : results.pan_errors_deg) sum_pan_err += e;
    for (double e : results.tilt_errors_deg) sum_tilt_err += e;
    double mean_pan_err = sum_pan_err / results.pan_errors_deg.size();
    double mean_tilt_err = sum_tilt_err / results.tilt_errors_deg.size();

    std::cout << "  Homed Frames Evaluated       : " << results.pan_errors_deg.size() << "\n";
    std::cout << "  Max Pan Angle Error          : " << max_pan_err << "° (Spec limit: < 0.1°)\n";
    std::cout << "  Mean Pan Angle Error         : " << mean_pan_err << "°\n";
    std::cout << "  Max Tilt Angle Error         : " << max_tilt_err << "° (Spec limit: < 0.1°)\n";
    std::cout << "  Mean Tilt Angle Error        : " << mean_tilt_err << "°\n";
    assert(max_pan_err < 0.10 && "Pan interpolation error exceeded 0.1° limit!");
    assert(max_tilt_err < 0.10 && "Tilt interpolation error exceeded 0.1° limit!");
    std::cout << "  --> PASS: Pan and tilt interpolation within <0.1° tolerance.\n";

    // =========================================================================
    // BENCHMARK 3: Truth Stamp Extraction
    // =========================================================================
    std::cout << "\n[Benchmark 3] Truth Stamp Wire Header Extraction:\n";
    std::cout << "  Truth Stamp Matches          : " << results.truth_stamp_matches << " / 444 valid/corrupt frames\n";
    assert(results.truth_stamp_matches == 444 && "Mismatch in extracted embedded truth stamps!");
    std::cout << "  --> PASS: 100% of embedded truth stamps verified.\n";

    // =========================================================================
    // BENCHMARK 4: Frame Fate Classification Accuracy (100% precision)
    // =========================================================================
    std::cout << "\n[Benchmark 4] Frame Fate Classification Benchmark:\n";
    std::map<std::string, size_t> expected_counts;
    std::map<std::string, size_t> classified_counts;

    for (const auto& [key, tr] : truth) {
        expected_counts[tr.fate]++;
    }
    for (const auto& [key, fate] : results.classified_fates) {
        classified_counts[fate]++;
    }

    std::cout << "  Fate Breakdown (Classified vs Truth):\n";
    for (const auto& [fate, count] : expected_counts) {
        std::cout << "   - " << fate << ": " << classified_counts[fate] << " / " << count << "\n";
        assert(classified_counts[fate] == count && "Fate count mismatch!");
    }

    size_t fate_mismatches = 0;
    for (const auto& [key, tr] : truth) {
        auto it = results.classified_fates.find(key);
        if (it == results.classified_fates.end() || it->second != tr.fate) {
            fate_mismatches++;
            std::cerr << "  MISMATCH: Boot " << key.first << " Seq " << key.second
                      << " Expected=" << tr.fate << " Got=" << (it != results.classified_fates.end() ? it->second : "MISSING") << "\n";
        }
    }
    if (fate_mismatches > 0) {
        std::cerr << "Fatal: " << fate_mismatches << " fate mismatches detected!\n";
        std::exit(1);
    }
    std::cout << "  --> PASS: Classified all 475 frame fates with 100.0% precision.\n";

    // =========================================================================
    // BENCHMARK 5: Milestone Anomalous Events from events.csv
    // =========================================================================
    std::cout << "\n[Benchmark 5] Anomalous Capture Events Verification:\n";
    std::cout << "  1. 32-bit Clock Wrap at 5s   : " << (results.clock_wrap_observed ? "DETECTED & UNWRAPPED" : "FAILED") << "\n";
    assert(results.clock_wrap_observed);

    std::cout << "  2. Corrupt Frames at 7s      : " << results.corrupt_frames_detected << " / 3 detected\n";
    assert(results.corrupt_frames_detected == 3);

    std::cout << "  3. 5000 Garbage Bytes at 9s  : " << results.garbage_bytes_recovered << " bytes discarded\n";
    assert(results.garbage_bytes_recovered >= 5000);

    std::cout << "  4. Wire Drop / Cut at 11s    : " << (results.wire_drop_observed ? "DETECTED (Frame 7775 Cut)" : "FAILED") << "\n";
    assert(results.wire_drop_observed);

    std::cout << "  5. Camera Reboot at 14s      : " << (results.reboot_observed ? "DETECTED (Boot 3 -> Boot 4)" : "FAILED") << "\n";
    assert(results.reboot_observed);

    std::cout << "  6. Home Command at 16s       : " << (results.homing_ack_observed ? "ACKNOWLEDGED & HOMED" : "FAILED") << "\n";
    assert(results.homing_ack_observed);
    std::cout << "  --> PASS: All historical anomaly events confirmed.\n";

    std::cout << "\n====================================================================\n";
    std::cout << "ALL 5 BENCHMARKS PASSED (100% PRECISION)\n";
    std::cout << "====================================================================\n";
    return 0;
}
