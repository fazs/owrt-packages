#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "acquisition.hpp"
#include "config.hpp"
#include "ethercat_backend.hpp"
#include "log.hpp"
#include "sim_backend.hpp"
#include "web_server.hpp"

namespace {

/// @brief Gibt die Aufrufsyntax auf stdout aus.
void usage() {
    std::cout << "ethercat-weigh " EW_VERSION "\n"
                 "Usage: ethercat-weigh [-c <config.json>] [--sim] [--syslog] [--write-default-config <pfad>]\n"
                 "  -c, --config   Konfigurationsdatei (Default: /etc/ethercat-weigh/config.json)\n"
                 "  --sim          Simulation statt EtherCAT (ohne Hardware)\n"
                 "  --syslog       Logausgabe per syslog (daemon.info/warn/err) statt stderr\n"
                 "  --write-default-config <pfad>  Default-Konfiguration schreiben und beenden\n";
}

} // namespace

/**
 * @brief Einstieg: Optionen auswerten, Konfiguration laden, Acquisition und Webserver starten.
 * @return 0 bei regulärem Ende, 1 bei Konfigurationsfehler, 2 bei ungültigen Optionen
 */
int main(int argc, char** argv) {
    std::string config_path = "/etc/ethercat-weigh/config.json";
    bool sim = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "-c" || a == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--sim") {
            sim = true;
        } else if (a == "--syslog") {
            ew::log_use_syslog("ethercat-weigh");
        } else if (a == "--write-default-config" && i + 1 < argc) {
            try {
                ew::save_config(argv[++i], ew::Config{});
            } catch (const std::exception& ex) {
                std::cerr << ex.what() << '\n';
                return 1;
            }
            return 0;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }

    ew::Config cfg;
    try {
        cfg = ew::load_config(config_path);
    } catch (const std::exception& ex) {
        ew::log_error(ex.what());
        return 1;
    }

    std::unique_ptr<ew::IoBackend> backend;
    if (sim)
        backend = std::make_unique<ew::SimBackend>();
    else
        backend = std::make_unique<ew::EthercatBackend>();

    ew::log_info("ethercat-weigh %s, Backend %s, Config %s", EW_VERSION, backend->name().c_str(),
                 config_path.c_str());

    ew::Acquisition acq(std::move(backend), cfg, config_path);
    acq.start();
    ew::run_web_server(acq, cfg.http);  // blockiert bis SIGINT/SIGTERM
    acq.stop();
    ew::log_info("beendet");
    return 0;
}
