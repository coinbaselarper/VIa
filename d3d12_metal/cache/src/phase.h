#pragma once

#include "pcache.h"

namespace gta6rt::pcache {

// The title's phase from its own state words (retail 1.0.2.1 RVAs, as host/src/title_watch.cpp reads them).
// False when the title image is not found or not readable (standalone tools and tests).
bool read_title_phase(Phase& out);

double process_age_s();

}  // namespace gta6rt::pcache
