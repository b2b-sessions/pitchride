#pragma once

namespace mixxx {

/// B2B: called with the message of a fatal error (qFatal, RELEASE_ASSERT),
/// just before the app ends: the crash report writer sets it.
using FatalHook = void (*)(const char* message);
extern FatalHook g_onFatal;

} // namespace mixxx
