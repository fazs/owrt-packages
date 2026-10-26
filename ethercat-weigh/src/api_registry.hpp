#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "crow.h"
#include <nlohmann/json.hpp>

namespace ew {

/// Description of a REST route for the OpenAPI spec.
struct ApiDoc {
    /**
     * @param tag_ group (OpenAPI tag)
     * @param summary_ short description
     * @param request_ schema of the JSON body; null = no body
     * @param responses_ HTTP status code -> schema of the response
     */
    ApiDoc(std::string tag_, std::string summary_, nlohmann::json request_,
           std::map<int, nlohmann::json> responses_)
        : tag(std::move(tag_)), summary(std::move(summary_)), request(std::move(request_)),
          responses(std::move(responses_)) {}

    std::string tag;                              ///< group (OpenAPI tag)
    std::string summary;                          ///< short description
    nlohmann::json request;                       ///< schema of the JSON body; null = no body
    std::map<int, nlohmann::json> responses;      ///< HTTP status code -> schema of the response
    std::vector<std::string> params;              ///< names of the path parameters in order
    std::map<std::string, nlohmann::json> query;  ///< query parameter -> schema
    std::string media = "application/json";       ///< content type of the responses
};

/**
 * @brief Registers REST routes with Crow and records them for the OpenAPI spec.
 *
 * Crow itself has no metadata about routes; each route is therefore registered here together
 * with its description, so that path, method and parameters are defined in one place only.
 */
class ApiRegistry {
public:
    explicit ApiRegistry(crow::SimpleApp& app) : app_(app) {}

    /**
     * @brief Registers a route and its description.
     * @param method HTTP method
     * @param path Crow path, parameters as <uint>, <int>, <double>, <string>, <path>
     * @param doc description
     * @param handler Crow handler (lambda); parameters must match @p path
     * @throws std::runtime_error for a mismatching handler or a missing parameter name
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

    crow::SimpleApp& app_;              ///< Crow application
    nlohmann::json paths_ = nlohmann::json::object();       ///< OpenAPI "paths"
    nlohmann::json components_ = nlohmann::json::object();  ///< OpenAPI "components.schemas"
};

nlohmann::json schema_from(const nlohmann::json& example);
nlohmann::json schema_ref(const std::string& component);
nlohmann::json schema_object(const nlohmann::json& properties, const std::vector<std::string>& required = {});

} // namespace ew
