#pragma once

#include <nlohmann/json.hpp>

#include "acquisition.hpp"
#include "config.hpp"

namespace ew {

nlohmann::json openapi_spec();

void run_web_server(Acquisition& acq, const HttpConfig& http);

} // namespace ew
