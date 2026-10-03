/**
 * @file dump_tuner_page.c
 * @brief Host helper for the page-script test (SPEC-003 T-21 host part, SPEC-005 FR-15): prints the embedded page
 *        exactly as main/http_portal/tuner_page.c builds it, so test_tuner_page_js.js runs the real script.
 *
 * Usage: dump_tuner_page provisioning|station   -> the page on stdout, exit 0
 *        dump_tuner_page --skip                 -> a skip notice, exit 77 (ctest SKIP_RETURN_CODE; Node not found)
 */
#include <stdio.h>
#include <string.h>
#include "tuner_page.h"

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--skip") == 0) {
        printf("SKIPPED: Node.js was not found at configure time; the tuner page script test did not run.\n");
        return 77;
    }
    if (argc == 2 && strcmp(argv[1], "provisioning") == 0) {
        fputs(g_tuner_page, stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "station") == 0) {
        fputs(g_tuner_page_station, stdout);
        return 0;
    }
    fprintf(stderr, "usage: %s provisioning|station|--skip\n", argv[0]);
    return 2;
}
