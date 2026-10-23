#include "web_server.hpp"

#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_set>

#include "crow.h"
#include "log.hpp"

namespace ew {

namespace {

crow::response json_response(int code, const nlohmann::json& j) {
    crow::response r(code, j.dump());
    r.set_header("Content-Type", "application/json");
    r.set_header("Cache-Control", "no-store");
    return r;
}

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

crow::response serve_file(const std::string& root, std::string rel) {
    if (rel.empty()) rel = "index.html";
    if (rel.find("..") != std::string::npos || rel.front() == '/')
        return crow::response(400);
    std::ifstream f(root + "/" + rel, std::ios::binary);
    if (!f) {
        // SPA-Fallback: unbekannte Pfade ohne Dateiendung -> index.html
        if (rel.find('.') == std::string::npos) return serve_file(root, "index.html");
        return crow::response(404);
    }
    std::ostringstream os;
    os << f.rdbuf();
    crow::response r(200, os.str());
    r.set_header("Content-Type", mime_type(rel));
    r.set_header("Cache-Control", rel == "index.html" ? "no-cache" : "public, max-age=86400");
    return r;
}

struct WsHub {
    std::mutex m;
    std::unordered_set<crow::websocket::connection*> conns;

    void broadcast(const std::string& msg) {
        std::lock_guard<std::mutex> lk(m);
        for (auto* c : conns) c->send_text(msg);
    }
};

} // namespace

void run_web_server(Acquisition& acq, const HttpConfig& http) {
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);
    WsHub hub;

    acq.set_publisher([&hub](const std::string& msg) { hub.broadcast(msg); });

    // --- REST ------------------------------------------------------------------
    CROW_ROUTE(app, "/api/info")([&] { return json_response(200, acq.info()); });

    CROW_ROUTE(app, "/api/state")([&] { return json_response(200, acq.snapshot()); });

    CROW_ROUTE(app, "/api/config").methods(crow::HTTPMethod::Get)([&] {
        return json_response(200, nlohmann::json(acq.config()));
    });

    CROW_ROUTE(app, "/api/config").methods(crow::HTTPMethod::Put)([&](const crow::request& req) {
        Config next;
        try {
            next = config_from_json(nlohmann::json::parse(req.body));
        } catch (const std::exception& ex) {
            return json_response(400, {{"ok", false}, {"errors", {std::string(ex.what())}}});
        }
        const auto r = acq.update_config(next);
        return json_response(r.ok ? 200 : 400, {{"ok", r.ok},
                                                {"restart_required", r.restart_required},
                                                {"errors", r.errors}});
    });

    CROW_ROUTE(app, "/api/measurements")([&] { return json_response(200, acq.measurements_json()); });

    CROW_ROUTE(app, "/api/measurements.csv")([&](const crow::request& req) {
        const char* u = req.url_params.get("unit");
        crow::response r(200, acq.measurements_csv(u ? u : "g"));
        r.set_header("Content-Type", "text/csv; charset=utf-8");
        r.set_header("Content-Disposition", "attachment; filename=\"measurements.csv\"");
        return r;
    });

    CROW_ROUTE(app, "/api/statistics/reset").methods(crow::HTTPMethod::Post)([&] {
        acq.reset_statistics();
        return json_response(200, {{"ok", true}});
    });

    auto result = [](const CalResult& r) {
        return json_response(r.ok ? 200 : 409, {{"ok", r.ok}, {"error", r.error}});
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
            return json_response(400, {{"ok", false}, {"error", ex.what()}});
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
            return json_response(400, {{"ok", false}, {"error", ex.what()}});
        }
        return result(acq.set_sim_load(g));
    });

    // --- WebSocket -------------------------------------------------------------
    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen([&](crow::websocket::connection& conn) {
            nlohmann::json hello = {{"type", "hello"},
                                    {"snapshot", acq.snapshot()},
                                    {"config", acq.config()}};
            std::lock_guard<std::mutex> lk(hub.m);
            hub.conns.insert(&conn);
            conn.send_text(hello.dump());
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
