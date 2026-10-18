#pragma once
// Makes a failing process report on stderr and exit instead of opening a modal
// dialog (MSVC Debug CRT assertions, abort() and Windows error boxes), so test
// and CLI runs can never hang waiting for a click. A no-op elsewhere.
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mirage::cli {

inline void makeNonInteractive() {
#ifdef _MSC_VER
  _set_error_mode(_OUT_TO_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
}

}  // namespace mirage::cli
