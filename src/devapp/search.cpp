#include "search.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace synth::devapp {

namespace {

char lower(char c) { return (char)std::tolower((unsigned char)c); }

bool boundary(const std::string& s, size_t i) {
  if (i == 0) return true;
  char p = s[i - 1];
  return p == ' ' || p == '.' || p == '_' || p == '-' || p == '/';
}

// Subsequence match with a score: every matched character counts, one
// that lands at the start of a word counts for more, and a run counts
// for more than the same characters scattered. Returns false when the
// query is not a subsequence at all.
bool fuzzy(const std::string& hay, const std::string& needle, int& score) {
  score = 0;
  if (needle.empty()) return true;
  size_t i = 0;
  int streak = 0;
  for (size_t k = 0; k < hay.size() && i < needle.size(); k++) {
    if (lower(hay[k]) != needle[i]) {
      streak = 0;
      continue;
    }
    score += 10;
    if (boundary(hay, k)) score += 8;
    if (k == 0) score += 10;
    score += streak * 4;
    streak++;
    i++;
  }
  if (i < needle.size()) return false;
  // A short name that used all of itself beats a long one that hid the
  // query somewhere in the middle.
  score -= (int)(hay.size() - needle.size());
  return true;
}

int kindRank(SearchItem::Kind k) {
  switch (k) {
    case SearchItem::Kind::Tab: return 0;
    case SearchItem::Kind::Window: return 1;
    case SearchItem::Kind::Element: return 2;
  }
  return 3;
}

}  // namespace

std::vector<WindowElement> windowElements(const PanelMeta& panel,
                                          const ProjectMeta& meta) {
  std::vector<WindowElement> out;
  for (const PanelMember& m : panel.controls) {
    const ControlMeta* plain = nullptr;
    bool group = false;
    for (const ControlMeta& c : meta.controls) {
      if (c.name == m.name && c.group.empty()) plain = &c;
      if (c.group == m.name) group = true;
    }
    if (plain)
      out.push_back(
          WindowElement{WindowElement::Kind::Control, m.name, m.depth, m.key});
    else if (group) {
      out.push_back(
          WindowElement{WindowElement::Kind::Group, m.name, m.depth, m.key});
      // The lanes under it: a lane is what has a value to select and
      // nudge, so each is a row in its own right, named the way the
      // metadata names it ("<group>.<lane>").
      for (const ControlMeta& c : meta.controls)
        if (c.group == m.name)
          out.push_back(WindowElement{WindowElement::Kind::Lane, c.name,
                                      m.depth + 1, {}});
    }
  }
  for (const std::string& t : panel.targets)
    for (const TargetMeta& m : meta.targets)
      if (m.name == t)
        out.push_back(WindowElement{WindowElement::Kind::Target, t, 0, {}});
  return out;
}

std::vector<WindowElement> overviewElements(
    const std::vector<UnitIndex>& units) {
  std::vector<WindowElement> out;
  for (const UnitIndex& u : units)
    for (const PanelMeta& p : u.panels) {
      WindowRef ref;
      ref.kind = WindowRef::Kind::Panel;
      ref.unit = u.unit;
      ref.panel = p.name;
      out.push_back(
          WindowElement{WindowElement::Kind::Panel, windowId(ref), 0, {}});
    }
  return out;
}

std::vector<SearchItem> buildSearchIndex(const std::vector<Tab>& tabs,
                                         const std::vector<UnitIndex>& units) {
  std::vector<SearchItem> out;
  for (const Tab& t : tabs) {
    SearchItem item;
    item.kind = SearchItem::Kind::Tab;
    item.label = tabLabel(t);
    size_t n = windowsIn(t).size();
    item.detail = n == 1 ? "1 window" : std::to_string(n) + " windows";
    item.tab = t.index;
    out.push_back(std::move(item));
  }

  // The overview, then every panel every unit declares - including the
  // ones no tab is showing, because finding one is how you open it.
  auto place = [&tabs](const WindowRef& w) { return tabHolding(tabs, w); };
  SearchItem ov;
  ov.kind = SearchItem::Kind::Window;
  ov.window = overviewWindow();
  ov.label = windowTitle(ov.window);
  ov.tab = place(ov.window);
  ov.detail = ov.tab ? "tab " + std::to_string(ov.tab) : "closed";
  out.push_back(std::move(ov));

  for (const UnitIndex& u : units) {
    if (!u.meta) continue;
    for (const PanelMeta& p : u.panels) {
      WindowRef ref;
      ref.kind = WindowRef::Kind::Panel;
      ref.unit = u.unit;
      ref.panel = p.name;
      int tab = place(ref);
      SearchItem w;
      w.kind = SearchItem::Kind::Window;
      w.window = ref;
      w.label = p.name;
      w.tab = tab;
      w.detail = tab ? "tab " + std::to_string(tab) : "closed";
      if (u.unit != ".") w.detail += " - " + u.unit;
      out.push_back(std::move(w));

      for (const WindowElement& e : windowElements(p, *u.meta)) {
        SearchItem it;
        it.kind = SearchItem::Kind::Element;
        it.window = ref;
        it.element = e.name;
        it.label = e.name;
        it.tab = tab;
        const char* what = e.kind == WindowElement::Kind::Target ? "waveform"
                           : e.kind == WindowElement::Kind::Group ? "lanes"
                           : e.kind == WindowElement::Kind::Lane  ? "lane"
                           : e.kind == WindowElement::Kind::Panel ? "panel"
                                                                 : "control";
        it.detail = p.name + " - " + what;
        out.push_back(std::move(it));
      }
    }
  }
  return out;
}

std::vector<Match> searchItems(const std::vector<SearchItem>& items,
                               std::string_view query) {
  std::string q;
  for (char c : query)
    if (!std::isspace((unsigned char)c)) q += lower(c);

  std::vector<Match> out;
  bool numeric = !q.empty() &&
                 std::all_of(q.begin(), q.end(),
                             [](char c) { return c >= '0' && c <= '9'; });
  for (size_t i = 0; i < items.size(); i++) {
    const SearchItem& it = items[i];
    int score = 0;
    if (!fuzzy(it.label, q, score)) {
      // "3" finds tab 3 even when its name shares no letter with it.
      if (!(numeric && it.kind == SearchItem::Kind::Tab &&
            std::to_string(it.tab) == q))
        continue;
      score = 0;
    }
    if (numeric && it.kind == SearchItem::Kind::Tab &&
        std::to_string(it.tab) == q)
      score += 1000;  // an exact tab number is what was asked for
    out.push_back(Match{i, score});
  }

  std::stable_sort(out.begin(), out.end(),
                   [&items, &q](const Match& a, const Match& b) {
                     if (q.empty()) return false;  // keep the index's order
                     if (a.score != b.score) return a.score > b.score;
                     const SearchItem& x = items[a.item];
                     const SearchItem& y = items[b.item];
                     int rx = kindRank(x.kind), ry = kindRank(y.kind);
                     if (rx != ry) return rx < ry;
                     return x.label < y.label;
                   });
  return out;
}

std::vector<std::string> rowKeys(const std::vector<WindowElement>& es) {
  std::vector<std::string> out(es.size());
  std::set<std::string> taken;
  size_t needed = 0;
  for (size_t i = 0; i < es.size(); i++) {
    if (es[i].key.empty()) {
      needed++;
      continue;
    }
    out[i] = es[i].key;
    taken.insert(es[i].key);
  }

  // Enough labels that `needed` of them survive striking out the
  // reserved ones - and, once the automatic labels are two characters
  // long, striking out any that start with a reserved letter too: `g`
  // and `ga` in one window would make `ga` unreachable, since `g`
  // matches first.
  std::vector<std::string> pool;
  for (size_t ask = needed + taken.size();
       pool.size() < needed && ask <= needed + taken.size() + 64; ask++) {
    pool.clear();
    for (const std::string& l : autoKeys(ask)) {
      if (taken.count(l)) continue;
      if (l.size() > 1 && taken.count(l.substr(0, 1))) continue;
      pool.push_back(l);
    }
  }

  size_t next = 0;
  for (size_t i = 0; i < es.size(); i++)
    if (out[i].empty() && next < pool.size()) out[i] = pool[next++];
  return out;
}

KeyMatch matchKey(const std::vector<std::string>& labels,
                    std::string_view typed, size_t& index) {
  if (typed.empty()) return labels.empty() ? KeyMatch::None : KeyMatch::Prefix;
  for (size_t i = 0; i < labels.size(); i++)
    if (labels[i] == typed) {
      index = i;
      return KeyMatch::Exact;
    }
  for (const std::string& l : labels)
    if (l.size() > typed.size() && l.compare(0, typed.size(), typed) == 0)
      return KeyMatch::Prefix;
  return KeyMatch::None;
}

std::vector<std::string> autoKeys(size_t n) {
  // Home row first, then the rest of the keyboard by reachability.
  static const std::string alphabet = "asdfghjklqwertyuiopzxcvbnm";
  std::vector<std::string> out;
  if (n <= alphabet.size()) {
    for (size_t i = 0; i < n; i++) out.push_back(std::string(1, alphabet[i]));
    return out;
  }
  // Past the alphabet every label is two characters, so no label is a
  // prefix of another and the second keystroke is never ambiguous.
  for (char a : alphabet) {
    for (char b : alphabet) {
      if (out.size() == n) return out;
      out.push_back({a, b});
    }
  }
  return out;
}

}  // namespace synth::devapp
