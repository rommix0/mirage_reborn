// Runs before main: a failed Debug CRT assertion or abort() in a test reports
// on stderr and ends the process instead of waiting on a modal dialog.
#include "non_interactive.h"

namespace {
const bool kNonInteractive = (mirage::cli::makeNonInteractive(), true);
}  // namespace
