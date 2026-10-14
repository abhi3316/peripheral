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

int main() {
    test_disk_recorder_lifecycle();
    return 0;
}
