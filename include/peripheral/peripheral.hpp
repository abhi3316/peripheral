#pragma once

/**
 * @file peripheral.hpp
 * @brief Master umbrella include for the Peripheral Camera Gateway.
 *
 * Provides access to all subsystems:
 *  - transport: TcpSocket, StreamCapture, SocketStats
 *  - memory: StreamBuffer, FrameMemoryPool, FrameSlotHandle
 *  - protocol: Wire protocol (VFRM, PTCL), VideoFramer, ControlFramer, CRC32
 *  - camera: CameraEndpoint, CameraSession
 *  - web: WebServer, REST API, Operator Viewer
 */

// 1. Transport Subsystem
#include "peripheral/transport/socket_types.hpp"
#include "peripheral/transport/socket.hpp"
#include "peripheral/transport/stream_capture.hpp"

// 2. Memory Subsystem
#include "peripheral/memory/stream_buffer.hpp"
#include "peripheral/memory/frame_pool.hpp"

// 3. Protocol & Framing Subsystem
#include "peripheral/protocol/crc32.hpp"
#include "peripheral/protocol/protocol.hpp"
#include "peripheral/protocol/video_frame.hpp"
#include "peripheral/protocol/video_framer.hpp"
#include "peripheral/protocol/control_framer.hpp"

// 4. Camera Coordination Subsystem
#include "peripheral/camera/camera_endpoint.hpp"
#include "peripheral/camera/camera_session.hpp"

// 5. Web & Serving Subsystem
#include "peripheral/web/web_server.hpp"
