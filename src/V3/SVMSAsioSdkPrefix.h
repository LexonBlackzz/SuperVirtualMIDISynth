// Forced-include prefix for the vendored ASIO SDK translation units.
// SVMS compiles with WIN32_LEAN_AND_MEAN and UNICODE, both of which break
// the SDK's ANSI/COM expectations; undo them for the SDK files only.
#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif
#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif
#include <windows.h>
