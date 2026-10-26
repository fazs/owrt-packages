#include "api_registry.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace ew {

namespace {

/// OpenAPI version of the generated document
constexpr const char* kOpenApiVersion = "3.1.0";

/// Prefix for references to components.schemas
constexpr const char* kComponentPrefix = "#/components/schemas/";

/**
 * @brief Description of a response for the OpenAPI spec (mandatory field "description").
 * @param code HTTP status code
 * @return text for the status code, "Response" for an unknown code
 */
std::string response_description(int code) {
    switch (code) {
    case crow::status::OK: return "OK";
    case crow::status::BAD_REQUEST: return "Bad request";
    case crow::status::NOT_FOUND: return "Not found";
    case crow::status::CONFLICT: return "Not executable in the current state";
    default: return "Response";
    }
}

/**
 * @brief Schema of a Crow path parameter.
 * @param tag type from the path without angle brackets (uint, int, double, string, path)
 * @return JSON schema
 * @throws std::runtime_error for an unknown type
 */
nlohmann::json param_schema(const std::string& tag) {
    if (tag == "uint") return {{"type", "integer"}, {"minimum", 0}};
    if (tag == "int") return {{"type", "integer"}};
    if (tag == "double") return {{"type", "number"}};
    if (tag == "string" || tag == "path") return {{"type", "string"}};
    throw std::runtime_error("Unbekannter Pfadparameter <" + tag + ">");
}

} // namespace

/**
 * @brief Stores a schema under components.schemas (reference with schema_ref()).
 * @param name name of the component
 * @param schema JSON schema
 */
void ApiRegistry::component(const std::string& name, nlohmann::json schema) {
    components_[name] = std::move(schema);
}

/**
 * @brief Converts path and description into an OpenAPI operation.
 *
 * Crow parameters (<uint> etc.) become {name} with the names from ApiDoc::params.
 *
 * @param method HTTP method
 * @param path Crow path
 * @param doc description
 * @throws std::runtime_error for a missing parameter name or a duplicate route
 */
void ApiRegistry::record(crow::HTTPMethod method, const std::string& path, ApiDoc doc) {
    std::string oa_path;
    nlohmann::json parameters = nlohmann::json::array();
    size_t n_param = 0U;
    for (size_t i = 0U; i < path.size(); ++i) {
        if (path[i] != '<') {
            oa_path += path[i];
            continue;
        }
        const size_t end = path.find('>', i);
        if (end == std::string::npos) throw std::runtime_error("Pfad ohne '>': " + path);
        if (n_param >= doc.params.size()) throw std::runtime_error("Name des Pfadparameters fehlt: " + path);
        const std::string& name = doc.params[n_param++];
        oa_path += "{" + name + "}";
        parameters.push_back({{"name", name},
                              {"in", "path"},
                              {"required", true},
                              {"schema", param_schema(path.substr(i + 1U, end - i - 1U))}});
        i = end;
    }
    for (const auto& [name, schema] : doc.query)
        parameters.push_back({{"name", name}, {"in", "query"}, {"required", false}, {"schema", schema}});

    std::string verb = crow::method_name(method);
    std::transform(verb.begin(), verb.end(), verb.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    nlohmann::json op = {{"tags", {doc.tag}}, {"summary", doc.summary}};
    if (!parameters.empty()) op["parameters"] = parameters;
    if (!doc.request.is_null())
        op["requestBody"] = {{"required", true},
                             {"content", {{"application/json", {{"schema", doc.request}}}}}};
    nlohmann::json responses = nlohmann::json::object();
    for (const auto& [code, schema] : doc.responses) {
        nlohmann::json r = {{"description", response_description(code)}};
        if (!schema.is_null()) r["content"] = {{doc.media, {{"schema", schema}}}};
        responses[std::to_string(code)] = r;
    }
    op["responses"] = responses;

    nlohmann::json& item = paths_[oa_path];
    if (item.contains(verb)) throw std::runtime_error("Route doppelt registriert: " + verb + " " + path);
    item[verb] = op;
}

/**
 * @brief OpenAPI document of all registered routes.
 * @param version version of the application (info.version)
 * @param description description (info.description)
 * @return OpenAPI 3.1
 */
nlohmann::json ApiRegistry::openapi(const std::string& version, const std::string& description) const {
    nlohmann::json doc = {{"openapi", kOpenApiVersion},
                          {"info", {{"title", "ethercat-weigh"}, {"version", version}, {"description", description}}},
                          {"paths", paths_}};
    if (!components_.empty()) doc["components"] = {{"schemas", components_}};
    return doc;
}

/**
 * @brief Derives a JSON schema from an example value (types only, without values).
 *
 * Objects are described recursively; required fields are not specified, because an
 * example does not prove that a field is present in every state. Arrays get the
 * schema of their first element; null and empty arrays remain unconstrained.
 *
 * @param example example value, e.g. nlohmann::json(Config{})
 * @return JSON schema
 */
nlohmann::json schema_from(const nlohmann::json& example) {
    switch (example.type()) {
    case nlohmann::json::value_t::object: {
        nlohmann::json props = nlohmann::json::object();
        for (const auto& [key, value] : example.items()) props[key] = schema_from(value);
        return schema_object(props);
    }
    case nlohmann::json::value_t::array:
        if (example.empty()) return {{"type", "array"}};
        return {{"type", "array"}, {"items", schema_from(example.front())}};
    case nlohmann::json::value_t::string: return {{"type", "string"}};
    case nlohmann::json::value_t::boolean: return {{"type", "boolean"}};
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned: return {{"type", "integer"}};
    case nlohmann::json::value_t::number_float: return {{"type", "number"}};
    default: return nlohmann::json::object();
    }
}

/**
 * @brief Reference to a schema under components.schemas.
 * @param component name of the component (ApiRegistry::component)
 * @return {"$ref": ...}
 */
nlohmann::json schema_ref(const std::string& component) {
    return {{"$ref", std::string(kComponentPrefix) + component}};
}

/**
 * @brief Schema of an object.
 * @param properties field name -> schema
 * @param required required fields
 * @return JSON schema
 */
nlohmann::json schema_object(const nlohmann::json& properties, const std::vector<std::string>& required) {
    nlohmann::json s = {{"type", "object"}, {"properties", properties}};
    if (!required.empty()) s["required"] = required;
    return s;
}

} // namespace ew
