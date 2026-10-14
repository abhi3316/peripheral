#pragma once

#include "peripheral/memory/stream_buffer.hpp"
#include "peripheral/protocol/crc32.hpp"
#include "peripheral/protocol/protocol.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace peripheral {

class ControlFramer {
public:
    using HelloCallback = std::function<void(const HelloPayload&)>;
    using PoseCallback = std::function<void(const PosePayload&)>;
    using TimeRespCallback = std::function<void(const TimeRespPayload&)>;
    using AckCallback = std::function<void(const AckPayload&)>;
    using StatusCallback = std::function<void(const StatusPayload&)>;

    explicit ControlFramer(size_t buffer_capacity = 64 * 1024);
    ~ControlFramer() = default;

    // Push new raw TCP bytes received from control connection
    void push_data(std::span<const uint8_t> data);

    // Parse all complete control messages currently available in buffer
    size_t parse_available();

    // Callback setters
    void set_hello_callback(HelloCallback cb);
    void set_pose_callback(PoseCallback cb);
    void set_time_resp_callback(TimeRespCallback cb);
    void set_ack_callback(AckCallback cb);
    void set_status_callback(StatusCallback cb);

    // Outgoing packet serialization helpers
    static std::vector<uint8_t> serialize_time_req(uint64_t echo, uint32_t msg_seq);
    static std::vector<uint8_t> serialize_cmd(uint32_t cmd_id, CommandType cmd, float a, float b, uint32_t msg_seq);

    void reset();

private:
    StreamBuffer buffer_;

    HelloCallback on_hello_;
    PoseCallback on_pose_;
    TimeRespCallback on_time_resp_;
    AckCallback on_ack_;
    StatusCallback on_status_;
};

} // namespace peripheral
