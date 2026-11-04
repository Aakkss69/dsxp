/* Standalone runner used by Scripts/test-core.sh (not part of the Xcode test bundle). */
#include "tests.h"

int main(void) { return dsxp_core_tests_run() == 0 ? 0 : 1; }
