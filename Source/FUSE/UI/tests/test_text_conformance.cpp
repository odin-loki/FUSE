// Unicode conformance gates for fuse_text, run against the official test files (trimmed copies in
// tests/data/unicode, Unicode License V3 in LICENSE.txt there):
//   fuse_text_grapheme_conformance  GraphemeBreakTest.txt (15.1) vs fuse_text's own UAX #29 (both profiles):
//                                   nextBoundary, prevBoundary and graphemeCount
//   fuse_text_linebreak_conformance LineBreakTest.txt (15.0, libunibreak's line-break data version) vs lineBreaks()
//                                   (FULL; MINIMAL runs the lines its space / newline breaker claims: letters, SP, LF)
//   fuse_text_bidi_conformance      BidiCharacterTest.txt (15.1) vs bidiLevels() + reorderVisual() (FULL; MINIMAL
//                                   skips with 77): paragraph level, levels after L1, visual order
// A case that fails must be fixed or listed in kKnown with a reason.
#include "fuse/text/text.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace fuse::text;

namespace {
struct Known {
    const char* gate;
    const char* cps; // the test line's code points
    const char* reason;
};
// Known exceptions. All are in libunibreak's own skip list (src/test_skips.h of libunibreak 6.1).
constexpr const char* kLb25 = "libunibreak implements plain LB25, not the Example 7 number regex the test file uses";
constexpr Known kKnown[] = {
    {"linebreak", "007D 0025", kLb25}, {"linebreak", "007D 0024", kLb25}, {"linebreak", "002C 0030", kLb25},
    {"linebreak", "0025 2329", kLb25}, {"linebreak", "0025 0028", kLb25}, {"linebreak", "0024 2329", kLb25},
    {"linebreak", "0024 0028", kLb25}, {"linebreak", "002F 0030", kLb25}, {"linebreak", "0029 0025", kLb25},
    {"linebreak", "0029 0024", kLb25},
    {"linebreak", "0065 0071 0075 0061 006C 0073 0020 002E 0033 0035 0020 0063 0065 006E 0074 0073", kLb25},
    {"linebreak", "0063 006F 0064 0065 005C 0028 0073 005C 0029", kLb25},
    {"linebreak", "0063 006F 0064 0065 005C 007B 0073 005C 007D", kLb25},
    {"linebreak", "BD24 C5B4 002E 0020 0041 002E 0032 0020 BCFC", kLb25},
    {"linebreak", "BD10 C694 002E 0020 0041 002E 0033 0020 BABB", kLb25},
    {"linebreak", "C694 002E 0020 0041 002E 0034 0020 BABB", kLb25},
    {"linebreak", "1F02C 1F3FF", "unassigned Extended_Pictographic (XX) is treated as AL, so LB30b does not apply"},
};
bool known(std::string_view gate, std::string_view what) {
    for (const Known& k : kKnown) {
        const std::string_view c = k.cps;
        if (gate == k.gate && what.starts_with(c) && what.substr(c.size()).starts_with(" :")) return true;
    }
    return false;
}

std::vector<std::string> readLines(const std::string& path) {
    std::ifstream f(path);
    std::vector<std::string> out;
    for (std::string l; std::getline(f, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
    }
    return out;
}

struct Tally {
    int pass = 0, fail = 0, knownFail = 0, shown = 0;
    void report(std::string_view gate, int line, bool ok, const std::string& what) {
        if (ok) { ++pass; return; }
        if (known(gate, what)) { ++knownFail; return; }
        ++fail;
        if (shown++ < 80) std::fprintf(stderr, "  line %d: %s\n", line, what.c_str());
    }
    int finish(const char* name, int total) const {
        std::printf("%s: %d/%d pass, %d known exceptions, %d failures\n", name, pass, total, knownFail, fail);
        return fail ? 1 : 0;
    }
};

// "÷ 0020 × 0308 ÷" -> code points and the marks after each (true = break); `sot` gets the leading mark.
bool parseBreakLine(std::string_view l, std::vector<char32_t>& cps, std::vector<bool>& after, bool& sot) {
    cps.clear();
    after.clear();
    std::istringstream in{std::string(l)};
    std::string tok;
    bool first = true;
    while (in >> tok) {
        const bool brk = tok == "\xC3\xB7", no = tok == "\xC3\x97";
        if (brk || no) {
            if (first) sot = brk;
            else after.push_back(brk);
            first = false;
            continue;
        }
        cps.push_back(char32_t(std::strtoul(tok.c_str(), nullptr, 16)));
    }
    return !cps.empty() && after.size() == cps.size();
}

std::string hexList(const std::vector<char32_t>& cps) {
    std::string s;
    char b[16];
    for (char32_t c : cps) {
        std::snprintf(b, sizeof b, "%04X ", unsigned(c));
        s += b;
    }
    return s;
}

// ---- UAX #29 ------------------------------------------------------------------------------------------------------
int grapheme(const std::string& dir) {
    const auto lines = readLines(dir + "/GraphemeBreakTest.txt");
    Tally t;
    int total = 0;
    std::vector<char32_t> cps;
    std::vector<bool> after;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const std::string& l = lines[li];
        if (l.empty() || l[0] == '#') continue;
        bool sot = true;
        if (!parseBreakLine(l, cps, after, sot)) continue;
        ++total;
        std::string s;
        std::vector<std::size_t> want; // boundary byte offsets after 0
        for (std::size_t k = 0; k < cps.size(); ++k) {
            appendUtf8(s, cps[k]);
            if (after[k]) want.push_back(s.size());
        }
        std::vector<std::size_t> got;
        for (std::size_t i = 0; i < s.size();) got.push_back(i = nextBoundary(s, i));
        std::vector<std::size_t> back;
        for (std::size_t i = s.size(); i > 0;) back.insert(back.begin(), i), i = prevBoundary(s, i);
        const bool ok = got == want && back == want && graphemeCount(s) == want.size();
        t.report("grapheme", int(li + 1), ok, hexList(cps) + "(" + std::to_string(want.size()) + " clusters, next gives " +
                                                std::to_string(got.size()) + ", count " +
                                                std::to_string(graphemeCount(s)) + ")");
    }
    std::printf("Unicode %s grapheme tables\n", unicodeVersion());
    return t.finish("fuse_text_grapheme_conformance", total);
}

// ---- UAX #14 ------------------------------------------------------------------------------------------------------
int linebreak(const std::string& dir) {
    const bool full = textProfile() == TextProfile::Full;
    const auto lines = readLines(dir + "/LineBreakTest.txt");
    Tally t;
    int total = 0, skipped = 0;
    std::vector<char32_t> cps;
    std::vector<bool> after;
    std::vector<LineBreak> br;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const std::string& l = lines[li];
        if (l.empty() || l[0] == '#') continue;
        bool sot = false;
        if (!parseBreakLine(l, cps, after, sot)) continue;
        if (!full) { // the MINIMAL breaker only claims Latin letters, spaces and hard newlines
            bool simple = true;
            for (char32_t c : cps) simple &= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ' || c == '\n';
            if (!simple) { ++skipped; continue; }
        }
        ++total;
        std::string s;
        std::vector<std::size_t> ends;
        for (char32_t c : cps) {
            appendUtf8(s, c);
            ends.push_back(s.size() - 1);
        }
        lineBreaks(s, br, LineBreakStyle::Strict); // UAX #14 default CJ resolution
        bool ok = br.size() == s.size();
        std::string what = hexList(cps) + ":";
        for (std::size_t k = 0; br.size() == s.size() && k < cps.size(); ++k) {
            const bool brk = br[ends[k]] == LineBreak::Must || br[ends[k]] == LineBreak::Allow;
            what += brk ? " /" : " x";
            ok &= brk == bool(after[k]);
        }
        t.report("linebreak", int(li + 1), ok, what);
    }
    if (!full) std::printf("MINIMAL: %d lines outside the minimal breaker's subset skipped\n", skipped);
    if (total == 0) {
        std::printf("fuse_text_linebreak_conformance: no lines in this profile's subset, skipped\n");
        return 77;
    }
    return t.finish("fuse_text_linebreak_conformance", total);
}

// ---- UAX #9 -------------------------------------------------------------------------------------------------------
int bidi(const std::string& dir) {
    if (textProfile() != TextProfile::Full) {
        std::printf("fuse_text_bidi_conformance: MINIMAL has no bidi, skipped\n");
        return 77;
    }
    const auto lines = readLines(dir + "/BidiCharacterTest.txt");
    Tally t;
    int total = 0;
    std::vector<std::uint8_t> byteLevels, cpLevels;
    std::vector<std::uint32_t> order;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const std::string& l = lines[li];
        if (l.empty() || l[0] == '#') continue;
        std::vector<std::string> f;
        std::istringstream in(l);
        for (std::string x; std::getline(in, x, ';');) f.push_back(x);
        if (f.size() < 5) continue;
        ++total;
        std::vector<char32_t> cps;
        {
            std::istringstream c(f[0]);
            for (std::string x; c >> x;) cps.push_back(char32_t(std::strtoul(x.c_str(), nullptr, 16)));
        }
        const int dirField = std::atoi(f[1].c_str()), wantPara = std::atoi(f[2].c_str());
        std::vector<int> wantLv; // -1 = removed by X9
        {
            std::istringstream c(f[3]);
            for (std::string x; c >> x;) wantLv.push_back(x == "x" ? -1 : std::atoi(x.c_str()));
        }
        std::vector<std::uint32_t> wantOrder;
        {
            std::istringstream c(f[4]);
            for (std::string x; c >> x;) wantOrder.push_back(std::uint32_t(std::atoi(x.c_str())));
        }
        std::string s;
        std::vector<std::size_t> starts;
        for (char32_t c : cps) {
            starts.push_back(s.size());
            appendUtf8(s, c);
        }
        std::uint8_t para = 0;
        bidiLevels(s, dirField == 0 ? BaseDirection::Ltr : dirField == 1 ? BaseDirection::Rtl : BaseDirection::Auto,
                   byteLevels, para);
        cpLevels.clear();
        for (std::size_t st : starts) cpLevels.push_back(byteLevels[st]);
        bool ok = para == wantPara && wantLv.size() == cps.size();
        for (std::size_t k = 0; ok && k < cps.size(); ++k) ok &= wantLv[k] < 0 || wantLv[k] == cpLevels[k];
        reorderVisual(cpLevels, order);
        std::vector<std::uint32_t> gotOrder;
        for (std::uint32_t k : order)
            if (k < wantLv.size() && wantLv[k] >= 0) gotOrder.push_back(k);
        ok &= gotOrder == wantOrder;
        std::string what = f[0] + " dir " + f[1] + ": para " + std::to_string(para) + " levels";
        for (std::uint8_t v : cpLevels) what += " " + std::to_string(v);
        what += " order";
        for (std::uint32_t v : gotOrder) what += " " + std::to_string(v);
        t.report("bidi", int(li + 1), ok, what);
    }
    return t.finish("fuse_text_bidi_conformance", total);
}
} // namespace

int main() {
    const std::string dir = FUSE_UI_TEST_DATA "/unicode";
    const std::string_view gate = FUSE_TEXT_CONFORMANCE_GATE;
    if (gate == "grapheme") return grapheme(dir);
    if (gate == "linebreak") return linebreak(dir);
    if (gate == "bidi") return bidi(dir);
    std::fprintf(stderr, "unknown gate\n");
    return 2;
}
