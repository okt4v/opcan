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
void checkVision();
void checkTournament();
void checkUpdate();
void checkShare();
void checkGif();
void checkSpeech();
void checkBackup();
void checkVerify();
void checkAnalysis();
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
    checkVision();
    checkTournament();
    checkUpdate();
    checkShare();
    checkGif();
    checkSpeech();
    checkBackup();
    checkVerify();
    checkAnalysis();
    std::puts("all checks passed");
}
