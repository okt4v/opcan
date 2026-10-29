// Self-checks: build target opcan_check, run ./opcan_check, exits non-zero on failure.
#include <cstdio>
void checkCore();
void checkReview();
int main() {
    checkCore();
    checkReview();
    std::puts("all checks passed");
}
