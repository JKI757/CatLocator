#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace catlocator {

struct Location {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

struct BeaconReading {
    std::string beacon_id;
    std::string tag_id;
    std::int32_t rssi{0};
    std::string timestamp;  // ISO8601 UTC string
    Location beacon_location;
    std::map<std::string, std::string> metadata;
};

}  // namespace catlocator

