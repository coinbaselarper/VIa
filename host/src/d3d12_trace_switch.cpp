#include "d3d12_trace.h"

namespace host {
void enable_d3d12_trace() { rt::d3d12_trace::set_enabled(true); }
}  // namespace host
