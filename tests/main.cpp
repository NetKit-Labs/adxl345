#include "harness.hpp"

#include <print>
#include <string>

void run_unit_tests();
void run_regression_tests();

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which != "all" && which != "unit" && which != "regression") {
        std::println(stderr, "usage: adxl345_tests [all|unit|regression]");
        return 2;
    }
    if (which == "all" || which == "unit") {
        run_unit_tests();
    }
    if (which == "all" || which == "regression") {
        run_regression_tests();
    }
    std::println("{} passed, {} failed", test::passed, test::failed);
    return test::failed == 0 ? 0 : 1;
}
