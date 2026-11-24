#pragma once
#include "app.h"

/// Broadcasts a SoftKVM discovery request on every IPv4 network and collects Mac replies
/// for timeout_ms. Blocking; run it off the main thread. Returns the number found.
int discovery_run(AppHost *out, int max, int timeout_ms);
