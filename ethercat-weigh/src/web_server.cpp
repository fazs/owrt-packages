#include "web_server.hpp"

#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_set>

#include "api_registry.hpp"
#include "crow.h"
#include "device_description.hpp"
#include "log.hpp"
#include "sim_backend.hpp"

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

/// Beschreibung der API (info.description der OpenAPI-Spec)
constexpr const char* kApiDescription =
    "REST-API von ethercat-weigh. Ereignisse (sample, measurement, config, ...) kommen über den "
    "WebSocket /ws; die erste Nachricht nach dem Verbindungsaufbau ist {\"type\": \"init\", "
    "\"snapshot\", \"config\"}.";

/// @name OpenAPI-Tags (Gruppen der Routen)
/// @{
constexpr const char* kTagState = "Zustand";
constexpr const char* kTagConfig = "Konfiguration";
constexpr const char* kTagMeasurement = "Messungen";
constexpr const char* kTagCalibration = "Kalibrierung";
constexpr const char* kTagDevice = "Klemme";
constexpr const char* kTagDebug = "Debug";
constexpr const char* kTagSim = "Simulation";
/// @}

/// @name Namen der Schema-Komponenten
/// @{
constexpr const char* kSchemaResult = "Result";
constexpr const char* kSchemaConfigResult = "ConfigResult";
constexpr const char* kSchemaConfig = "Config";
constexpr const char* kSchemaInfo = "Info";
constexpr const char* kSchemaMeasurement = "Measurement";
/// @}

/**
 * @brief Schema eines einfachen Typs.
 * @param type JSON-Schema-Typ ("boolean", "integer", "number", "string", "object")
 */
nlohmann::json schema_type(const char* type) { return {{"type", type}}; }

/**
 * @brief Schema einer Ganzzahl mit Wertebereich.
 * @param max Obergrenze (Untergrenze 0)
 */
nlohmann::json schema_uint(uint64_t max) { return {{"type", "integer"}, {"minimum", 0}, {"maximum", max}}; }

/**
 * @brief Schema eines Strings mit festen Werten.
 * @param values erlaubte Werte
 */
nlohmann::json schema_enum(const std::vector<std::string>& values) {
    return {{"type", "string"}, {"enum", values}};
}

} // namespace

/**
 * @brief Registriert alle REST-Routen mit ihrer Beschreibung.
 *
 * Antwortschemas werden, wo möglich, aus Beispielwerten von @p acq abgeleitet
 * (schema_from()), damit sie den Datenstrukturen folgen.
 *
 * @param api Registry (Crow-Anwendung)
 * @param acq Acquisition (Datenquelle und Kommandoziel)
 * @param spec OpenAPI-Spec, die GET /api/openapi.json ausliefert
 */
void register_api(ApiRegistry& api, Acquisition& acq, const nlohmann::json& spec) {
    using crow::HTTPMethod;

    const auto result = schema_ref(kSchemaResult);
    api.component(kSchemaResult, schema_object({{"ok", schema_type("boolean")}, {"error", schema_type("string")}},
                                               {"ok"}));
    api.component(kSchemaConfigResult,
                  schema_object({{"ok", schema_type("boolean")},
                                 {"restart_required", schema_type("boolean")},
                                 {"errors", {{"type", "array"}, {"items", schema_type("string")}}}},
                                {"ok", "errors"}));
    api.component(kSchemaConfig, schema_from(nlohmann::json(acq.config())));
    api.component(kSchemaInfo, schema_from(acq.info()));
    const auto measurements = acq.measurements_json();
    api.component(kSchemaMeasurement,
                  measurements.empty() ? schema_type("object") : schema_from(measurements.front()));
    nlohmann::json snapshot = schema_from(acq.snapshot());
    snapshot["properties"]["info"] = schema_ref(kSchemaInfo);
    snapshot["properties"]["measurements"] = {{"type", "array"}, {"items", schema_ref(kSchemaMeasurement)}};

    nlohmann::json command_ids = nlohmann::json::array();
    for (const auto& c : DeviceDescription::el3356_0010().json().at("commands")) command_ids.push_back(c.at("id"));
    std::vector<std::string> dtypes;
    for (const auto t : el3356::kValueTypes) dtypes.emplace_back(el3356::dtype_name(t));

    auto cal_result = [](const CalResult& r) {
        return json_response(r.ok ? kHttpOk : kHttpConflict, {{"ok", r.ok}, {"error", r.error}});
    };

    // --- Zustand und Konfiguration ---------------------------------------------
    api.add(HTTPMethod::Get, "/api/openapi.json",
            {kTagState, "Diese OpenAPI-Spec", {}, {{kHttpOk, schema_type("object")}}},
            [&spec] { return json_response(kHttpOk, spec); });

    api.add(HTTPMethod::Get, "/api/info",
            {kTagState, "Version, Backend-Zustand, Zyklen, Überläufe, max. Zykluszeit", {},
             {{kHttpOk, schema_ref(kSchemaInfo)}}},
            [&] { return json_response(kHttpOk, acq.info()); });

    api.add(HTTPMethod::Get, "/api/state",
            {kTagState, "Snapshot: letztes Sample, Statistik, letzte Messungen, info", {}, {{kHttpOk, snapshot}}},
            [&] { return json_response(kHttpOk, acq.snapshot()); });

    api.add(HTTPMethod::Get, "/api/config",
            {kTagConfig, "Gespeicherte Konfiguration", {}, {{kHttpOk, schema_ref(kSchemaConfig)}}},
            [&] { return json_response(kHttpOk, nlohmann::json(acq.config())); });

    api.add(HTTPMethod::Put, "/api/config",
            {kTagConfig, "Konfiguration validieren, speichern und anwenden", schema_ref(kSchemaConfig),
             {{kHttpOk, schema_ref(kSchemaConfigResult)}, {kHttpBadRequest, schema_ref(kSchemaConfigResult)}}},
            [&](const crow::request& req) {
                Config next;
                try {
                    next = config_from_json(nlohmann::json::parse(req.body));
                } catch (const std::exception& ex) {
                    return json_response(kHttpBadRequest, {{"ok", false}, {"errors", {std::string(ex.what())}}});
                }
                const auto r = acq.update_config(next);
                return json_response(r.ok ? kHttpOk : kHttpBadRequest,
                                     {{"ok", r.ok}, {"restart_required", r.restart_required}, {"errors", r.errors}});
            });

    // --- Messungen ---------------------------------------------------------------
    api.add(HTTPMethod::Get, "/api/measurements",
            {kTagMeasurement, "Gehaltene Messungen (Massen in g)", {},
             {{kHttpOk, {{"type", "array"}, {"items", schema_ref(kSchemaMeasurement)}}}}},
            [&] { return json_response(kHttpOk, acq.measurements_json()); });

    {
        ApiDoc doc{kTagMeasurement, "Gehaltene Messungen als CSV (Trennzeichen ';')", {},
                   {{kHttpOk, schema_type("string")}}};
        doc.query = {{"unit", schema_type("string")}};
        doc.media = "text/csv";
        api.add(HTTPMethod::Get, "/api/measurements.csv", doc, [&](const crow::request& req) {
            const char* u = req.url_params.get("unit");
            crow::response r(kHttpOk, acq.measurements_csv(u ? u : "g"));
            r.set_header("Content-Type", "text/csv; charset=utf-8");
            r.set_header("Content-Disposition", "attachment; filename=\"measurements.csv\"");
            return r;
        });
    }

    {
        ApiDoc doc{kTagMeasurement, "Messung löschen", {}, {{kHttpOk, result}, {kHttpNotFound, result}}};
        doc.params = {"id"};
        api.add(HTTPMethod::Delete, "/api/measurements/<uint>", doc, [&](uint64_t id) {
            if (!acq.delete_measurement(id))
                return json_response(kHttpNotFound, {{"ok", false}, {"error", "Messung nicht gefunden"}});
            return json_response(kHttpOk, {{"ok", true}});
        });
    }

    api.add(HTTPMethod::Post, "/api/statistics/reset",
            {kTagMeasurement, "Statistik und Messungen löschen", {}, {{kHttpOk, result}}}, [&] {
                acq.reset_statistics();
                return json_response(kHttpOk, {{"ok", true}});
            });

    // --- Tara und Kalibrierung -----------------------------------------------------
    api.add(HTTPMethod::Post, "/api/tare",
            {kTagCalibration, "Tara (Software) aus dem Mittel der letzten 0,5 s setzen", {},
             {{kHttpOk, result}, {kHttpConflict, result}}},
            [&] { return cal_result(acq.tare()); });

    api.add(HTTPMethod::Post, "/api/tare/clear",
            {kTagCalibration, "Tara (Software) löschen", {}, {{kHttpOk, result}, {kHttpConflict, result}}},
            [&] { return cal_result(acq.clear_tare()); });

    api.add(HTTPMethod::Post, "/api/calibration",
            {kTagCalibration, "Kalibrierschritt ausführen (Aktionen siehe README)",
             schema_object({{"action", schema_type("string")},
                            {"mass", schema_type("number")},
                            {"unit", schema_type("string")},
                            {"index", schema_type("integer")},
                            {"nominal_load", schema_type("number")}},
                           {"action"}),
             {{kHttpOk, result}, {kHttpBadRequest, result}, {kHttpConflict, result}}},
            [&](const crow::request& req) {
                nlohmann::json j;
                try {
                    j = nlohmann::json::parse(req.body);
                } catch (const std::exception& ex) {
                    return json_response(kHttpBadRequest, {{"ok", false}, {"error", ex.what()}});
                }
                return cal_result(acq.calibration(j));
            });

    api.add(HTTPMethod::Post, "/api/sim/load",
            {kTagSim, "Nur Simulation: Last in g auflegen; null = automatisches Profil",
             schema_object({{"grams", {{"type", {"number", "null"}}}}}),
             {{kHttpOk, result}, {kHttpBadRequest, result}, {kHttpConflict, result}}},
            [&](const crow::request& req) {
                std::optional<double> g;
                try {
                    const auto j = nlohmann::json::parse(req.body);
                    if (j.contains("grams") && j["grams"].is_number()) g = j["grams"].get<double>();
                } catch (const std::exception& ex) {
                    return json_response(kHttpBadRequest, {{"ok", false}, {"error", ex.what()}});
                }
                return cal_result(acq.set_sim_load(g));
            });

    // --- Klemme --------------------------------------------------------------------
    api.add(HTTPMethod::Get, "/api/device/description",
            {kTagDevice, "Gerätebeschreibung der Klemme (aus der ESI erzeugt)", {},
             {{kHttpOk, schema_type("object")}}},
            [] {
                crow::response r(kHttpOk, DeviceDescription::el3356_0010().text());
                r.set_header("Content-Type", "application/json");
                r.set_header("Cache-Control", "no-cache");
                return r;
            });

    api.add(HTTPMethod::Post, "/api/device/command",
            {kTagDevice, "Kommando der Gerätebeschreibung starten; Abschluss als WebSocket-Ereignis device_command",
             schema_object({{"id", {{"type", "string"}, {"enum", command_ids}}}}, {"id"}),
             {{kHttpOk, result}, {kHttpBadRequest, result}, {kHttpConflict, result}}},
            [&](const crow::request& req) {
                std::string id;
                try {
                    id = nlohmann::json::parse(req.body).at("id").get<std::string>();
                } catch (const std::exception& ex) {
                    return json_response(kHttpBadRequest, {{"ok", false}, {"error", ex.what()}});
                }
                return cal_result(acq.device_command(id));
            });

    const auto diag_item = schema_object(
        {{"key", schema_type("string")}, {"value", nlohmann::json::object()}, {"error", schema_type("string")}});
    const auto diag = schema_object({{"ok", schema_type("boolean")},
                                     {"error", schema_type("string")},
                                     {"items", {{"type", "array"}, {"items", diag_item}}}},
                                    {"ok"});
    api.add(HTTPMethod::Get, "/api/device/diag",
            {kTagDevice, "Diagnose per SDO (nur auf Anfrage)", {}, {{kHttpOk, diag}, {kHttpConflict, diag}}}, [&] {
                const auto j = acq.device_diagnostics();
                return json_response(j.value("ok", false) ? kHttpOk : kHttpConflict, j);
            });

    // --- Debug (PDOs, Initialisierungssequenz) -----------------------------------
    const auto pdo_entry = schema_object({{"index", schema_type("integer")},
                                          {"sub", schema_type("integer")},
                                          {"bits", schema_type("integer")},
                                          {"name", schema_type("string")},
                                          {"type", schema_enum(dtypes)},
                                          {"offset", {{"type", {"integer", "null"}}}},
                                          {"value", nlohmann::json::object()},
                                          {"override", {{"type", {"boolean", "null"}}}}});
    const auto pdo = schema_object({{"index", schema_type("integer")},
                                    {"name", schema_type("string")},
                                    {"rx", schema_type("boolean")},
                                    {"assigned", schema_type("boolean")},
                                    {"excludes", {{"type", "array"}, {"items", schema_type("integer")}}},
                                    {"entries", {{"type", "array"}, {"items", pdo_entry}}}});
    const auto pdos = schema_object({{"ok", schema_type("boolean")},
                                     {"error", schema_type("string")},
                                     {"pdos", {{"type", "array"}, {"items", pdo}}},
                                     {"inputs", schema_type("string")},
                                     {"outputs", schema_type("string")}},
                                    {"ok"});
    api.add(HTTPMethod::Get, "/api/debug/pdos",
            {kTagDebug, "PDO-Katalog mit Zuordnung, Bitlage, Werten, Übersteuerungen und Prozessabbild (Hex)", {},
             {{kHttpOk, pdos}, {kHttpConflict, pdos}}},
            [&] {
                const auto j = acq.debug_pdos();
                return json_response(j.value("ok", false) ? kHttpOk : kHttpConflict, j);
            });

    api.add(HTTPMethod::Post, "/api/debug/override",
            {kTagDebug, "Steuerbit 0x7000 übersteuern; value null hebt auf, clear hebt alle auf",
             schema_object({{"sub", schema_uint(std::numeric_limits<uint8_t>::max())},
                            {"value", {{"type", {"boolean", "null"}}}},
                            {"clear", schema_type("boolean")}}),
             {{kHttpOk, result}, {kHttpBadRequest, result}, {kHttpConflict, result}}},
            [&](const crow::request& req) {
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
                return cal_result(acq.debug_override(sub, value));
            });

    const auto sdo_result = schema_object({{"ok", schema_type("boolean")},
                                           {"error", schema_type("string")},
                                           {"value", {{"type", {"number", "boolean"}}}},
                                           {"hex", schema_type("string")}},
                                          {"ok"});
    api.add(HTTPMethod::Post, "/api/debug/sdo",
            {kTagDebug, "Einzelner SDO-Zugriff",
             schema_object({{"op", schema_enum({"read", "write"})},
                            {"index", schema_uint(std::numeric_limits<uint16_t>::max())},
                            {"sub", schema_uint(std::numeric_limits<uint8_t>::max())},
                            {"type", schema_enum(dtypes)},
                            {"value", {{"type", {"number", "boolean"}}}}},
                           {"op", "index", "sub", "type"}),
             {{kHttpOk, sdo_result}, {kHttpBadRequest, result}, {kHttpConflict, sdo_result}}},
            [&](const crow::request& req) {
                nlohmann::json j;
                try {
                    j = acq.debug_sdo(nlohmann::json::parse(req.body));
                } catch (const std::exception& ex) {
                    return json_response(kHttpBadRequest, {{"ok", false}, {"error", ex.what()}});
                }
                return json_response(j.value("ok", false) ? kHttpOk : kHttpConflict, j);
            });

    api.add(HTTPMethod::Get, "/api/debug/init",
            {kTagDebug, "Initialisierungssequenz mit den Werten der aktuellen Konfiguration", {},
             {{kHttpOk, schema_from(acq.debug_init_sequence())}}},
            [&] { return json_response(kHttpOk, acq.debug_init_sequence()); });

    api.add(HTTPMethod::Post, "/api/debug/reinit",
            {kTagDebug, "Prozessdatenquelle schließen und neu initialisieren", {}, {{kHttpOk, result}}}, [&] {
                acq.reinit();
                return json_response(kHttpOk, {{"ok", true}});
            });
}

/**
 * @brief OpenAPI-Spec der REST-API.
 *
 * Unabhängig vom laufenden Betrieb: Die Routen werden an einer eigenen Crow-Anwendung mit
 * einer nicht gestarteten Simulation (Default-Konfiguration, eine Beispielmessung) registriert.
 * Endpoint und --write-openapi liefern dadurch dasselbe Dokument.
 *
 * @return OpenAPI 3.1
 */
nlohmann::json openapi_spec() {
    crow::SimpleApp app;
    ApiRegistry api(app);
    Acquisition acq(std::make_unique<SimBackend>(), Config{}, "");
    acq.import_measurements({Measurement{}});
    const nlohmann::json unused;
    register_api(api, acq, unused);
    return api.openapi(EW_VERSION, kApiDescription);
}

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
    const nlohmann::json spec = openapi_spec();
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);
    WsHub hub;

    acq.set_publisher([&hub](const std::string& msg) { hub.broadcast(msg); });

    ApiRegistry api(app);
    register_api(api, acq, spec);

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
