#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "calibration_controller.hpp"
#include "io_backend.hpp"

namespace ew {

/**
 * @brief Führt Kommandos der Gerätebeschreibung aus (Tara, Restore, …).
 *
 * Wird ausschließlich im Acquisition-Thread verwendet. Kommandos über 0xFB00:01 werden
 * zyklisch über 0xFB00:02 abgefragt (step()), ohne den Prozessdatenzyklus zu blockieren;
 * danach wird 0xFB00:01 auf kCmdNone zurückgesetzt.
 */
class DeviceCommandRunner {
public:
    static constexpr double kTimeoutS = 30.0;  ///< Zeit bis zum Abbruch eines Kommandos [s]

    /// true, solange ein Kommando auf seinen Abschluss wartet
    [[nodiscard]] bool active() const { return !id_.empty(); }
    /// Kennung des laufenden Kommandos, leer ohne Kommando
    [[nodiscard]] const std::string& id() const { return id_; }

    CalResult start(const std::string& id, IoBackend& io, double t, bool& finished);
    std::optional<CalResult> step(IoBackend& io, double t);

private:
    std::string id_;       ///< laufendes Kommando
    double started_ = 0.0; ///< Startzeit [s]
};

nlohmann::json read_diagnostics(IoBackend& io);

} // namespace ew
