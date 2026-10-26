#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "acquisition.hpp"
#include "config.hpp"
#include "ethercat_backend.hpp"
#include "log.hpp"
#include "measurement_csv.hpp"
#include "sim_backend.hpp"
#include "web_server.hpp"

namespace {

constexpr int kExitOk = EXIT_SUCCESS;     ///< reguläres Ende
constexpr int kExitError = EXIT_FAILURE;  ///< Konfigurations- oder Schreibfehler
constexpr int kExitUsage = 2;             ///< ungültige Optionen (Konvention der Shell-Werkzeuge)

/// @brief Gibt die Aufrufsyntax auf stdout aus.
void usage() {
    std::cout << "ethercat-weigh " EW_VERSION "\n"
                 "Usage: ethercat-weigh [-c <config.json>] [--sim] [--sim-csv <datei>] [--syslog]\n"
                 "                      [--write-default-config <pfad>] [--write-openapi <pfad>]\n"
                 "  -c, --config   Konfigurationsdatei (Default: /etc/ethercat-weigh/config.json)\n"
                 "  --sim          Simulation statt EtherCAT (ohne Hardware)\n"
                 "  --sim-csv <datei>  Simulation; Messungen aus CSV (Format des Exports oder\n"
                 "                 ein Wert in g je Zeile) beim Start übernehmen\n"
                 "  --syslog       Logausgabe per syslog (daemon.info/warn/err) statt stderr\n"
                 "  --write-default-config <pfad>  Default-Konfiguration schreiben und beenden\n"
                 "  --write-openapi <pfad>  OpenAPI-Spec der REST-API schreiben und beenden\n";
}

} // namespace

/**
 * @brief Einstieg: Optionen auswerten, Konfiguration laden, Acquisition und Webserver starten.
 * @return kExitOk, kExitError bei Konfigurationsfehler, kExitUsage bei ungültigen Optionen
 */
int main(int argc, char** argv) {
    std::string config_path = "/etc/ethercat-weigh/config.json";
    bool sim = false;
    std::string sim_csv;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "-c" || a == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--sim") {
            sim = true;
        } else if (a == "--sim-csv" && i + 1 < argc) {
            sim = true;
            sim_csv = argv[++i];
        } else if (a == "--syslog") {
            ew::log_use_syslog("ethercat-weigh");
        } else if (a == "--write-default-config" && i + 1 < argc) {
            try {
                ew::save_config(argv[++i], ew::Config{});
            } catch (const std::exception& ex) {
                std::cerr << ex.what() << '\n';
                return kExitError;
            }
            return kExitOk;
        } else if (a == "--write-openapi" && i + 1 < argc) {
            const char* path = argv[++i];
            std::ofstream f(path);
            if (!(f << ew::openapi_spec().dump(2) << '\n')) {
                std::cerr << "Schreiben fehlgeschlagen: " << path << '\n';
                return kExitError;
            }
            return kExitOk;
        } else if (a == "-h" || a == "--help") {
            usage();
            return kExitOk;
        } else {
            usage();
            return kExitUsage;
        }
    }

    ew::Config cfg;
    try {
        cfg = ew::load_config(config_path);
    } catch (const std::exception& ex) {
        ew::log_error(ex.what());
        return kExitError;
    }

    std::unique_ptr<ew::IoBackend> backend;
    if (sim)
        backend = std::make_unique<ew::SimBackend>();
    else
        backend = std::make_unique<ew::EthercatBackend>();

    ew::log_info("ethercat-weigh %s, Backend %s, Config %s", EW_VERSION, backend->name().c_str(),
                 config_path.c_str());

    ew::Acquisition acq(std::move(backend), cfg, config_path);
    if (!sim_csv.empty()) {
        std::ifstream f(sim_csv);
        if (!f) {
            ew::log_error("CSV nicht lesbar: %s", sim_csv.c_str());
            return kExitError;
        }
        try {
            const auto ms = ew::parse_measurements_csv(f);
            const size_t n = acq.import_measurements(ms);
            ew::log_info("%zu von %zu Messungen aus %s übernommen", n, ms.size(), sim_csv.c_str());
        } catch (const std::exception& ex) {
            ew::log_error("%s: %s", sim_csv.c_str(), ex.what());
            return kExitError;
        }
    }
    acq.start();
    ew::run_web_server(acq, cfg.http);  // blockiert bis SIGINT/SIGTERM
    acq.stop();
    ew::log_info("beendet");
    return kExitOk;
}
