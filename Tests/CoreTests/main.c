/* Standalone runner used by Scripts/test-core.sh (not part of the Xcode test bundle).
 *   core-tests                      run every test
 *   core-tests --write-configs DIR  write the default config files (configs/) */
#include "tests.h"
#include <string.h>

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--write-configs")) {
        if (test_write_configs(argv[2]) != 0) { fprintf(stderr, "cannot write into %s\n", argv[2]); return 1; }
        fprintf(stderr, "wrote bridge.conf, controls.conf, feedback.conf into %s\n", argv[2]);
        return 0;
    }
    return dsxp_core_tests_run() == 0 ? 0 : 1;
}
