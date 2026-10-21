#ifndef SVMS_CONFIGURATOR_DRIVERLINK_H
#define SVMS_CONFIGURATOR_DRIVERLINK_H

// How the configurator talks to a running driver, on every platform: the
// SVMSAPI remote client (svms::EngineLink over SVMS_Remote_*). The transport
// (Win32 shared memory, Unix socket) lives behind frontends/RemoteClient.cpp.
#include "configurator/EngineLink.h"

#endif
