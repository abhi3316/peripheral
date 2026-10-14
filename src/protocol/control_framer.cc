#include "peripheral/protocol/control_framer.hpp"

#include <cstring>

namespace peripheral {

ControlFramer::ControlFramer(size_t buffer_capacity)
    : buffer_(buffer_capacity) {}

void ControlFramer::push_data(std::span<const uint8_t> data) {
    buffer_.append(data);
}

void ControlFramer::set_hello_callback(HelloCallback cb) { on_hello_ = std::move(cb); }
void ControlFramer::set_pose_callback(PoseCallback cb) { on_pose_ = std::move(cb); }
void ControlFramer::set_time_resp_callback(TimeRespCallback cb) { on_time_resp_ = std::move(cb); }
void ControlFramer::set_ack_callback(AckCallback cb) { on_ack_ = std::move(cb); }
void ControlFramer::set_status_callback(StatusCallback cb) { on_status_ = std::move(cb); }

size_t ControlFramer::parse_available() {
    size_t messages_parsed = 0;

    while (buffer_.readable_bytes() >= sizeof(ControlHeader)) {
        // Step 1: Scan for PTCL marker (0x4C435450)
        auto marker_offset = buffer_.find_marker(PTCL_START_MARKER);
        if (!marker_offset.has_value()) {
            size_t available = buffer_.readable_bytes();
            if (available > 3) {
                buffer_.consume(available - 3);
            }
            break;
        }

        if (marker_offset.value() > 0) {
            buffer_.consume(marker_offset.value());
        }

        if (buffer_.readable_bytes() < sizeof(ControlHeader)) {
            break;
        }

        auto readable = buffer_.readable_span();
        auto header_span = readable.subspan(0, sizeof(ControlHeader));

        ControlHeader header{};
        std::memcpy(&header, header_span.data(), sizeof(ControlHeader));

        if (header.version != 1) {
            buffer_.consume(1);
            continue;
        }

        size_t total_msg_bytes = sizeof(ControlHeader) + header.payload_bytes;
        if (buffer_.readable_bytes() < total_msg_bytes) {
            break; // Wait for full payload to arrive
        }

        // Step 2: Validate CRC-32 (header bytes 0..11 + payload)
        auto payload_span = readable.subspan(sizeof(ControlHeader), header.payload_bytes);

        // Compute CRC over header[0..11] then continue over payload
        uint32_t calc_crc = crc32(header_span.subspan(0, 12));
        calc_crc = crc32(payload_span, calc_crc);

        if (calc_crc != header.crc32) {
            // Bad checksum, advance by 1 byte and scan again
            buffer_.consume(1);
            continue;
        }

        // Step 3: Dispatch typed payload
        switch (static_cast<ControlMsgType>(header.type)) {
            case ControlMsgType::Hello: {
                if (header.payload_bytes >= sizeof(HelloPayload) && on_hello_) {
                    HelloPayload payload{};
                    std::memcpy(&payload, payload_span.data(), sizeof(HelloPayload));
                    on_hello_(payload);
                }
                break;
            }
            case ControlMsgType::Pose: {
                if (header.payload_bytes >= sizeof(PosePayload) && on_pose_) {
                    PosePayload payload{};
                    std::memcpy(&payload, payload_span.data(), sizeof(PosePayload));
                    on_pose_(payload);
                }
                break;
            }
            case ControlMsgType::TimeResp: {
                if (header.payload_bytes >= sizeof(TimeRespPayload) && on_time_resp_) {
                    TimeRespPayload payload{};
                    std::memcpy(&payload, payload_span.data(), sizeof(TimeRespPayload));
                    on_time_resp_(payload);
                }
                break;
            }
            case ControlMsgType::Ack: {
                if (header.payload_bytes >= sizeof(AckPayload) && on_ack_) {
                    AckPayload payload{};
                    std::memcpy(&payload, payload_span.data(), sizeof(AckPayload));
                    on_ack_(payload);
                }
                break;
            }
            case ControlMsgType::Status: {
                if (header.payload_bytes >= sizeof(StatusPayload) && on_status_) {
                    StatusPayload payload{};
                    std::memcpy(&payload, payload_span.data(), sizeof(StatusPayload));
                    on_status_(payload);
                }
                break;
            }
            default:
                break;
        }

        buffer_.consume(total_msg_bytes);
        messages_parsed++;
    }

    return messages_parsed;
}

std::vector<uint8_t> ControlFramer::serialize_time_req(uint64_t echo, uint32_t msg_seq) {
    ControlHeader hdr{};
    hdr.start_marker = PTCL_START_MARKER;
    hdr.version = 1;
    hdr.type = static_cast<uint8_t>(ControlMsgType::TimeReq);
    hdr.payload_bytes = sizeof(TimeReqPayload);
    hdr.msg_seq = msg_seq;

    TimeReqPayload payload{echo};

    // Calculate CRC32 over header[0..11] + payload
    uint8_t hdr_bytes[12];
    std::memcpy(hdr_bytes, &hdr, 12);

    uint8_t payload_bytes[sizeof(TimeReqPayload)];
    std::memcpy(payload_bytes, &payload, sizeof(TimeReqPayload));

    uint32_t c = crc32(std::span<const uint8_t>(hdr_bytes, 12));
    c = crc32(std::span<const uint8_t>(payload_bytes, sizeof(payload_bytes)), c);
    hdr.crc32 = c;

    std::vector<uint8_t> packet(sizeof(ControlHeader) + sizeof(TimeReqPayload));
    std::memcpy(packet.data(), &hdr, sizeof(ControlHeader));
    std::memcpy(packet.data() + sizeof(ControlHeader), &payload, sizeof(TimeReqPayload));
    return packet;
}

std::vector<uint8_t> ControlFramer::serialize_cmd(uint32_t cmd_id, CommandType cmd, float a, float b, uint32_t msg_seq) {
    ControlHeader hdr{};
    hdr.start_marker = PTCL_START_MARKER;
    hdr.version = 1;
    hdr.type = static_cast<uint8_t>(ControlMsgType::Cmd);
    hdr.payload_bytes = sizeof(CmdPayload);
    hdr.msg_seq = msg_seq;

    CmdPayload payload{};
    payload.cmd_id = cmd_id;
    payload.cmd = static_cast<uint8_t>(cmd);
    payload.a = a;
    payload.b = b;

    uint8_t hdr_bytes[12];
    std::memcpy(hdr_bytes, &hdr, 12);

    uint8_t payload_bytes[sizeof(CmdPayload)];
    std::memcpy(payload_bytes, &payload, sizeof(CmdPayload));

    uint32_t c = crc32(std::span<const uint8_t>(hdr_bytes, 12));
    c = crc32(std::span<const uint8_t>(payload_bytes, sizeof(payload_bytes)), c);
    hdr.crc32 = c;

    std::vector<uint8_t> packet(sizeof(ControlHeader) + sizeof(CmdPayload));
    std::memcpy(packet.data(), &hdr, sizeof(ControlHeader));
    std::memcpy(packet.data() + sizeof(ControlHeader), &payload, sizeof(CmdPayload));
    return packet;
}

void ControlFramer::reset() {
    buffer_.clear();
}

} // namespace peripheral
