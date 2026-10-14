#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace peripheral {

/**
 * @brief Describes the network endpoints and identifiers for a physical or simulated camera rig.
 */
struct CameraEndpoint {
    std::string name;
    std::string host{"127.0.0.1"};
    uint16_t control_port{7001};
    uint16_t video_port{7002};

    // Helper returning standard simulator default endpoints for cam0 and cam1
    static std::vector<CameraEndpoint> default_endpoints() {
        return {
            CameraEndpoint{
                .name = "cam0",
                .host = "127.0.0.1",
                .control_port = 7001,
                .video_port = 7002
            },
            CameraEndpoint{
                .name = "cam1",
                .host = "127.0.0.1",
                .control_port = 7011,
                .video_port = 7012
            }
        };
    }
};

} // namespace peripheral
