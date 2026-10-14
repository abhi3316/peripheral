#include "peripheral/camera/pose_interpolator.hpp"
#include "peripheral/camera/time_sync.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace peripheral;

void test_ptp_offset_and_delay() {
    std::cout << "[Test] PTP Clock Offset & Delay Calculation...\n";
    TimeSyncFilter filter;
    assert(!filter.is_synchronized());

    // Server sends TIME_REQ at t1 = 10,000,000 µs
    uint64_t t1 = 10000000;
    // Camera receives at t2 = 2,000,100 µs (camera is running ~8,000,000 µs behind server)
    uint32_t t2 = 2000100;
    // Camera replies at t3 = 2,000,120 µs (20 µs camera dwell)
    uint32_t t3 = 2000120;
    // Server receives TIME_RESP at t4 = 10,000,320 µs (320 µs total round trip)
    uint64_t t4 = 10000320;

    filter.on_time_resp(t1, t2, t3, t4, 1);
    assert(filter.is_synchronized());

    // Expected RTT = (320 - 20) = 300 µs
    assert(filter.last_rtt_us() == 300);

    // Expected Offset = ((10000000 - 2000100) + (10000320 - 2000120)) / 2
    //                 = (7999900 + 8000200) / 2 = 8000050 µs
    assert(filter.current_offset_us() == 8000050);

    // Translation: camera time 2,000,100 + 8,000,050 = 10,000,150 (midpoint of transmission)
    uint64_t server_time = filter.camera_to_server_us(2000100);
    assert(server_time == 10000150);

    // Inverse translation
    uint32_t cam_time = filter.server_to_camera_us(10000150);
    assert(cam_time == 2000100);

    std::cout << "  Passed (RTT: " << filter.last_rtt_us() << "µs, Offset: " << filter.current_offset_us() << "µs)\n";
}

void test_counter_wrap_around() {
    std::cout << "[Test] 32-bit Counter Wrap-Around Handling...\n";
    TimeSyncFilter filter;

    // Near 2^32 - 1 = 4,294,967,295 µs
    uint64_t t1 = 5000000000ULL;
    uint32_t t2 = 4294960000U;
    uint32_t t3 = 4294960010U;
    uint64_t t4 = 5000000200ULL;
    filter.on_time_resp(t1, t2, t3, t4, 1);
    assert(filter.is_synchronized());

    int64_t offset_before = filter.current_offset_us();

    // Now roll over past 2^32 to 50,000 µs
    // Elapsed camera time from t2 to 50,000 is (2^32 - 4294960000) + 50000 = 7296 + 50000 = 57296 µs
    uint64_t dt = 57296;
    uint64_t t1_wrap = t1 + dt;
    uint32_t t2_wrap = 50000U; // rolled over
    uint32_t t3_wrap = 50010U;
    uint64_t t4_wrap = t4 + dt;

    filter.on_time_resp(t1_wrap, t2_wrap, t3_wrap, t4_wrap, 1);

    // Filter should smoothly maintain offset without a 4-billion microsecond jump
    int64_t offset_after = filter.current_offset_us();
    assert(std::abs(offset_after - offset_before) < 1000);

    // Camera time 50,000 should translate to server time ~5,000,100,000
    uint64_t translated = filter.camera_to_server_us(50000U);
    assert(translated > 5000000000ULL);

    std::cout << "  Passed (Smooth 2^32 wrap verified)\n";
}

void test_jitter_rejection() {
    std::cout << "[Test] Jitter Outlier Rejection...\n";
    TimeSyncFilter filter;

    uint64_t t1 = 10000000;
    filter.on_time_resp(t1, 1000000, 1000010, t1 + 200, 1);
    filter.on_time_resp(t1 + 250000, 1250000, 1250010, t1 + 250210, 1);
    filter.on_time_resp(t1 + 500000, 1500000, 1500010, t1 + 500190, 1);

    int64_t stable_offset = filter.current_offset_us();

    // Now inject a massive 50 ms network delay spike (50,000 µs)
    uint64_t t1_spike = t1 + 750000;
    uint32_t t2_spike = 1750000;
    uint32_t t3_spike = 1750010;
    uint64_t t4_spike = t1_spike + 50000; // 50 ms spike

    filter.on_time_resp(t1_spike, t2_spike, t3_spike, t4_spike, 1);

    // Jitter filter must have rejected this outlier; offset should remain stable
    int64_t offset_after_spike = filter.current_offset_us();
    assert(std::abs(offset_after_spike - stable_offset) < 500);

    std::cout << "  Passed (Spike rejected, offset deviation < 500µs)\n";
}

void test_boot_reset() {
    std::cout << "[Test] Camera Reboot / Boot ID Change Reset...\n";
    TimeSyncFilter filter;

    filter.on_time_resp(10000000, 1000000, 1000010, 10000200, 1);
    assert(filter.is_synchronized());
    assert(filter.sample_count() == 1);

    // Camera reboots: boot_id becomes 2 with a new random clock start
    filter.on_time_resp(20000000, 99999999, 100000009, 20000200, 2);
    assert(filter.sample_count() == 1); // Reset and started fresh for boot 2
    assert(filter.is_synchronized());

    std::cout << "  Passed (Reboot cleanly reset clock state)\n";
}

void test_pose_interpolator() {
    std::cout << "[Test] 200 Hz Pose Interpolator...\n";
    PoseInterpolator interp;
    assert(interp.size() == 0);

    // Empty buffer
    auto result_empty = interp.interpolate(1000);
    assert(!result_empty.valid);

    // Push sample 1: t = 10,000,000 µs, Pan = 10.0°, Tilt = 2.0°
    PosePayload p1{};
    p1.counter = 10000000;
    p1.pan_counts = 100;  // 10.0°
    p1.tilt_counts = 20;  // 2.0°
    p1.homed = 1;
    interp.push_pose(p1);

    // Single sample returns sample
    auto res1 = interp.interpolate(10000000);
    assert(res1.valid && res1.homed);
    assert(std::abs(res1.pan_deg - 10.0) < 1e-4);
    assert(std::abs(res1.tilt_deg - 2.0) < 1e-4);

    // Push sample 2: t = 10,005,000 µs (5 ms later), Pan = 20.0°, Tilt = 4.0°
    PosePayload p2{};
    p2.counter = 10005000;
    p2.pan_counts = 200;  // 20.0°
    p2.tilt_counts = 40;  // 4.0°
    p2.homed = 1;
    interp.push_pose(p2);

    // Interpolate at t = 10,002,500 µs (exact midpoint)
    auto res_mid = interp.interpolate(10002500);
    assert(res_mid.valid && res_mid.homed);
    assert(std::abs(res_mid.pan_deg - 15.0) < 1e-4);
    assert(std::abs(res_mid.tilt_deg - 3.0) < 1e-4);

    // Interpolate at t = 10,001,000 µs (20% between p1 and p2)
    auto res_20 = interp.interpolate(10001000);
    assert(res_20.valid);
    assert(std::abs(res_20.pan_deg - 12.0) < 1e-4);
    assert(std::abs(res_20.tilt_deg - 2.4) < 1e-4);

    std::cout << "  Passed (Exact midpoint & fraction interpolation verified)\n";
}

int main() {
    std::cout << "====================================================\n";
    std::cout << " Unit Tests: TimeSyncFilter & PoseInterpolator      \n";
    std::cout << "====================================================\n";

    test_ptp_offset_and_delay();
    test_counter_wrap_around();
    test_jitter_rejection();
    test_boot_reset();
    test_pose_interpolator();

    std::cout << "\nAll TimeSync & PoseInterpolator tests PASSED!\n";
    return 0;
}
