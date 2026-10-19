#include "callgrind.h"

void cachetrace_roi_start(void) {
    CALLGRIND_START_INSTRUMENTATION;
}

void cachetrace_roi_stop(void) {
    CALLGRIND_STOP_INSTRUMENTATION;
}

