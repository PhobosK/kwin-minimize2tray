#pragma once

namespace Minimize2Tray {
// Open the Unix socket peer's kernel process handle. Caller owns the result.
int openPeerProcess(int socket);
} // namespace Minimize2Tray
