#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "calibration_controller.hpp"
#include "io_backend.hpp"

namespace ew {

/**
 * @brief Executes commands of the device description (tare, restore, …).
 *
 * Used exclusively in the acquisition thread. Commands via 0xFB00:01 are
 * polled cyclically via 0xFB00:02 (step()) without blocking the process data cycle;
 * afterwards 0xFB00:01 is reset to kCmdNone.
 */
class DeviceCommandRunner {
public:
    static constexpr double kTimeoutS = 30.0;  ///< time until a command is aborted [s]

    /// true while a command is waiting for completion
    [[nodiscard]] bool active() const { return !id_.empty(); }
    /// identifier of the running command, empty without a command
    [[nodiscard]] const std::string& id() const { return id_; }

    CalResult start(const std::string& id, IoBackend& io, double t, bool& finished);
    std::optional<CalResult> step(IoBackend& io, double t);

private:
    std::string id_;       ///< running command
    double started_ = 0.0; ///< start time [s]
};

nlohmann::json read_diagnostics(IoBackend& io);

} // namespace ew
