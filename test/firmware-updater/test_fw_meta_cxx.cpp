/**
 * @file test_fw_meta_cxx.cpp
 * @brief fw_meta.h is usable from C++ as-is (FW_META_STATIC_ASSERT maps to static_assert): this file includes it
 *        first, with no test-side shim, and checks the layout at compile time and at run time.
 */
extern "C" {
#include "fw_meta.h"
}
#include <catch2/catch_test_macros.hpp>

static_assert(sizeof(fw_meta_record_t) == 60, "record layout seen from C++");
static_assert(sizeof(fw_ctrl_record_t) == 12, "control record layout seen from C++");

TEST_CASE("fw_meta.h compiles from C++ without a shim and keeps the section 7.2 layout", "[T-19][NFR-8]")
{
    CHECK(sizeof(fw_embedded_meta_t) == 16);
    CHECK(FW_IMAGE_HEAD_LEN == 304u);
}
