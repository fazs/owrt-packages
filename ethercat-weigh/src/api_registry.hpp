#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "crow.h"
#include <nlohmann/json.hpp>

namespace ew {

/// Beschreibung einer REST-Route für die OpenAPI-Spec.
struct ApiDoc {
    /**
     * @param tag_ Gruppe (OpenAPI-Tag)
     * @param summary_ Kurzbeschreibung
     * @param request_ Schema des JSON-Body; null = kein Body
     * @param responses_ HTTP-Statuscode -> Schema der Antwort
     */
    ApiDoc(std::string tag_, std::string summary_, nlohmann::json request_,
           std::map<int, nlohmann::json> responses_)
        : tag(std::move(tag_)), summary(std::move(summary_)), request(std::move(request_)),
          responses(std::move(responses_)) {}

    std::string tag;                              ///< Gruppe (OpenAPI-Tag)
    std::string summary;                          ///< Kurzbeschreibung
    nlohmann::json request;                       ///< Schema des JSON-Body; null = kein Body
    std::map<int, nlohmann::json> responses;      ///< HTTP-Statuscode -> Schema der Antwort
    std::vector<std::string> params;              ///< Namen der Pfadparameter in Reihenfolge
    std::map<std::string, nlohmann::json> query;  ///< Query-Parameter -> Schema
    std::string media = "application/json";       ///< Content-Type der Antworten
};

/**
 * @brief Registriert REST-Routen bei Crow und zeichnet sie für die OpenAPI-Spec auf.
 *
 * Crow selbst kennt keine Metadaten zu Routen; jede Route wird daher hier zusammen mit
 * ihrer Beschreibung registriert, damit Pfad, Methode und Parameter nur an einer Stelle stehen.
 */
class ApiRegistry {
public:
    explicit ApiRegistry(crow::SimpleApp& app) : app_(app) {}

    /**
     * @brief Registriert eine Route und ihre Beschreibung.
     * @param method HTTP-Methode
     * @param path Crow-Pfad, Parameter als <uint>, <int>, <double>, <string>, <path>
     * @param doc Beschreibung
     * @param handler Crow-Handler (Lambda); Parameter müssen zu @p path passen
     * @throws std::runtime_error bei unpassendem Handler oder fehlendem Parameternamen
     */
    template <class H>
    void add(crow::HTTPMethod method, const std::string& path, ApiDoc doc, H&& handler) {
        record(method, path, std::move(doc));
        app_.route_dynamic(path).methods(method)(std::forward<H>(handler));
    }

    void component(const std::string& name, nlohmann::json schema);

    [[nodiscard]] nlohmann::json openapi(const std::string& version, const std::string& description) const;

private:
    void record(crow::HTTPMethod method, const std::string& path, ApiDoc doc);

    crow::SimpleApp& app_;              ///< Crow-Anwendung
    nlohmann::json paths_ = nlohmann::json::object();       ///< OpenAPI "paths"
    nlohmann::json components_ = nlohmann::json::object();  ///< OpenAPI "components.schemas"
};

nlohmann::json schema_from(const nlohmann::json& example);
nlohmann::json schema_ref(const std::string& component);
nlohmann::json schema_object(const nlohmann::json& properties, const std::vector<std::string>& required = {});

} // namespace ew
