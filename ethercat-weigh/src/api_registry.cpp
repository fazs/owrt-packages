#include "api_registry.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace ew {

namespace {

/// OpenAPI-Version des erzeugten Dokuments
constexpr const char* kOpenApiVersion = "3.1.0";

/// Präfix für Verweise auf components.schemas
constexpr const char* kComponentPrefix = "#/components/schemas/";

/**
 * @brief Beschreibung einer Antwort für die OpenAPI-Spec (Pflichtfeld "description").
 * @param code HTTP-Statuscode
 * @return Text zum Statuscode, "Antwort" bei unbekanntem Code
 */
std::string response_description(int code) {
    switch (code) {
    case crow::status::OK: return "OK";
    case crow::status::BAD_REQUEST: return "Ungültige Anfrage";
    case crow::status::NOT_FOUND: return "Nicht gefunden";
    case crow::status::CONFLICT: return "Im aktuellen Zustand nicht ausführbar";
    default: return "Antwort";
    }
}

/**
 * @brief Schema eines Crow-Pfadparameters.
 * @param tag Typangabe aus dem Pfad ohne spitze Klammern (uint, int, double, string, path)
 * @return JSON-Schema
 * @throws std::runtime_error bei unbekannter Typangabe
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
 * @brief Legt ein Schema unter components.schemas ab (Verweis mit schema_ref()).
 * @param name Name der Komponente
 * @param schema JSON-Schema
 */
void ApiRegistry::component(const std::string& name, nlohmann::json schema) {
    components_[name] = std::move(schema);
}

/**
 * @brief Setzt Pfad und Beschreibung in eine OpenAPI-Operation um.
 *
 * Crow-Parameter (<uint> usw.) werden zu {name} mit den Namen aus ApiDoc::params.
 *
 * @param method HTTP-Methode
 * @param path Crow-Pfad
 * @param doc Beschreibung
 * @throws std::runtime_error bei fehlendem Parameternamen oder doppelter Route
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
 * @brief OpenAPI-Dokument aller registrierten Routen.
 * @param version Version der Anwendung (info.version)
 * @param description Beschreibung (info.description)
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
 * @brief Leitet ein JSON-Schema aus einem Beispielwert ab (nur Typen, ohne Werte).
 *
 * Objekte werden rekursiv beschrieben; Pflichtfelder werden nicht angegeben, weil ein
 * Beispiel nicht belegt, dass ein Feld in jedem Zustand vorhanden ist. Arrays erhalten das
 * Schema ihres ersten Elements; null und leere Arrays bleiben ohne Einschränkung.
 *
 * @param example Beispielwert, z. B. nlohmann::json(Config{})
 * @return JSON-Schema
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
 * @brief Verweis auf ein Schema unter components.schemas.
 * @param component Name der Komponente (ApiRegistry::component)
 * @return {"$ref": ...}
 */
nlohmann::json schema_ref(const std::string& component) {
    return {{"$ref", std::string(kComponentPrefix) + component}};
}

/**
 * @brief Schema eines Objekts.
 * @param properties Feldname -> Schema
 * @param required Pflichtfelder
 * @return JSON-Schema
 */
nlohmann::json schema_object(const nlohmann::json& properties, const std::vector<std::string>& required) {
    nlohmann::json s = {{"type", "object"}, {"properties", properties}};
    if (!required.empty()) s["required"] = required;
    return s;
}

} // namespace ew
