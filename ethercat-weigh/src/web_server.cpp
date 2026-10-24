#include "web_server.hpp"

#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_set>

#include "crow.h"
#include "log.hpp"

namespace ew {

namespace {

/// @name HTTP-Statuscodes (RFC 9110)
/// @{
constexpr int kHttpOk = 200;          ///< OK
constexpr int kHttpBadRequest = 400;  ///< ungültige Anfrage
constexpr int kHttpNotFound = 404;    ///< nicht gefunden
constexpr int kHttpConflict = 409;    ///< Kommando im aktuellen Zustand nicht ausführbar
/// @}

/// Cache-Dauer statischer Dateien des Frontends [s] (1 Tag)
constexpr int kStaticMaxAgeS = 24 * 60 * 60;

/**
 * @brief JSON-Antwort ohne Caching.
 * @param code HTTP-Statuscode
 * @param j Antwortkörper
 * @return Crow-Antwort mit Content-Type application/json
 */
crow::response json_response(int code, const nlohmann::json& j) {
    crow::response r(code, j.dump());
    r.set_header("Content-Type", "application/json");
    r.set_header("Cache-Control", "no-store");
    return r;
}

/**
 * @brief Content-Type anhand der Dateiendung.
 * @param path Dateipfad
 * @return MIME-Typ, application/octet-stream bei unbekannter Endung
 */
std::string mime_type(const std::string& path) {
    auto ends = [&](const char* ext) {
        const std::string e(ext);
        return path.size() >= e.size() && path.compare(path.size() - e.size(), e.size(), e) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js") || ends(".mjs")) return "text/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".json")) return "application/json";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".ico")) return "image/x-icon";
    if (ends(".woff2")) return "font/woff2";
    return "application/octet-stream";
}

/**
 * @brief Liefert eine Datei des statischen Frontends aus.
 *
 * Pfade mit ".." oder führendem "/" werden abgewiesen. Unbekannte Pfade ohne
 * Dateiendung liefern index.html (SPA-Fallback).
 *
 * @param root Frontend-Verzeichnis (http.web_root)
 * @param rel Pfad relativ zu @p root; leer = index.html
 * @return Datei, kHttpBadRequest oder kHttpNotFound
 */
crow::response serve_file(const std::string& root, std::string rel) {
    if (rel.empty()) rel = "index.html";
    if (rel.find("..") != std::string::npos || rel.front() == '/')
        return crow::response(kHttpBadRequest);
    std::ifstream f(root + "/" + rel, std::ios::binary);
    if (!f) {
        // SPA-Fallback: unbekannte Pfade ohne Dateiendung -> index.html
        if (rel.find('.') == std::string::npos) return serve_file(root, "index.html");
        return crow::response(kHttpNotFound);
    }
    std::ostringstream os;
    os << f.rdbuf();
    crow::response r(kHttpOk, os.str());
    r.set_header("Content-Type", mime_type(rel));
    r.set_header("Cache-Control",
                 rel == "index.html" ? "no-cache" : "public, max-age=" + std::to_string(kStaticMaxAgeS));
    return r;
}

/// Leitet Crow-Meldungen in das eigene Logging um (Level bleibt erhalten).
class CrowLogHandler : public crow::ILogHandler {
public:
    void log(std::string message, crow::LogLevel level) override {
        switch (level) {
        case crow::LogLevel::Debug: log_debug("crow: %s", message.c_str()); break;
        case crow::LogLevel::Info: log_info("crow: %s", message.c_str()); break;
        case crow::LogLevel::Warning: log_warn("crow: %s", message.c_str()); break;
        default: log_error("crow: %s", message.c_str()); break;
        }
    }
};

/// Offene WebSocket-Verbindungen.
struct WsHub {
    std::mutex m;                                            ///< schützt conns
    std::unordered_set<crow::websocket::connection*> conns;  ///< offene Verbindungen

    /// Sendet @p msg an alle offenen Verbindungen.
    void broadcast(const std::string& msg) {
        std::lock_guard<std::mutex> lk(m);
        for (auto* c : conns) c->send_text(msg);
    }
};

} // namespace

/**
 * @brief HTTP-Server (Crow): REST-API, WebSocket /ws und statisches Frontend.
 *
 * Registriert sich als Publisher der Acquisition und verteilt deren Ereignisse
 * an alle WebSocket-Clients. Blockiert bis SIGINT/SIGTERM.
 *
 * @param acq Acquisition (Datenquelle und Kommandoziel)
 * @param http Adresse, Port, Threads und Frontend-Verzeichnis
 */
void run_web_server(Acquisition& acq, const HttpConfig& http) {
    static CrowLogHandler crow_log;
    crow::logger::setHandler(&crow_log);
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);
    WsHub hub;

    acq.set_publisher([&hub](const std::string& msg) { hub.broadcast(msg); });

    // --- REST ------------------------------------------------------------------
    CROW_ROUTE(app, "/api/info")([&] { return json_response(kHttpOk, acq.info()); });

    CROW_ROUTE(app, "/api/state")([&] { return json_response(kHttpOk, acq.snapshot()); });

    CROW_ROUTE(app, "/api/config").methods(crow::HTTPMethod::Get)([&] {
        return json_response(kHttpOk, nlohmann::json(acq.config()));
    });

    CROW_ROUTE(app, "/api/config").methods(crow::HTTPMethod::Put)([&](const crow::request& req) {
        Config next;
        try {
            next = config_from_json(nlohmann::json::parse(req.body));
        } catch (const std::exception& ex) {
            return json_response(kHttpBadRequest, {{"ok", false}, {"errors", {std::string(ex.what())}}});
        }
        const auto r = acq.update_config(next);
        return json_response(r.ok ? kHttpOk : kHttpBadRequest, {{"ok", r.ok},
                                                {"restart_required", r.restart_required},
                                                {"errors", r.errors}});
    });

    CROW_ROUTE(app, "/api/measurements")([&] { return json_response(kHttpOk, acq.measurements_json()); });

    CROW_ROUTE(app, "/api/measurements.csv")([&](const crow::request& req) {
        const char* u = req.url_params.get("unit");
        crow::response r(kHttpOk, acq.measurements_csv(u ? u : "g"));
        r.set_header("Content-Type", "text/csv; charset=utf-8");
        r.set_header("Content-Disposition", "attachment; filename=\"measurements.csv\"");
        return r;
    });

    CROW_ROUTE(app, "/api/measurements/<uint>").methods(crow::HTTPMethod::Delete)([&](uint64_t id) {
        if (!acq.delete_measurement(id))
            return json_response(kHttpNotFound, {{"ok", false}, {"error", "Messung nicht gefunden"}});
        return json_response(kHttpOk, {{"ok", true}});
    });

    CROW_ROUTE(app, "/api/statistics/reset").methods(crow::HTTPMethod::Post)([&] {
        acq.reset_statistics();
        return json_response(kHttpOk, {{"ok", true}});
    });

    auto result = [](const CalResult& r) {
        return json_response(r.ok ? kHttpOk : kHttpConflict, {{"ok", r.ok}, {"error", r.error}});
    };

    CROW_ROUTE(app, "/api/tare").methods(crow::HTTPMethod::Post)([&] { return result(acq.tare()); });

    CROW_ROUTE(app, "/api/tare/clear").methods(crow::HTTPMethod::Post)([&] {
        return result(acq.clear_tare());
    });

    // Kalibrierung: {"action": "...", ...} siehe README
    CROW_ROUTE(app, "/api/calibration").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(req.body);
        } catch (const std::exception& ex) {
            return json_response(kHttpBadRequest, {{"ok", false}, {"error", ex.what()}});
        }
        return result(acq.calibration(j));
    });

    // Nur Simulation: {"grams": <zahl>} oder {"grams": null} für das automatische Profil
    CROW_ROUTE(app, "/api/sim/load").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
        std::optional<double> g;
        try {
            const auto j = nlohmann::json::parse(req.body);
            if (j.contains("grams") && j["grams"].is_number()) g = j["grams"].get<double>();
        } catch (const std::exception& ex) {
            return json_response(kHttpBadRequest, {{"ok", false}, {"error", ex.what()}});
        }
        return result(acq.set_sim_load(g));
    });

    // --- Debug (PDOs, Initialisierungssequenz) -----------------------------------
    CROW_ROUTE(app, "/api/debug/pdos")([&] {
        const auto j = acq.debug_pdos();
        return json_response(j.value("ok", false) ? kHttpOk : kHttpConflict, j);
    });

    // {"sub": <0x7000-Subindex>, "value": true|false|null} oder {"clear": true}
    CROW_ROUTE(app, "/api/debug/override").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
        std::optional<uint8_t> sub;
        std::optional<bool> value;
        try {
            const auto j = nlohmann::json::parse(req.body);
            if (!j.value("clear", false)) {
                const int s = j.at("sub").get<int>();
                if (s < 0 || s > std::numeric_limits<uint8_t>::max())
                    throw std::runtime_error("sub außerhalb des Wertebereichs");
                sub = static_cast<uint8_t>(s);
                if (j.contains("value") && j["value"].is_boolean()) value = j["value"].get<bool>();
            }
        } catch (const std::exception& ex) {
            return json_response(kHttpBadRequest, {{"ok", false}, {"error", ex.what()}});
        }
        return result(acq.debug_override(sub, value));
    });

    // {"op": "read"|"write", "index", "sub", "type": "bool|u8|u16|u32|u64|i8|i16|i32|f32", "value"}
    CROW_ROUTE(app, "/api/debug/sdo").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
        nlohmann::json j;
        try {
            j = acq.debug_sdo(nlohmann::json::parse(req.body));
        } catch (const std::exception& ex) {
            return json_response(kHttpBadRequest, {{"ok", false}, {"error", ex.what()}});
        }
        return json_response(j.value("ok", false) ? kHttpOk : kHttpConflict, j);
    });

    CROW_ROUTE(app, "/api/debug/init")([&] { return json_response(kHttpOk, acq.debug_init_sequence()); });

    CROW_ROUTE(app, "/api/debug/reinit").methods(crow::HTTPMethod::Post)([&] {
        acq.reinit();
        return json_response(kHttpOk, {{"ok", true}});
    });

    // --- WebSocket -------------------------------------------------------------
    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen([&](crow::websocket::connection& conn) {
            // Erste Nachricht nach dem Connect: vollständiger Zustand zur Initialisierung des Clients
            nlohmann::json initial_state = {{"type", "init"},
                                            {"snapshot", acq.snapshot()},
                                            {"config", acq.config()}};
            std::lock_guard<std::mutex> lk(hub.m);
            hub.conns.insert(&conn);
            conn.send_text(initial_state.dump());
        })
        .onclose([&](crow::websocket::connection& conn, const std::string&) {
            std::lock_guard<std::mutex> lk(hub.m);
            hub.conns.erase(&conn);
        })
        .onmessage([](crow::websocket::connection&, const std::string&, bool) {});

    // --- Statisches Frontend ---------------------------------------------------
    const std::string root = http.web_root;
    CROW_ROUTE(app, "/")([root] { return serve_file(root, "index.html"); });
    CROW_ROUTE(app, "/<path>")([root](const std::string& p) { return serve_file(root, p); });

    log_info("HTTP %s:%u, Frontend aus %s", http.bind.c_str(), http.port, root.c_str());
    app.bindaddr(http.bind).port(http.port).concurrency(http.threads).run();

    acq.set_publisher(nullptr);
}

} // namespace ew
