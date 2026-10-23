#pragma once

#include "acquisition.hpp"
#include "config.hpp"

namespace ew {

// HTTP-Server (Crow): REST-API, WebSocket /ws, statisches Frontend aus http.web_root.
// Blockiert bis SIGINT/SIGTERM.
void run_web_server(Acquisition& acq, const HttpConfig& http);

} // namespace ew
