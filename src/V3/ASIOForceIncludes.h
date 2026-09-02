#pragma once
// The Steinberg ASIO SDK sources assume the host build has already pulled in
// the Windows + COM headers (iasiodrv.h declares `interface IASIO : public
// IUnknown`). Force them ahead of every SDK translation unit.
#include <windows.h>
#include <objbase.h>
