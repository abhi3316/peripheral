#include "peripheral/events/atomic_event_ring.hpp"
#include "peripheral/events/event_logger.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace peripheral;

void test_atomic_event_ring_basic() {
    std::cout << "--- Testing AtomicEventRingBuffer Basic Operations ---\n";
    AtomicEventRingBuffer<16> ring; // Small ring for test

    assert(ring.capacity() == 16);
    assert(ring.total_pushed() == 0);
    assert(ring.get_recent_events().empty());

    // 1. Push events across all levels
    uint64_t id1 = ring.push(LogLevel::Debug, "cam0", "debug_msg", "Raw debug payload");
    uint64_t id2 = ring.push(LogLevel::Info, "cam0", "connected", "Camera connected");
    uint64_t id3 = ring.push(LogLevel::Warning, "cam1", "jitter", "High PTP jitter detected");
    uint64_t id4 = ring.push(LogLevel::Alarm, "cam1", "motor_stall", "Motor stalled at 45.2 deg");
    uint64_t id5 = ring.push(LogLevel::Error, "system", "disk_error", "I/O error on storage");

    assert(id1 == 1);
    assert(id2 == 2);
    assert(id3 == 3);
    assert(id4 == 4);
    assert(id5 == 5);
    (void)id1; (void)id2; (void)id3; (void)id4; (void)id5;
    assert(ring.total_pushed() == 5);

    // 2. Query recent events (newest first)
    auto all_events = ring.get_recent_events(10, LogLevel::Debug);
    assert(all_events.size() == 5);
    assert(all_events[0].id == 5);
    assert(all_events[0].level == LogLevel::Error);
    assert(all_events[0].camera_id == "system");
    assert(all_events[0].code == "disk_error");
    assert(all_events[4].id == 1);
    assert(all_events[4].level == LogLevel::Debug);

    // 3. Level classification filtering flag
    auto warn_and_above = ring.get_recent_events(10, LogLevel::Warning);
    assert(warn_and_above.size() == 3); // warning, alarm, error
    assert(warn_and_above[0].level == LogLevel::Error);
    assert(warn_and_above[1].level == LogLevel::Alarm);
    assert(warn_and_above[2].level == LogLevel::Warning);

    auto alarms_only = ring.get_recent_events(10, LogLevel::Alarm);
    assert(alarms_only.size() == 2); // alarm, error

    // 4. get_events_since (oldest to newest for streaming)
    auto since2 = ring.get_events_since(2, 10, LogLevel::Debug);
    assert(since2.size() == 3);
    assert(since2[0].id == 3);
    assert(since2[1].id == 4);
    assert(since2[2].id == 5);

    // 5. Test JSON serialization
    std::string json = all_events[0].to_json();
    assert(json.find("\"id\":5") != std::string::npos);
    assert(json.find("\"level\":\"error\"") != std::string::npos);
    assert(json.find("\"code\":\"disk_error\"") != std::string::npos);

    std::cout << "[PASS] Basic Atomic Ring Buffer Succeeded!\n";
}

void test_atomic_event_ring_concurrency() {
    std::cout << "--- Testing AtomicEventRingBuffer Multi-Threaded Concurrency & Wrapping ---\n";
    AtomicEventRingBuffer<64> ring; // 64 slots

    constexpr int NUM_THREADS = 4;
    constexpr int PUSHES_PER_THREAD = 100; // 400 total pushes > 64 capacity (wraps ~6 times)

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&ring, t]() {
            std::string cam = "cam" + std::to_string(t % 2);
            for (int i = 0; i < PUSHES_PER_THREAD; ++i) {
                ring.push(LogLevel::Info, cam, "frame_stat",
                          "Thread " + std::to_string(t) + " step " + std::to_string(i));
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    assert(ring.total_pushed() == NUM_THREADS * PUSHES_PER_THREAD);
    std::cout << "  Successfully pushed " << ring.total_pushed()
              << " events across " << NUM_THREADS << " threads into 64-slot ring buffer.\n";

    // Read latest events (should fetch up to 64 slots available)
    auto recent = ring.get_recent_events(100, LogLevel::Debug);
    assert(recent.size() == 64);
    assert(recent.front().id == 400);
    assert(recent.back().id == 337);

    // Verify consistency: all IDs must be strictly decreasing by 1
    for (size_t i = 1; i < recent.size(); ++i) {
        assert(recent[i - 1].id == recent[i].id + 1);
    }

    std::cout << "[PASS] Multi-Threaded Atomic Ring Buffer Concurrency Succeeded!\n";
}

void test_alarm_state_machine() {
    std::cout << "--- Testing EventLogger & Alarm Management State Machine ---\n";
    EventLogger logger(LogLevel::Debug);

    assert(logger.active_alarms_count() == 0);

    // 1. Raise alarm
    bool raised = logger.raise_alarm("camera_link_down", "warning", "cam0", "Camera cam0 video link down");
    assert(raised);
    (void)raised;
    assert(logger.active_alarms_count() == 1);

    // Re-raising same alarm should update but not duplicate
    bool duplicate = logger.raise_alarm("camera_link_down", "warning", "cam0", "Camera cam0 video link still down");
    assert(!duplicate);
    (void)duplicate;
    assert(logger.active_alarms_count() == 1);

    // 2. Raise second alarm
    logger.raise_alarm("disk_near_full", "alarm", "system", "Local disk at 92% quota");
    assert(logger.active_alarms_count() == 2);

    auto alarms = logger.get_active_alarms();
    assert(alarms.size() == 2);

    std::string alarms_json = logger.to_alarms_json();
    assert(alarms_json.find("\"code\":\"camera_link_down\"") != std::string::npos);
    assert(alarms_json.find("\"code\":\"disk_near_full\"") != std::string::npos);

    // 3. Clear alarm
    bool cleared = logger.clear_alarm("camera_link_down", "cam0");
    assert(cleared);
    (void)cleared;
    assert(logger.active_alarms_count() == 1);

    // Clearing non-existent alarm returns false
    assert(!logger.clear_alarm("camera_link_down", "cam0"));

    logger.clear_alarm("disk_near_full", "system");
    assert(logger.active_alarms_count() == 0);

    // 4. Verify notifications were pushed to atomic ring
    auto events = logger.get_recent_events(10, LogLevel::Debug);
    assert(events.size() >= 4); // 2 raised + 2 cleared

    // 5. Test subsystem automatic condition monitors
    logger.update_camera_link("cam1", false, true); // Control down
    assert(logger.active_alarms_count() == 1);
    logger.update_camera_link("cam1", true, true);  // Restored
    assert(logger.active_alarms_count() == 0);

    logger.update_motor_status("cam0", true, "Pan motor stall");
    assert(logger.active_alarms_count() == 1);
    logger.update_motor_status("cam0", false);
    assert(logger.active_alarms_count() == 0);

    logger.update_clock_sync("cam0", false, 150.0); // Drift 150 ppm
    assert(logger.active_alarms_count() == 1);
    logger.update_clock_sync("cam0", true, 12.0);   // Normal
    assert(logger.active_alarms_count() == 0);

    logger.update_disk_usage(1800000000ULL, 2000000000ULL); // 90% -> warning
    assert(logger.active_alarms_count() == 1);
    logger.update_disk_usage(1500000000ULL, 2000000000ULL); // 75% -> cleared
    assert(logger.active_alarms_count() == 0);

    std::cout << "[PASS] Alarm Management State Machine Succeeded!\n";
}

int main() {
    test_atomic_event_ring_basic();
    test_atomic_event_ring_concurrency();
    test_alarm_state_machine();
    return 0;
}
