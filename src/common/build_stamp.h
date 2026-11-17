// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace Common {

/// Package stamp: a 16-byte id written into the binary after the build (per package).
/// Empty for an unstamped build. Shown in the log and the log export.
std::string BuildStamp();

} // namespace Common
