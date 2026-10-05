/**
 * @file dump_updater_page.c
 * @brief Host helper for the updater page-script test (SPEC-007 T-19, FR-25): prints the page exactly as
 *        updater/main/updater_http.c assembles it (head + installed version + middle + reason text + tail).
 *
 * Usage: dump_updater_page          -> the page on stdout, exit 0
 *        dump_updater_page --skip   -> a skip notice, exit 77 (ctest SKIP_RETURN_CODE; Node not found)
 */
#include <stdio.h>
#include <string.h>
#include "updater_page.h"

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--skip") == 0) {
        printf("SKIPPED: Node.js was not found at configure time; the updater page script test did not run.\n");
        return 77;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--skip]\n", argv[0]);
        return 2;
    }
    fputs(g_updater_page_head, stdout);
    fputs("01.00.00", stdout);
    fputs(g_updater_page_middle, stdout);
    fputs(GetUpdaterReasonText(UPDATER_REASON_REQUESTED), stdout);
    fputs(g_updater_page_tail, stdout);
    return 0;
}
