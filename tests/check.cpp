// Self-checks: build target opcan_check, run ./opcan_check, exits non-zero on failure.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
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
    // each run gets its own temp dir: concurrent runs (several checkouts) otherwise share the fixed-name test DBs
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() /
                   ("opcan_check_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp);
#ifdef _WIN32
    _putenv_s("TMP", tmp.string().c_str()), _putenv_s("TEMP", tmp.string().c_str());
#else
    setenv("TMPDIR", tmp.c_str(), 1);
#endif
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
    std::error_code ec;
    fs::remove_all(tmp, ec);
    std::puts("all checks passed");
}
