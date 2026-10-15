#ifndef SVMS_CONFIGURATOR_DRIVERLINK_H
#define SVMS_CONFIGURATOR_DRIVERLINK_H

// How the configurator talks to a running driver: Win32 shared-memory
// RuntimeLink on Windows, the Control API socket on Linux. Both provide
// svms::RuntimeLinkClient with the same public surface.
#ifdef _WIN32
#include "../SVMSRuntimeLink.h"
#else
#include "SocketRuntimeLink.h"
#endif

#endif
