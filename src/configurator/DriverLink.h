#ifndef SVMS_CONFIGURATOR_DRIVERLINK_H
#define SVMS_CONFIGURATOR_DRIVERLINK_H

// How the configurator talks to a running driver. Pages use svms::EngineLink.
// Windows: the SVMSAPI remote client (EngineLink.h). Linux: the Control API
// socket client, until the native API can describe a driver's parameters
// (the "unknown to this version" page needs that schema).
#ifdef _WIN32
#include "configurator/EngineLink.h"
#else
#include "configurator/SocketRuntimeLink.h"
namespace svms {
class EngineLink : public RuntimeLinkClient {};
} // namespace svms
#endif

#endif
