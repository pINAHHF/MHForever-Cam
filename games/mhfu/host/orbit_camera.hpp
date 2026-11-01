#pragma once

#include "mhf/psp/runtime.hpp"

namespace mhf::psp {

// Registers the MHFU standard-camera replacement. The original translated
// function remains available as the native collision/special-mode fallback.
void register_mhfu_orbit_camera(Runtime &runtime);

} // namespace mhf::psp
