#include "vr/VRCore.h"

#include "spdlog/spdlog.h"

int main() {
    while (true) {
        try {
            VRCore vRCore;
            vRCore.runVR();
        }
        catch (std::runtime_error e) {
            spdlog::critical(e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(5000));
        }
    }

    return 0;
}
