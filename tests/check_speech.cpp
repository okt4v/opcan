#include "speech.hpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

void checkSpeech() {
    CHECK(spokenSan("e4") == "e4");
    CHECK(spokenSan("exd5") == "e takes d5");
    CHECK(spokenSan("Nxe5+") == "Knight takes e5, check");
    CHECK(spokenSan("Nbd7") == "Knight b d7");
    CHECK(spokenSan("R1e2") == "Rook 1 e2");
    CHECK(spokenSan("Nbxd7!") == "Knight b takes d7");
    CHECK(spokenSan("O-O") == "Castles kingside");
    CHECK(spokenSan("O-O-O+") == "Castles queenside, check");
    CHECK(spokenSan("0-0#") == "Castles kingside, checkmate");
    CHECK(spokenSan("e8=Q") == "e8, promotes to queen");
    CHECK(spokenSan("exd8=Q#") == "e takes d8, promotes to queen, checkmate");
    CHECK(spokenSan("b1=N+?!") == "b1, promotes to knight, check");
    CHECK(spokenSan("Qh4e1??") == "Queen h4 e1");
    CHECK(spokenSan("Kf1") == "King f1");
}
