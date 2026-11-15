#include <monkey_dust/particles/pu_script.h>
#include <sstream>

namespace md::pu {
namespace {

struct Line { std::vector<std::string> words; int no = 0; };

std::vector<Line> Split(const std::string& text) {
    std::vector<Line> lines;
    std::istringstream in(text);
    std::string s;
    int no = 0;
    while (std::getline(in, s)) {
        ++no;
        const size_t c = s.find("//");
        if (c != std::string::npos) s.erase(c);
        Line l; l.no = no;
        std::istringstream ws(s);
        std::string w;
        while (ws >> w) {
            // a brace written next to a word still opens / closes a block
            while (w.size() > 1 && (w.front() == '{' || w.front() == '}')) { l.words.push_back(std::string(1, w.front())); w.erase(0, 1); }
            std::vector<std::string> tail;
            while (w.size() > 1 && (w.back() == '{' || w.back() == '}')) { tail.insert(tail.begin(), std::string(1, w.back())); w.pop_back(); }
            if (!w.empty()) l.words.push_back(w);
            for (auto& t : tail) l.words.push_back(t);
        }
        if (!l.words.empty()) lines.push_back(std::move(l));
    }
    return lines;
}

bool Block(const std::vector<Line>& L, size_t& i, std::vector<Node>& out, bool top, std::string& err) {
    while (i < L.size()) {
        const Line& l = L[i];
        if (l.words[0] == "}") {
            if (top) { err = "line " + std::to_string(l.no) + ": stray '}'"; return false; }
            ++i;
            return true;
        }
        if (l.words[0] == "{") { err = "line " + std::to_string(l.no) + ": '{' without a keyword"; return false; }
        Node n; n.key = l.words[0]; n.line = l.no;
        bool opens = false;
        for (size_t k = 1; k < l.words.size(); ++k) {
            if (l.words[k] == "{") { opens = true; break; }
            n.args.push_back(l.words[k]);
        }
        ++i;
        if (!opens && i < L.size() && L[i].words[0] == "{") { opens = true; if (L[i].words.size() > 1) { err = "line " + std::to_string(L[i].no) + ": text after '{'"; return false; } ++i; }
        if (opens && !Block(L, i, n.children, false, err)) return false;
        out.push_back(std::move(n));
    }
    if (!top) { err = "unexpected end of file inside a block"; return false; }
    return true;
}

}  // namespace

bool Parse(const std::string& text, std::vector<Node>& out, std::string& err) {
    out.clear();
    const std::vector<Line> L = Split(text);
    size_t i = 0;
    return Block(L, i, out, true, err);
}

const Node* Find(const Node& n, const std::string& key) {
    for (const Node& c : n.children) if (c.key == key) return &c;
    return nullptr;
}

}  // namespace md::pu
