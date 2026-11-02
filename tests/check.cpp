// Self-checks: build target opcan_check, run ./opcan_check, exits non-zero on failure.
#include <cstdio>
void checkCore();
void checkReview();
void checkStore();
void checkStats();
void checkCoach();
void checkPlay();
void checkExplorer();
void checkRobust();
void checkTrain();
int main() {
    checkCore();
    checkReview();
    checkStore();
    checkStats();
    checkCoach();
    checkPlay();
    checkExplorer();
    checkRobust();
    checkTrain();
    std::puts("all checks passed");
}
