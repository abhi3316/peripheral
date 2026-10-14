#include "peripheral/storage/disk_recorder.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>

using namespace peripheral;

void test_disk_recorder_lifecycle() {
    std::cout << "--- Testing DiskRecorder Lifecycle ---\n";
    std::filesystem::path test_dir = "record_test";
    std::filesystem::remove_all(test_dir);

    DiskRecorderConfig config;
    config.root_dir = test_dir;
    config.chunk_max_bytes = 1024 * 1024; // 1 MB chunks to trigger chunk rotation quickly
    config.disk_quota_bytes = 50 * 1024 * 1024;

    DiskRecorder recorder(config);
    assert(!recorder.is_recording());
    assert(recorder.list_recordings().empty());

    // 1. Start recording
    bool ok = recorder.start_recording("rec-001");
    assert(ok);
    (void)ok;
    assert(recorder.is_recording());

    auto active = recorder.active_recording();
    assert(active.has_value());
    assert(active->recording_id == "rec-001");
    assert(active->state == "recording");

    // 2. Feed simulated frames for cam0 and cam1
    for (uint32_t seq = 1; seq <= 5; ++seq) {
        VideoFrame f0{};
        f0.header.frame_seq = seq;
        f0.header.boot_id = 1;
        f0.header.width = 320;
        f0.header.height = 200;
        f0.image.resize(320 * 200, 100);
        f0.fate = FrameFate::Sent;
        recorder.on_frame("cam0", f0);

        VideoFrame f1{};
        f1.header.frame_seq = seq;
        f1.header.boot_id = 1;
        f1.header.width = 320;
        f1.header.height = 200;
        f1.image.resize(320 * 200, 200);
        f1.fate = FrameFate::Sent;
        recorder.on_frame("cam1", f1);
    }

    // 3. Test gap detection: simulate sequence jump from 5 to 10 on cam0 (missing frames 6..9)
    VideoFrame f_jump{};
    f_jump.header.frame_seq = 10;
    f_jump.header.boot_id = 1;
    f_jump.header.width = 320;
    f_jump.header.height = 200;
    f_jump.image.resize(320 * 200, 100);
    f_jump.fate = FrameFate::Sent;
    recorder.on_frame("cam0", f_jump);

    // 4. Test damaged frame gap detection on cam1
    VideoFrame f_corrupt{};
    f_corrupt.header.frame_seq = 6;
    f_corrupt.header.boot_id = 1;
    f_corrupt.header.width = 320;
    f_corrupt.header.height = 200;
    f_corrupt.image.resize(320 * 200, 200);
    f_corrupt.fate = FrameFate::Corrupt;
    recorder.on_frame("cam1", f_corrupt);

    // 5. Verify live metrics
    active = recorder.active_recording();
    assert(active.has_value());
    assert(active->frames == 12);
    assert(active->bytes > 0);
    assert(active->gaps.size() == 2);
    assert(active->gaps[0].camera_id == "cam0");
    assert(active->gaps[0].from_seq == 6);
    assert(active->gaps[0].to_seq == 9);
    assert(active->gaps[0].reason == "dropped_by_camera");

    assert(active->gaps[1].camera_id == "cam1");
    assert(active->gaps[1].from_seq == 6);
    assert(active->gaps[1].to_seq == 6);
    assert(active->gaps[1].reason == "damaged");

    // 6. Stop recording
    ok = recorder.stop_recording();
    assert(ok);
    assert(!recorder.is_recording());
    assert(!recorder.active_recording().has_value());

    // 7. Verify persistence on disk
    std::filesystem::path rec_dir = test_dir / "rec-001";
    assert(std::filesystem::exists(rec_dir));
    assert(std::filesystem::exists(rec_dir / "manifest.json"));
    assert(std::filesystem::exists(rec_dir / "part-0001.bin"));

    auto list = recorder.list_recordings();
    assert(list.size() == 1);
    assert(list[0].recording_id == "rec-001");
    assert(list[0].state == "stopped");
    assert(list[0].frames == 12);
    assert(list[0].gaps.size() == 2);

    std::string json = recorder.list_recordings_json();
    assert(json.find("\"recordingId\": \"rec-001\"") != std::string::npos);
    assert(json.find("\"dropped_by_camera\"") != std::string::npos);
    assert(json.find("\"damaged\"") != std::string::npos);

    // Clean up
    std::filesystem::remove_all(test_dir);
    std::cout << "[PASS] DiskRecorder Lifecycle Test Succeeded!\n";
}

void test_disk_reclamation_policy() {
    std::cout << "--- Testing Automatic Disk Reclamation Policy (80% High Watermark) ---\n";
    std::filesystem::path test_dir = "record_reclaim_test";
    std::filesystem::remove_all(test_dir);

    DiskRecorderConfig config;
    config.root_dir = test_dir;
    config.chunk_max_bytes = 20 * 1024;               // 20 KB chunk rotation
    config.reclamation_threshold_bytes = 50 * 1024;    // 50 KB (80% threshold for test)
    config.emergency_quota_bytes = 100 * 1024;         // 100 KB emergency quota
    config.disk_quota_bytes = 120 * 1024;

    DiskRecorder recorder(config);

    // 1. Record session rec-001 producing multiple chunks
    bool ok = recorder.start_recording("rec-001");
    assert(ok);
    (void)ok;

    for (uint32_t seq = 1; seq <= 8; ++seq) {
        VideoFrame f{};
        f.header.frame_seq = seq;
        f.header.boot_id = 1;
        f.header.width = 100;
        f.header.height = 100;
        f.image.resize(100 * 100, 128); // 10,000 bytes per frame
        f.fate = FrameFate::Sent;
        recorder.on_frame("cam0", f);
    }
    recorder.stop_recording();

    std::filesystem::path rec001_dir = test_dir / "rec-001";
    assert(std::filesystem::exists(rec001_dir));
    assert(std::filesystem::exists(rec001_dir / "manifest.json"));
    assert(std::filesystem::exists(rec001_dir / "part-0001.bin"));
    assert(std::filesystem::exists(rec001_dir / "part-0002.bin"));

    uint64_t used_before = recorder.total_disk_used_bytes();
    std::cout << "  Recorded 8 frames in rec-001. Total disk used: " << used_before << " bytes\n";
    assert(used_before >= config.reclamation_threshold_bytes);

    // 2. SAFETY CHECK: Reclaim without .uploaded markers must NOT delete any chunks!
    uint64_t reclaimed_unuploaded = recorder.reclaim_disk_space();
    assert(reclaimed_unuploaded == 0 && "Unuploaded chunks must never be evicted!");
    (void)reclaimed_unuploaded;
    assert(std::filesystem::exists(rec001_dir / "part-0001.bin"));
    assert(std::filesystem::exists(rec001_dir / "part-0002.bin"));

    // 3. Simulate cloud uploader completing part-0001.bin and writing receipt marker
    std::ofstream marker1(rec001_dir / "part-0001.bin.uploaded");
    marker1 << "{\"status\":\"uploaded\"}\n";
    marker1.close();

    // 4. Trigger disk reclamation
    uint64_t freed = recorder.reclaim_disk_space();
    assert(freed > 0 && "Uploaded chunk must be reclaimed when over threshold!");
    (void)freed;
    assert(recorder.total_bytes_reclaimed() == freed);

    // 5. Assert that part-0001.bin was deleted, but part-0001.bin.uploaded and manifest.json remain!
    assert(!std::filesystem::exists(rec001_dir / "part-0001.bin") && "Evicted .bin must be removed!");
    assert(std::filesystem::exists(rec001_dir / "part-0001.bin.uploaded") && "Receipt marker must be preserved!");
    assert(std::filesystem::exists(rec001_dir / "manifest.json") && "Manifest must be preserved!");

    // part-0002.bin was NOT uploaded, so it must still be intact on disk!
    assert(std::filesystem::exists(rec001_dir / "part-0002.bin") && "Unuploaded part-0002.bin must remain!");

    // 6. Verify determine_upload_status handles evicted chunks seamlessly
    auto list = recorder.list_recordings();
    assert(list.size() == 1);
    assert(list[0].upload == "partial");

    // Upload part-0002.bin and manifest.json
    std::ofstream marker2(rec001_dir / "part-0002.bin.uploaded");
    marker2 << "{\"status\":\"uploaded\"}\n";
    marker2.close();
    std::ofstream marker_mf(rec001_dir / "manifest.json.uploaded");
    marker_mf << "{\"status\":\"uploaded\"}\n";
    marker_mf.close();

    list = recorder.list_recordings();
    assert(list.size() == 1);
    assert(list[0].upload == "uploaded");

    // Clean up
    std::filesystem::remove_all(test_dir);
    std::cout << "[PASS] Automatic Disk Reclamation Policy Succeeded!\n";
}

void test_emergency_disk_full_gap_accounting() {
    std::cout << "--- Testing Emergency Disk Full (95% Quota) Gap Accounting ---\n";
    std::filesystem::path test_dir = "record_quota_test";
    std::filesystem::remove_all(test_dir);

    DiskRecorderConfig config;
    config.root_dir = test_dir;
    config.chunk_max_bytes = 100 * 1024;
    config.reclamation_threshold_bytes = 40 * 1024;
    config.emergency_quota_bytes = 45 * 1024;         // Small 45 KB emergency limit
    config.disk_quota_bytes = 50 * 1024;

    DiskRecorder recorder(config);
    bool ok = recorder.start_recording("rec-full");
    assert(ok);
    (void)ok;

    // Feed frames until emergency quota is saturated
    // Each frame is ~10,032 bytes
    for (uint32_t seq = 1; seq <= 10; ++seq) {
        VideoFrame f{};
        f.header.frame_seq = seq;
        f.header.boot_id = 1;
        f.header.width = 100;
        f.header.height = 100;
        f.image.resize(100 * 100, 128);
        f.fate = FrameFate::Sent;
        recorder.on_frame("cam0", f);
    }

    auto active = recorder.active_recording();
    assert(active.has_value());
    assert(active->frames < 10 && "Excess frames must not be written past emergency quota!");
    assert(!active->gaps.empty() && "disk_full gap must be recorded!");

    bool found_disk_full_gap = false;
    for (const auto& g : active->gaps) {
        if (g.reason == "disk_full") {
            found_disk_full_gap = true;
            break;
        }
    }
    assert(found_disk_full_gap && "Expected disk_full gap reason when emergency quota exceeded!");
    (void)found_disk_full_gap;

    recorder.stop_recording();
    std::filesystem::remove_all(test_dir);
    std::cout << "[PASS] Emergency Disk Full Gap Accounting Succeeded!\n";
}

int main() {
    test_disk_recorder_lifecycle();
    test_disk_reclamation_policy();
    test_emergency_disk_full_gap_accounting();
    return 0;
}

