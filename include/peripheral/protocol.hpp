#pragma once

#include <cstdint>
#include <string_view>

namespace peripheral {

// Protocol constants
inline constexpr uint32_t VFRM_START_MARKER = 0x4D524656; // 'VFRM' little-endian
inline constexpr uint32_t PTCL_START_MARKER = 0x4C435450; // 'PTCL' little-endian

enum class FrameFate : uint8_t {
    Sent,              // Intact frame (header CRC OK, payload CRC OK)
    Corrupt,           // Header CRC OK, but payload CRC does not match image
    Cut,               // Socket closed partway through frame reception
    DroppedByCamera,   // Camera dropped because server was too slow (frame_seq gap while connected)
    NoClient           // Captured while disconnected (frame_seq jump upon reconnect)
};

[[nodiscard]] constexpr std::string_view to_string(FrameFate fate) noexcept {
    switch (fate) {
        case FrameFate::Sent: return "sent";
        case FrameFate::Corrupt: return "corrupt";
        case FrameFate::Cut: return "cut";
        case FrameFate::DroppedByCamera: return "dropped_by_camera";
        case FrameFate::NoClient: return "no_client";
    }
    return "unknown";
}

enum class ControlMsgType : uint8_t {
    Hello = 0x01,
    Pose = 0x02,
    TimeResp = 0x03,
    Ack = 0x04,
    Status = 0x05,
    TimeReq = 0x81,
    Cmd = 0x82
};

enum class CommandType : uint8_t {
    Home = 1,
    GoTo = 2,
    Jog = 3,
    Stop = 4,
    ClearFault = 5
};

#pragma pack(push, 1)

// 32-byte Video Frame Wire Header
struct VideoFrameHeader {
    uint32_t start_marker{VFRM_START_MARKER};
    uint32_t frame_seq{0};
    uint32_t exposure_start{0}; // Camera clock in µs
    uint16_t boot_id{0};
    uint16_t width{0};
    uint16_t height{0};
    uint8_t reserved[6]{0};
    uint32_t payload_crc32{0};
    uint32_t header_crc32{0}; // CRC32 of bytes 0..27
};
static_assert(sizeof(VideoFrameHeader) == 32, "VideoFrameHeader must be exactly 32 bytes");

// 12-byte Simulator Truth Stamp (embedded in the first 12 pixels of image payload)
struct TruthStamp {
    int32_t pan_counts{0};  // Thousandths of a degree (10^-3 deg)
    int32_t tilt_counts{0}; // Thousandths of a degree (10^-3 deg)
    uint32_t frame_seq{0};

    [[nodiscard]] double pan_deg() const noexcept { return pan_counts / 1000.0; }
    [[nodiscard]] double tilt_deg() const noexcept { return tilt_counts / 1000.0; }
};
static_assert(sizeof(TruthStamp) == 12, "TruthStamp must be exactly 12 bytes");

// 16-byte Control Message Wire Header
struct ControlHeader {
    uint32_t start_marker{PTCL_START_MARKER};
    uint8_t version{1};
    uint8_t type{0};
    uint16_t payload_bytes{0};
    uint32_t msg_seq{0};
    uint32_t crc32{0}; // CRC32 of bytes 0..11 followed by payload
};
static_assert(sizeof(ControlHeader) == 16, "ControlHeader must be exactly 16 bytes");

// HELLO payload (64 bytes)
struct HelloPayload {
    char camera_id[16]{0};
    char firmware_version[16]{0};
    uint16_t boot_id{0};
    uint8_t homed{0};
    uint8_t reserved{0};
    float pan_min_deg{-170.0f};
    float pan_max_deg{170.0f};
    float tilt_min_deg{-30.0f};
    float tilt_max_deg{90.0f};
    float pan_max_deg_per_s{100.0f};
    float tilt_max_deg_per_s{50.0f};
    uint32_t exposure_us{10000};
};
static_assert(sizeof(HelloPayload) == 64, "HelloPayload must be exactly 64 bytes");

// POSE payload (16 bytes, 200 Hz)
struct PosePayload {
    uint32_t counter{0};    // Camera clock in µs
    int32_t pan_counts{0};  // Tenths of a degree (0.1 deg)
    int32_t tilt_counts{0}; // Tenths of a degree (0.1 deg)
    uint8_t motion{0};      // 0 idle, 1 homing, 2 moving, 3 jogging, 4 stopping, 5 faulted
    uint8_t homed{0};
    uint8_t fault{0};       // 0 none, 1 pan stall, 2 tilt stall
    uint8_t reserved{0};

    [[nodiscard]] double pan_deg() const noexcept { return pan_counts / 10.0; }
    [[nodiscard]] double tilt_deg() const noexcept { return tilt_counts / 10.0; }
};
static_assert(sizeof(PosePayload) == 16, "PosePayload must be exactly 16 bytes");

// TIME_RESP payload (16 bytes)
struct TimeRespPayload {
    uint64_t echo{0};
    uint32_t t2{0}; // Camera clock when request arrived (µs)
    uint32_t t3{0}; // Camera clock when reply sent (µs)
};
static_assert(sizeof(TimeRespPayload) == 16, "TimeRespPayload must be exactly 16 bytes");

// TIME_REQ payload (8 bytes)
struct TimeReqPayload {
    uint64_t echo{0}; // Server timestamp in µs
};
static_assert(sizeof(TimeReqPayload) == 8, "TimeReqPayload must be exactly 8 bytes");

// CMD payload (16 bytes)
struct CmdPayload {
    uint32_t cmd_id{0};
    uint8_t cmd{0}; // 1 home, 2 go to, 3 jog, 4 stop, 5 clear fault
    uint8_t reserved[3]{0};
    float a{0.0f};  // Pan in deg for goto, or speed in deg/s for jog
    float b{0.0f};  // Tilt in deg for goto, or speed in deg/s for jog
};
static_assert(sizeof(CmdPayload) == 16, "CmdPayload must be exactly 16 bytes");

// ACK payload (8 bytes)
struct AckPayload {
    uint32_t cmd_id{0};
    uint8_t result{0}; // 0 accepted, 1 not homed, 2 out of limits, 3 faulted, 4 invalid
    uint8_t duplicate{0};
    uint16_t reserved{0};
};
static_assert(sizeof(AckPayload) == 8, "AckPayload must be exactly 8 bytes");

// STATUS payload (24 bytes, 2 Hz)
struct StatusPayload {
    uint16_t boot_id{0};
    uint16_t fps{25};
    uint32_t frames_captured{0};
    uint32_t frames_sent{0};
    uint32_t frames_dropped{0};
    uint16_t width{0};
    uint16_t height{0};
    uint32_t reserved{0};
};
static_assert(sizeof(StatusPayload) == 24, "StatusPayload must be exactly 24 bytes");

#pragma pack(pop)

} // namespace peripheral
