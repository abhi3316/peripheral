#include "peripheral/peripheral.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

namespace {

std::vector<uint8_t> create_valid_test_frame(uint32_t seq, uint16_t boot_id, uint16_t width, uint16_t height, int32_t pan_milli, int32_t tilt_milli) {
    size_t payload_bytes = static_cast<size_t>(width) * height;
    std::vector<uint8_t> image(payload_bytes, 128); // Greyscale 128

    // Truth stamp in first 12 bytes
    peripheral::TruthStamp stamp{
        .pan_counts = pan_milli,
        .tilt_counts = tilt_milli,
        .frame_seq = seq
    };
    std::memcpy(image.data(), &stamp, sizeof(stamp));

    uint32_t payload_crc = peripheral::crc32(image);

    peripheral::VideoFrameHeader hdr{};
    hdr.start_marker = peripheral::VFRM_START_MARKER;
    hdr.frame_seq = seq;
    hdr.exposure_start = 100000;
    hdr.boot_id = boot_id;
    hdr.width = width;
    hdr.height = height;
    hdr.payload_crc32 = payload_crc;

    uint8_t hdr_bytes[28];
    std::memcpy(hdr_bytes, &hdr, 28);
    hdr.header_crc32 = peripheral::crc32(std::span<const uint8_t>(hdr_bytes, 28));

    std::vector<uint8_t> wire(sizeof(peripheral::VideoFrameHeader) + payload_bytes);
    std::memcpy(wire.data(), &hdr, sizeof(hdr));
    std::memcpy(wire.data() + sizeof(hdr), image.data(), payload_bytes);
    return wire;
}

void test_video_framer_normal_and_truth_stamp() {
    std::cout << "[Test 1] VideoFramer: Normal valid frame parsing and truth stamp extraction...\n";

    peripheral::VideoFramer framer;
    size_t frames_received = 0;
    framer.set_frame_callback([&frames_received](const peripheral::VideoFrame& frame) {
        assert(frame.is_valid());
        assert(frame.seq() == 1);
        assert(frame.width() == 200);
        assert(frame.height() == 100);
        assert(frame.truth_stamp.pan_counts == 12345);
        assert(frame.truth_stamp.tilt_counts == -5432);
        frames_received++;
    });

    auto wire = create_valid_test_frame(1, 1, 200, 100, 12345, -5432);
    framer.push_data(wire);
    auto parsed = framer.parse_available();

    assert(parsed.size() == 1);
    assert(frames_received == 1);
    assert(framer.valid_frames_count() == 1);
    assert(framer.corrupt_frames_count() == 0);
    std::cout << "  Passed: 1 valid frame parsed with correct truth stamp.\n";
}

void test_video_framer_corrupt_payload() {
    std::cout << "[Test 2] VideoFramer: Corrupt payload detection (Corrupt fate)...\n";

    peripheral::VideoFramer framer;
    framer.set_frame_callback([](const peripheral::VideoFrame& frame) {
        assert(frame.fate == peripheral::FrameFate::Corrupt);
        assert(!frame.is_valid());
    });

    auto wire = create_valid_test_frame(2, 1, 100, 50, 0, 0);
    // Damage one pixel in the payload without modifying header CRC
    wire[sizeof(peripheral::VideoFrameHeader) + 25] ^= 0xFF;

    framer.push_data(wire);
    auto parsed = framer.parse_available();

    assert(parsed.size() == 1);
    assert(parsed[0].fate == peripheral::FrameFate::Corrupt);
    assert(framer.corrupt_frames_count() == 1);
    std::cout << "  Passed: Frame with damaged pixels classified as Corrupt fate.\n";
}

void test_video_framer_garbage_recovery() {
    std::cout << "[Test 3] VideoFramer: Automatic 5,000-byte garbage recovery...\n";

    peripheral::VideoFramer framer;
    framer.set_frame_callback([](const peripheral::VideoFrame& frame) {
        assert(frame.is_valid());
        assert(frame.seq() == 42);
    });

    // Generate 5,000 random garbage bytes
    std::vector<uint8_t> stream_data(5000);
    std::mt19937 rng(1337);
    for (auto& b : stream_data) {
        b = static_cast<uint8_t>(rng() & 0xFF);
    }

    // Append valid frame after garbage
    auto frame_bytes = create_valid_test_frame(42, 1, 100, 50, 1000, 2000);
    stream_data.insert(stream_data.end(), frame_bytes.begin(), frame_bytes.end());

    framer.push_data(stream_data);
    auto parsed = framer.parse_available();

    assert(parsed.size() == 1);
    assert(parsed[0].seq() == 42);
    assert(framer.garbage_bytes_discarded() == 5000);
    std::cout << "  Passed: Skipped 5,000 garbage bytes and synchronized to VFRM frame.\n";
}

void test_control_framer() {
    std::cout << "[Test 4] ControlFramer: Parsing HELLO, POSE, TIME_RESP, ACK and outgoing serialization...\n";

    peripheral::ControlFramer framer;
    bool hello_seen = false;
    bool pose_seen = false;
    bool ack_seen = false;

    framer.set_hello_callback([&hello_seen](const peripheral::HelloPayload& hello) {
        hello_seen = true;
        assert(hello.boot_id == 1);
        assert(hello.homed == 1);
    });

    framer.set_pose_callback([&pose_seen](const peripheral::PosePayload& pose) {
        pose_seen = true;
        assert(pose.pan_counts == 450);
        assert(pose.tilt_counts == -150);
        assert(pose.motion == 2);
    });

    framer.set_ack_callback([&ack_seen](const peripheral::AckPayload& ack) {
        ack_seen = true;
        assert(ack.cmd_id == 99);
        assert(ack.result == 0);
    });

    // 1. Test HELLO packet
    peripheral::ControlHeader hello_hdr{};
    hello_hdr.start_marker = peripheral::PTCL_START_MARKER;
    hello_hdr.version = 1;
    hello_hdr.type = static_cast<uint8_t>(peripheral::ControlMsgType::Hello);
    hello_hdr.payload_bytes = sizeof(peripheral::HelloPayload);
    hello_hdr.msg_seq = 1;

    peripheral::HelloPayload hello_payload{};
    std::strncpy(hello_payload.camera_id, "cam0", sizeof(hello_payload.camera_id));
    hello_payload.boot_id = 1;
    hello_payload.homed = 1;

    uint8_t hdr12[12];
    std::memcpy(hdr12, &hello_hdr, 12);
    uint32_t c = peripheral::crc32(std::span<const uint8_t>(hdr12, 12));
    c = peripheral::crc32(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&hello_payload), sizeof(hello_payload)), c);
    hello_hdr.crc32 = c;

    std::vector<uint8_t> hello_wire(sizeof(hello_hdr) + sizeof(hello_payload));
    std::memcpy(hello_wire.data(), &hello_hdr, sizeof(hello_hdr));
    std::memcpy(hello_wire.data() + sizeof(hello_hdr), &hello_payload, sizeof(hello_payload));

    framer.push_data(hello_wire);

    // 2. Test POSE packet
    peripheral::ControlHeader pose_hdr{};
    pose_hdr.start_marker = peripheral::PTCL_START_MARKER;
    pose_hdr.version = 1;
    pose_hdr.type = static_cast<uint8_t>(peripheral::ControlMsgType::Pose);
    pose_hdr.payload_bytes = sizeof(peripheral::PosePayload);
    pose_hdr.msg_seq = 2;

    peripheral::PosePayload pose_payload{
        .counter = 500000,
        .pan_counts = 450,
        .tilt_counts = -150,
        .motion = 2,
        .homed = 1,
        .fault = 0,
        .reserved = 0
    };

    std::memcpy(hdr12, &pose_hdr, 12);
    c = peripheral::crc32(std::span<const uint8_t>(hdr12, 12));
    c = peripheral::crc32(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pose_payload), sizeof(pose_payload)), c);
    pose_hdr.crc32 = c;

    std::vector<uint8_t> pose_wire(sizeof(pose_hdr) + sizeof(pose_payload));
    std::memcpy(pose_wire.data(), &pose_hdr, sizeof(pose_hdr));
    std::memcpy(pose_wire.data() + sizeof(pose_hdr), &pose_payload, sizeof(pose_payload));

    framer.push_data(pose_wire);

    // 3. Test ACK packet
    peripheral::ControlHeader ack_hdr{};
    ack_hdr.start_marker = peripheral::PTCL_START_MARKER;
    ack_hdr.version = 1;
    ack_hdr.type = static_cast<uint8_t>(peripheral::ControlMsgType::Ack);
    ack_hdr.payload_bytes = sizeof(peripheral::AckPayload);
    ack_hdr.msg_seq = 3;

    peripheral::AckPayload ack_payload{
        .cmd_id = 99,
        .result = 0,
        .duplicate = 0,
        .reserved = 0
    };

    std::memcpy(hdr12, &ack_hdr, 12);
    c = peripheral::crc32(std::span<const uint8_t>(hdr12, 12));
    c = peripheral::crc32(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&ack_payload), sizeof(ack_payload)), c);
    ack_hdr.crc32 = c;

    std::vector<uint8_t> ack_wire(sizeof(ack_hdr) + sizeof(ack_payload));
    std::memcpy(ack_wire.data(), &ack_hdr, sizeof(ack_hdr));
    std::memcpy(ack_wire.data() + sizeof(ack_hdr), &ack_payload, sizeof(ack_payload));

    framer.push_data(ack_wire);

    size_t count = framer.parse_available();
    (void)count;
    assert(count == 3);
    assert(hello_seen);
    assert(pose_seen);
    assert(ack_seen);

    // 4. Test Outgoing serialization
    auto time_req_bytes = peripheral::ControlFramer::serialize_time_req(0xCAFEBABEDEADBEEF, 10);
    assert(time_req_bytes.size() == sizeof(peripheral::ControlHeader) + sizeof(peripheral::TimeReqPayload));

    auto cmd_bytes = peripheral::ControlFramer::serialize_cmd(100, peripheral::CommandType::Jog, 15.0f, -5.0f, 11);
    assert(cmd_bytes.size() == sizeof(peripheral::ControlHeader) + sizeof(peripheral::CmdPayload));

    std::cout << "  Passed: Control messages parsed and serialized accurately.\n";
}

void test_control_framer_garbage_and_bad_crc() {
    std::cout << "[Test 5] ControlFramer: Discarding socket noise, bad markers, and corrupt CRC packets...\n";

    peripheral::ControlFramer framer;
    size_t valid_pose_received = 0;
    framer.set_pose_callback([&valid_pose_received](const peripheral::PosePayload& p) {
        assert(p.pan_counts == 300);
        valid_pose_received++;
    });

    std::vector<uint8_t> noisy_stream;

    // 1. Injected 2,000 bytes of arbitrary socket noise
    std::mt19937 rng(42);
    for (size_t i = 0; i < 2000; ++i) {
        noisy_stream.push_back(static_cast<uint8_t>(rng() & 0xFF));
    }

    // 2. Corrupted Packet: Invalid Version
    peripheral::ControlHeader bad_ver_hdr{};
    bad_ver_hdr.start_marker = peripheral::PTCL_START_MARKER;
    bad_ver_hdr.version = 99; // Invalid version
    bad_ver_hdr.type = static_cast<uint8_t>(peripheral::ControlMsgType::Pose);
    bad_ver_hdr.payload_bytes = sizeof(peripheral::PosePayload);
    peripheral::PosePayload dummy_payload{};
    noisy_stream.insert(noisy_stream.end(), reinterpret_cast<const uint8_t*>(&bad_ver_hdr),
                        reinterpret_cast<const uint8_t*>(&bad_ver_hdr) + sizeof(bad_ver_hdr));
    noisy_stream.insert(noisy_stream.end(), reinterpret_cast<const uint8_t*>(&dummy_payload),
                        reinterpret_cast<const uint8_t*>(&dummy_payload) + sizeof(dummy_payload));

    // 3. Corrupted Packet: Damaged CRC Checksum
    peripheral::ControlHeader bad_crc_hdr{};
    bad_crc_hdr.start_marker = peripheral::PTCL_START_MARKER;
    bad_crc_hdr.version = 1;
    bad_crc_hdr.type = static_cast<uint8_t>(peripheral::ControlMsgType::Pose);
    bad_crc_hdr.payload_bytes = sizeof(peripheral::PosePayload);
    bad_crc_hdr.crc32 = 0xDEADBEEF; // Corrupt CRC
    noisy_stream.insert(noisy_stream.end(), reinterpret_cast<const uint8_t*>(&bad_crc_hdr),
                        reinterpret_cast<const uint8_t*>(&bad_crc_hdr) + sizeof(bad_crc_hdr));
    noisy_stream.insert(noisy_stream.end(), reinterpret_cast<const uint8_t*>(&dummy_payload),
                        reinterpret_cast<const uint8_t*>(&dummy_payload) + sizeof(dummy_payload));

    // 4. Trailing socket noise
    for (size_t i = 0; i < 500; ++i) {
        noisy_stream.push_back(static_cast<uint8_t>(rng() & 0xFF));
    }

    // 5. Valid POSE Packet
    peripheral::ControlHeader valid_hdr{};
    valid_hdr.start_marker = peripheral::PTCL_START_MARKER;
    valid_hdr.version = 1;
    valid_hdr.type = static_cast<uint8_t>(peripheral::ControlMsgType::Pose);
    valid_hdr.payload_bytes = sizeof(peripheral::PosePayload);
    valid_hdr.msg_seq = 42;

    peripheral::PosePayload valid_payload{};
    valid_payload.counter = 123456;
    valid_payload.pan_counts = 300;
    valid_payload.tilt_counts = 100;
    valid_payload.homed = 1;

    uint8_t hdr12[12];
    std::memcpy(hdr12, &valid_hdr, 12);
    uint32_t c = peripheral::crc32(std::span<const uint8_t>(hdr12, 12));
    c = peripheral::crc32(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&valid_payload), sizeof(valid_payload)), c);
    valid_hdr.crc32 = c;

    noisy_stream.insert(noisy_stream.end(), reinterpret_cast<const uint8_t*>(&valid_hdr),
                        reinterpret_cast<const uint8_t*>(&valid_hdr) + sizeof(valid_hdr));
    noisy_stream.insert(noisy_stream.end(), reinterpret_cast<const uint8_t*>(&valid_payload),
                        reinterpret_cast<const uint8_t*>(&valid_payload) + sizeof(valid_payload));

    framer.push_data(noisy_stream);
    size_t parsed = framer.parse_available();

    assert(parsed == 1);
    assert(valid_pose_received == 1);
    std::cout << "  Passed: All 2,500 noise bytes, bad versions, and corrupt CRCs discarded; valid packet recovered.\n";
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "Running peripheral::Framers Unit Tests  \n";
    std::cout << "========================================\n";

    test_video_framer_normal_and_truth_stamp();
    test_video_framer_corrupt_payload();
    test_video_framer_garbage_recovery();
    test_control_framer();
    test_control_framer_garbage_and_bad_crc();

    std::cout << "All Framers unit tests passed successfully!\n";
    return 0;
}
