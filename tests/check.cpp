// Self-checks: build target opcan_check, run ./opcan_check, exits non-zero on failure.
#include <cstdio>
void checkCore();
void checkReview();
void checkStore();
void checkStats();
int main() {
    checkCore();
    checkReview();
    checkStore();
    checkStats();
    std::puts("all checks passed");
}
