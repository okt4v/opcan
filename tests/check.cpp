// Self-checks: build target opcan_check, run ./opcan_check, exits non-zero on failure.
#include <cstdio>
void checkCore();
void checkReview();
void checkStore();
int main() {
    checkCore();
    checkReview();
    checkStore();
    std::puts("all checks passed");
}
