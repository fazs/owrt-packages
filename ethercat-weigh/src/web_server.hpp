#pragma once

#include "acquisition.hpp"
#include "config.hpp"

namespace ew {

void run_web_server(Acquisition& acq, const HttpConfig& http);

} // namespace ew
