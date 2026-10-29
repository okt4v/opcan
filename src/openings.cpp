#include "openings.hpp"
#include <fstream>
#include <sstream>

bool Openings::load(const std::string& tsvPath) {
    std::ifstream f(tsvPath);
    if (!f) return false;
    list_.clear(), named_.clear(), book_.clear();
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::stringstream ss(line);
        Opening o;
        if (!std::getline(ss, o.eco, '\t') || !std::getline(ss, o.name, '\t') || !std::getline(ss, o.pgn)) continue;
        if (o.eco == "eco") continue;  // header
        chess::Board b;
        std::vector<uint64_t> hashes;
        std::stringstream moves(o.pgn);
        bool ok = true;
        for (std::string tok; moves >> tok;) {
            if (tok.back() == '.') continue;  // move number
            try {
                auto m = chess::uci::parseSan(b, tok);
                if (m == chess::Move::NO_MOVE) { ok = false; break; }
                b.makeMove(m);
            } catch (...) { ok = false; break; }
            hashes.push_back(b.hash());
        }
        if (!ok || hashes.empty()) continue;
        book_.insert(hashes.begin(), hashes.end());
        named_.try_emplace(hashes.back(), list_.size());
        list_.push_back(std::move(o));
    }
    return !list_.empty();
}

const Opening* Openings::at(const chess::Board& b) const {
    auto it = named_.find(b.hash());
    return it == named_.end() ? nullptr : &list_[it->second];
}

bool Openings::isBook(const chess::Board& b) const { return book_.contains(b.hash()); }
