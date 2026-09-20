#pragma once

#include <string>
#include <vector>

namespace gt {

struct VirtualDisplayInfo {
    int driver_index = -1;
    std::wstring device_name;
    std::wstring device_id;
    bool active = false;
};

struct ConfiguredDisplay {
    int driver_index = -1;
    std::wstring device_name;
    int width = 0;
    int height = 0;
    int refresh_hz = 0;
    int x = 0;
    int y = 0;
};

int parse_vdd_driver_index(const std::wstring& device_id);
int choose_refresh_rate(const std::vector<int>& available, int preferred);
std::vector<VirtualDisplayInfo> enumerate_virtual_displays();
bool wait_for_virtual_displays(const std::vector<int>& driver_indices, int timeout_ms,
                               std::vector<VirtualDisplayInfo>& displays, std::string& error);
bool configure_virtual_displays(const std::vector<int>& driver_indices, int width, int height,
                                int preferred_hz, std::vector<ConfiguredDisplay>& configured,
                                std::string& error);

}  // namespace gt
