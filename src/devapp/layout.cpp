#include "layout.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace synth::devapp {

namespace {

Node leafNode(const WindowRef& w) {
  Node n;
  n.kind = Node::Kind::Leaf;
  n.window = w;
  return n;
}

// Fractions are the invariant every operation restores: one per child,
// each at least kMinFraction, summing to 1.
void normalize(Node& n) {
  if (n.kind != Node::Kind::Split) return;
  n.fractions.resize(n.children.size(), 0.0);
  double total = 0;
  for (double f : n.fractions) total += std::max(0.0, f);
  if (!(total > 0)) {
    for (double& f : n.fractions) f = 1.0 / (double)n.children.size();
    return;
  }
  for (double& f : n.fractions) f = std::max(0.0, f) / total;
}

void collectWindows(const Node& n, std::vector<WindowRef>& out) {
  if (n.kind == Node::Kind::Leaf) out.push_back(n.window);
  for (const Node& c : n.children) collectWindows(c, out);
}

bool findPath(const Node& n, const WindowRef& w, Path& p) {
  if (n.kind == Node::Kind::Leaf) return n.window == w;
  for (size_t i = 0; i < n.children.size(); i++) {
    p.push_back((int)i);
    if (findPath(n.children[i], w, p)) return true;
    p.pop_back();
  }
  return false;
}

// The first leaf at or under `p`, which is where focus lands whenever a
// path stops naming one.
Path descendFirst(const Node& root, Path p) {
  const Node* n = at(root, p);
  while (n && n->kind == Node::Kind::Split && !n->children.empty()) {
    p.push_back(0);
    n = at(root, p);
  }
  return p;
}

// Descending into a neighbour crossed in direction (axis, step): along
// the axis we enter by the near edge, so pick the child on that side.
Path descendEdge(const Node& root, Path p, Split axis, int step) {
  const Node* n = at(root, p);
  while (n && n->kind == Node::Kind::Split && !n->children.empty()) {
    int i = (n->split == axis && step < 0) ? (int)n->children.size() - 1 : 0;
    p.push_back(i);
    n = at(root, p);
  }
  return p;
}

bool validLeaf(const Node& root, const Path& p) {
  const Node* n = at(root, p);
  return n && n->kind == Node::Kind::Leaf;
}

// The focus may sit on a container, so most guards ask only that it
// still names something.
bool validNode(const Node& root, const Path& p) {
  const Node* n = at(root, p);
  return n && n->kind != Node::Kind::Empty;
}

// Repairs focus after surgery: keep it if it still names a leaf, else
// fall back to `aim` (and then to the first leaf of the tree).
void refocus(Tab& t, const Path& aim) {
  if (validNode(t.root, t.focused)) return;
  t.extend = 0;
  Path p = descendFirst(t.root, aim);
  if (validLeaf(t.root, p)) {
    t.focused = p;
    return;
  }
  t.focused = descendFirst(t.root, {});
  if (!validLeaf(t.root, t.focused)) t.focused.clear();
}

// Takes the leaf at `p` out of the tree, handing its share to its
// siblings and collapsing a container left holding one child. `aim` is
// where focus should land if it was the leaf that went.
void detachAt(Tab& t, const Path& p, Path& aim) {
  if (p.empty()) {
    t.root = Node{};
    aim.clear();
    return;
  }
  Path pp(p.begin(), p.end() - 1);
  int idx = p.back();
  Node* par = at(t.root, pp);
  if (!par || par->kind != Node::Kind::Split ||
      idx >= (int)par->children.size())
    return;
  par->children.erase(par->children.begin() + idx);
  if (idx < (int)par->fractions.size())
    par->fractions.erase(par->fractions.begin() + idx);
  normalize(*par);
  aim = pp;
  if (par->children.size() == 1) {
    Node only = std::move(par->children[0]);
    *par = std::move(only);  // a container of one is just that one
  } else if (par->children.empty()) {
    *par = Node{};
  } else {
    aim.push_back(std::min(idx, (int)par->children.size() - 1));
  }
}

}  // namespace

std::string windowId(const WindowRef& w) {
  if (w.kind == WindowRef::Kind::Overview) return "@overview";
  return w.unit + "/" + w.panel;
}

std::string windowTitle(const WindowRef& w) {
  return w.kind == WindowRef::Kind::Overview ? "overview" : w.panel;
}

WindowRef overviewWindow() {
  WindowRef w;
  w.kind = WindowRef::Kind::Overview;
  return w;
}

Split axisOf(Dir d) {
  return (d == Dir::Left || d == Dir::Right) ? Split::H : Split::V;
}
int stepOf(Dir d) { return (d == Dir::Left || d == Dir::Up) ? -1 : 1; }

const Node* at(const Node& root, const Path& p) {
  const Node* n = &root;
  for (int i : p) {
    if (n->kind != Node::Kind::Split || i < 0 ||
        i >= (int)n->children.size())
      return nullptr;
    n = &n->children[(size_t)i];
  }
  return n;
}

Node* at(Node& root, const Path& p) {
  return const_cast<Node*>(at((const Node&)root, p));
}

std::string tabLabel(const Tab& t) {
  std::string s = std::to_string(t.index);
  if (!t.name.empty()) s += ":" + t.name;
  return s;
}

std::vector<WindowRef> windowsIn(const Tab& t) {
  std::vector<WindowRef> out;
  collectWindows(t.root, out);
  return out;
}

std::vector<WindowRef> windowsIn(const std::vector<Tab>& tabs) {
  std::vector<WindowRef> out;
  for (const Tab& t : tabs) collectWindows(t.root, out);
  return out;
}

std::optional<Path> findWindow(const Tab& t, const WindowRef& w) {
  Path p;
  if (!findPath(t.root, w, p)) return std::nullopt;
  return p;
}

bool hasWindow(const Tab& t, const WindowRef& w) {
  return findWindow(t, w).has_value();
}

std::optional<WindowRef> focusedWindow(const Tab& t) {
  const Node* n = at(t.root, t.focused);
  if (!n || n->kind != Node::Kind::Leaf) return std::nullopt;
  return n->window;
}

std::vector<WindowRef> focusedWindows(const Tab& t) {
  std::vector<WindowRef> out;
  if (const Node* n = at(t.root, t.focused)) collectWindows(*n, out);
  return out;
}

bool focusIsContainer(const Tab& t) {
  const Node* n = at(t.root, t.focused);
  return n && n->kind == Node::Kind::Split;
}

bool focusParent(Tab& t) {
  if (t.empty() || t.focused.empty()) return false;
  t.focused.pop_back();
  t.extend = 0;  // the run belonged to the level we just left
  return true;
}

bool focusChild(Tab& t) {
  const Node* n = at(t.root, t.focused);
  if (!n || n->kind != Node::Kind::Split || n->children.empty()) return false;
  t.focused.push_back(0);
  t.extend = 0;
  return true;
}

Run focusedRun(const Tab& t) {
  Run r;
  if (t.empty() || t.focused.empty()) return r;  // the root has no siblings
  if (!validNode(t.root, t.focused)) return r;
  r.parent = Path(t.focused.begin(), t.focused.end() - 1);
  const Node* par = at(t.root, r.parent);
  if (!par || par->kind != Node::Kind::Split) return r;
  int i = t.focused.back();
  int j = std::clamp(i + t.extend, 0, (int)par->children.size() - 1);
  r.from = std::min(i, j);
  r.to = std::max(i, j);
  r.valid = true;
  return r;
}

bool inRun(const Run& r, const Path& path) {
  if (!r.valid || r.count() < 2) return false;
  if (path.size() != r.parent.size() + 1) return false;
  if (!std::equal(r.parent.begin(), r.parent.end(), path.begin())) return false;
  int i = path.back();
  return i >= r.from && i <= r.to;
}

bool extendSelection(Tab& t, Dir d) {
  Run r = focusedRun(t);
  if (!r.valid) return false;
  const Node* par = at(t.root, r.parent);
  // A run is a run of one container's children, so it can only grow the
  // way that container is split.
  if (!par || par->split != axisOf(d)) return false;
  int step = stepOf(d);
  int at_ = t.focused.back();
  int want = at_ + t.extend + step;
  if (want < 0 || want >= (int)par->children.size()) return false;
  t.extend += step;
  return true;
}

void clearSelection(Tab& t) { t.extend = 0; }

bool focusWindow(Tab& t, const WindowRef& w) {
  auto p = findWindow(t, w);
  if (!p) return false;
  t.focused = *p;
  return true;
}

// Puts a subtree where the focus says it goes. Every insertion in the
// app funnels through here, so a window and a whole container arriving
// from another tab land by the same rules.
bool insertNode(Tab& t, Node n) {
  if (n.kind == Node::Kind::Empty) return false;
  if (t.empty()) {
    t.root = std::move(n);
    t.focused.clear();
    t.extend = 0;
    t.pendingSplit.reset();
    return true;
  }
  if (!validNode(t.root, t.focused)) refocus(t, {});
  Node* f = at(t.root, t.focused);
  if (!f) return false;
  t.extend = 0;

  if (t.pendingSplit) {
    // `split v` then open: the focused node is wrapped in a new
    // container and the newcomer joins it there.
    Node wrapper;
    wrapper.kind = Node::Kind::Split;
    wrapper.split = *t.pendingSplit;
    wrapper.children = {std::move(*f), std::move(n)};
    wrapper.fractions = {0.5, 0.5};
    *f = std::move(wrapper);
    t.focused.push_back(1);
    t.pendingSplit.reset();
    return true;
  }

  // A container is focused: the newcomer joins it, as in i3.
  if (f->kind == Node::Kind::Split) {
    double share = 1.0 / (double)(f->children.size() + 1);
    for (double& fr : f->fractions) fr *= 1.0 - share;
    f->children.push_back(std::move(n));
    f->fractions.push_back(share);
    normalize(*f);
    t.focused.push_back((int)f->children.size() - 1);
    return true;
  }

  if (t.focused.empty()) {
    // A one-window tab: the root becomes the container.
    Node old = std::move(t.root);
    Node wrapper;
    wrapper.kind = Node::Kind::Split;
    wrapper.split = Split::H;
    wrapper.children = {std::move(old), std::move(n)};
    wrapper.fractions = {0.5, 0.5};
    t.root = std::move(wrapper);
    t.focused = {1};
    return true;
  }

  Path pp(t.focused.begin(), t.focused.end() - 1);
  int idx = t.focused.back();
  Node* par = at(t.root, pp);
  double share = par->fractions[(size_t)idx] * 0.5;
  par->fractions[(size_t)idx] = share;
  par->children.insert(par->children.begin() + idx + 1, std::move(n));
  par->fractions.insert(par->fractions.begin() + idx + 1, share);
  normalize(*par);
  t.focused = pp;
  t.focused.push_back(idx + 1);
  return true;
}

bool insertWindow(Tab& t, const WindowRef& w) {
  if (auto p = findWindow(t, w)) {
    t.focused = *p;
    t.extend = 0;
    return false;
  }
  return insertNode(t, leafNode(w));
}

namespace {

// i3's `default_orientation auto`, as a decision about the next
// insertion: split the focused node along its longer side.
void autoSplit(Tab& t, Rect area, const LayoutMetrics& m) {
  if (t.empty() || t.pendingSplit) return;
  Placement p = place(t, area, m);
  // The 1.2 is deliberate hysteresis: a nearly square window splits
  // downwards, which reads better than a hair-thin preference flipping
  // with the window size.
  auto pick = [&t](const Rect& r) {
    t.pendingSplit = r.w >= r.h * 1.2f ? Split::H : Split::V;
  };
  for (const PlacedWindow& pw : p.windows)
    if (pw.focused) pick(pw.rect);
  for (const PlacedSplit& ps : p.splits)
    if (ps.focused) pick(ps.rect);
}

}  // namespace

bool insertNodeAuto(Tab& t, Node n, Rect area, const LayoutMetrics& m) {
  autoSplit(t, area, m);
  return insertNode(t, std::move(n));
}

bool insertWindowAuto(Tab& t, const WindowRef& w, Rect area,
                      const LayoutMetrics& m) {
  if (hasWindow(t, w)) return insertWindow(t, w);  // just focus it
  autoSplit(t, area, m);
  return insertWindow(t, w);
}

bool removeWindow(Tab& t, const WindowRef& w) {
  auto p = findWindow(t, w);
  if (!p) return false;
  bool wasFocused = t.focused == *p;
  Path aim;
  detachAt(t, *p, aim);
  if (t.empty()) {
    t.focused.clear();
    t.pendingSplit.reset();
    return true;
  }
  if (wasFocused) t.focused = aim;
  refocus(t, aim);
  return true;
}

bool focusDir(Tab& t, Dir d) {
  if (t.empty()) return false;
  if (!validNode(t.root, t.focused)) refocus(t, {});
  t.extend = 0;
  Split axis = axisOf(d);
  int step = stepOf(d);
  Path p = t.focused;
  for (int i = (int)p.size() - 1; i >= 0; i--) {
    Path pp(p.begin(), p.begin() + i);
    const Node* par = at(t.root, pp);
    if (!par || par->split != axis) continue;
    int ni = p[(size_t)i] + step;
    if (ni < 0 || ni >= (int)par->children.size()) continue;
    Path np = pp;
    np.push_back(ni);
    t.focused = descendEdge(t.root, np, axis, step);
    return true;
  }
  return false;
}

bool moveDir(Tab& t, Dir d) {
  if (t.empty()) return false;
  if (!validNode(t.root, t.focused)) refocus(t, {});
  if (windowsIn(t).size() < 2) return false;
  Split axis = axisOf(d);
  int step = stepOf(d);
  Path p = t.focused;

  for (int i = (int)p.size() - 1; i >= 0; i--) {
    Path pp(p.begin(), p.begin() + i);
    Node* par = at(t.root, pp);
    if (!par || par->split != axis) continue;
    int ci = p[(size_t)i], ni = ci + step;
    if (ni < 0 || ni >= (int)par->children.size()) continue;
    std::swap(par->children[(size_t)ci], par->children[(size_t)ni]);
    std::swap(par->fractions[(size_t)ci], par->fractions[(size_t)ni]);
    p[(size_t)i] = ni;
    t.focused = p;
    return true;
  }

  // Nothing that way anywhere up the tree. The focus being the whole
  // tree is the end of it: there is no outside to move it to, and
  // detaching it would leave the root empty inside its own wrapper.
  if (p.empty()) return false;
  // Otherwise the node leaves its container and re-attaches at that end
  // of the root.
  const Node* moving = at(t.root, p);
  if (!moving || moving->kind == Node::Kind::Empty) return false;
  Node subtree = *moving;
  Path aim;
  detachAt(t, p, aim);
  if (t.root.kind == Node::Kind::Split && t.root.split == axis) {
    size_t pos = step > 0 ? t.root.children.size() : 0;
    double share = 1.0 / (double)(t.root.children.size() + 1);
    for (double& f : t.root.fractions) f *= 1.0 - share;
    t.root.children.insert(t.root.children.begin() + (long)pos,
                           std::move(subtree));
    t.root.fractions.insert(t.root.fractions.begin() + (long)pos, share);
    normalize(t.root);
    t.focused = {(int)pos};
  } else {
    Node old = t.root;
    Node wrapper;
    wrapper.kind = Node::Kind::Split;
    wrapper.split = axis;
    if (step > 0)
      wrapper.children = {std::move(old), std::move(subtree)};
    else
      wrapper.children = {std::move(subtree), std::move(old)};
    wrapper.fractions = {0.5, 0.5};
    t.root = std::move(wrapper);
    t.focused = {step > 0 ? 1 : 0};
  }
  return true;
}

bool groupSelection(Tab& t, Split s) {
  if (t.empty()) return false;
  Run r = focusedRun(t);
  // The root, or a run of one: there is nothing to gather, so this is
  // i3's plain `split` - it decides where the *next* window goes.
  if (!r.valid || r.count() == 1) {
    t.pendingSplit = s;
    return true;
  }
  Node* par = at(t.root, r.parent);
  if (!par || par->kind != Node::Kind::Split) return false;
  if (r.count() == (int)par->children.size()) {
    // Everything in the container: wrapping it would nest a copy of the
    // container inside itself, so re-orient it instead.
    par->split = s;
    t.extend = 0;
    return true;
  }

  Node group;
  group.kind = Node::Kind::Split;
  group.split = s;
  double total = 0;
  for (int i = r.from; i <= r.to; i++) {
    group.children.push_back(std::move(par->children[(size_t)i]));
    group.fractions.push_back(par->fractions[(size_t)i]);
    total += par->fractions[(size_t)i];
  }
  normalize(group);
  par->children.erase(par->children.begin() + r.from,
                      par->children.begin() + r.to + 1);
  par->fractions.erase(par->fractions.begin() + r.from,
                       par->fractions.begin() + r.to + 1);
  par->children.insert(par->children.begin() + r.from, std::move(group));
  par->fractions.insert(par->fractions.begin() + r.from, total);
  normalize(*par);
  // The new container is what you just made, so it is what is focused -
  // ready to be moved, resized or flattened straight back.
  t.focused = r.parent;
  t.focused.push_back(r.from);
  t.extend = 0;
  return true;
}

bool flattenFocused(Tab& t) {
  if (t.empty() || t.focused.empty()) return false;  // the root has no parent
  Node* n = at(t.root, t.focused);
  if (!n || n->kind != Node::Kind::Split) return false;
  Path pp(t.focused.begin(), t.focused.end() - 1);
  Node* par = at(t.root, pp);
  if (!par || par->kind != Node::Kind::Split) return false;
  int idx = t.focused.back();

  // Taken out before the erase below reaches them; the container's own
  // share is what its children divide up in its place.
  std::vector<Node> kids = std::move(n->children);
  std::vector<double> shares = n->fractions;
  double had = par->fractions[(size_t)idx];
  par->children.erase(par->children.begin() + idx);
  par->fractions.erase(par->fractions.begin() + idx);
  for (size_t k = 0; k < kids.size(); k++) {
    par->children.insert(par->children.begin() + idx + (long)k,
                         std::move(kids[k]));
    par->fractions.insert(par->fractions.begin() + idx + (long)k,
                          had * (k < shares.size() ? shares[k] : 0.0));
  }
  normalize(*par);
  t.focused = pp;
  t.focused.push_back(idx);
  t.extend = 0;
  return true;
}

bool closeFocused(Tab& t) {
  if (t.empty()) return false;
  Path aim;
  detachAt(t, t.focused, aim);
  t.extend = 0;
  if (t.empty()) {
    t.focused.clear();
    t.pendingSplit.reset();
    return true;
  }
  t.focused = aim;
  refocus(t, aim);
  return true;
}

bool resizeSplit(Tab& t, const Path& splitPath, size_t boundary,
                 double delta) {
  Node* n = at(t.root, splitPath);
  if (!n || n->kind != Node::Kind::Split) return false;
  if (boundary + 1 >= n->children.size()) return false;
  double a = n->fractions[boundary], b = n->fractions[boundary + 1];
  double want = std::clamp(delta, -(a - kMinFraction), b - kMinFraction);
  if (std::fabs(want) < 1e-9) return false;
  n->fractions[boundary] = a + want;
  n->fractions[boundary + 1] = b - want;
  return true;
}

bool resizeFocused(Tab& t, Dir d, double delta) {
  if (t.empty() || !validNode(t.root, t.focused)) return false;
  Split axis = axisOf(d);
  int step = stepOf(d);
  Path p = t.focused;
  for (int i = (int)p.size() - 1; i >= 0; i--) {
    Path pp(p.begin(), p.begin() + i);
    const Node* par = at(t.root, pp);
    if (!par || par->split != axis || par->children.size() < 2) continue;
    int ci = p[(size_t)i], last = (int)par->children.size() - 1;
    // Push the boundary on the side we are growing towards; at the end
    // of the row, pull the one behind us instead.
    if (step > 0) {
      if (ci < last) return resizeSplit(t, pp, (size_t)ci, delta);
      return resizeSplit(t, pp, (size_t)(ci - 1), -delta);
    }
    if (ci > 0) return resizeSplit(t, pp, (size_t)(ci - 1), -delta);
    return resizeSplit(t, pp, (size_t)ci, delta);
  }
  return false;
}

namespace {

// Whether `path` sits inside the focused subtree (but is not the
// focused node itself), which is how a window says "the container I am
// in has the keyboard".
bool under(const Path& focus, const Path& path) {
  return path.size() > focus.size() &&
         std::equal(focus.begin(), focus.end(), path.begin());
}

void placeNode(const Node& n, Rect r, int depth, const Path& path,
               const Tab& t, const Run& run, const LayoutMetrics& m,
               Placement& out) {
  if (n.kind == Node::Kind::Empty) return;
  if (n.kind == Node::Kind::Leaf) {
    PlacedWindow pw;
    pw.window = n.window;
    pw.rect = r;
    pw.depth = depth;
    pw.path = path;
    pw.focused = path == t.focused;
    pw.selected = inRun(run, path);
    pw.inFocus = under(t.focused, path);
    out.windows.push_back(std::move(pw));
    return;
  }
  out.splits.push_back(PlacedSplit{r, n.split, depth, path,
                                   path == t.focused, inRun(run, path)});
  Rect inner{r.x + m.pad, r.y + m.pad, std::max(0.0f, r.w - 2 * m.pad),
             std::max(0.0f, r.h - 2 * m.pad)};
  size_t k = n.children.size();
  if (k == 0) return;
  bool horiz = n.split == Split::H;
  float length = horiz ? inner.w : inner.h;
  float avail = std::max(0.0f, length - m.gap * (float)(k - 1));
  // Positions come from cumulative fractions, so the children tile the
  // container exactly instead of drifting by a pixel per child.
  double acc = 0;
  for (size_t i = 0; i < k; i++) {
    double from = acc;
    acc += i < n.fractions.size() ? n.fractions[i] : 0.0;
    float a = (float)from * avail + m.gap * (float)i;
    float b = (float)acc * avail + m.gap * (float)i;
    if (i + 1 == k) b = avail + m.gap * (float)i;  // no rounding at the end
    Rect cr = horiz ? Rect{inner.x + a, inner.y, b - a, inner.h}
                    : Rect{inner.x, inner.y + a, inner.w, b - a};
    Path cp = path;
    cp.push_back((int)i);
    placeNode(n.children[i], cr, depth + 1, cp, t, run, m, out);
    if (i + 1 < k) {
      float mid = (horiz ? inner.x : inner.y) + b + m.gap * 0.5f;
      Rect handle = horiz
                        ? Rect{mid - m.grab * 0.5f, inner.y, m.grab, inner.h}
                        : Rect{inner.x, mid - m.grab * 0.5f, inner.w, m.grab};
      out.dividers.push_back(Divider{handle, n.split, path, i, avail});
    }
  }
}

}  // namespace

Placement place(const Tab& t, Rect area, const LayoutMetrics& m) {
  Placement out;
  placeNode(t.root, area, 0, {}, t, focusedRun(t), m, out);
  return out;
}

Tab* findTab(std::vector<Tab>& tabs, int index) {
  for (Tab& t : tabs)
    if (t.index == index) return &t;
  return nullptr;
}

const Tab* findTab(const std::vector<Tab>& tabs, int index) {
  for (const Tab& t : tabs)
    if (t.index == index) return &t;
  return nullptr;
}

Tab& ensureTab(std::vector<Tab>& tabs, int index) {
  if (Tab* t = findTab(tabs, index)) return *t;
  Tab t;
  t.index = index;
  auto pos = std::lower_bound(
      tabs.begin(), tabs.end(), index,
      [](const Tab& a, int i) { return a.index < i; });
  return *tabs.insert(pos, std::move(t));
}

int nextFreeTabIndex(const std::vector<Tab>& tabs) {
  for (int i = 1;; i++)
    if (!findTab(tabs, i)) return i;
}

int tabHolding(const std::vector<Tab>& tabs, const WindowRef& w) {
  for (const Tab& t : tabs)
    if (hasWindow(t, w)) return t.index;
  return 0;
}

bool sendWindowToTab(std::vector<Tab>& tabs, const WindowRef& w, int to,
                     Rect area, const LayoutMetrics& m) {
  int from = tabHolding(tabs, w);
  if (from == 0) return false;
  if (from == to) return true;
  removeWindow(*findTab(tabs, from), w);
  insertWindowAuto(ensureTab(tabs, to), w, area, m);
  return true;
}

bool sendFocusedToTab(std::vector<Tab>& tabs, int from, int to, Rect area,
                      const LayoutMetrics& m) {
  if (from == to) return false;
  Tab* src = findTab(tabs, from);
  if (!src || src->empty()) return false;
  const Node* n = at(src->root, src->focused);
  if (!n || n->kind == Node::Kind::Empty) return false;
  // The subtree travels intact: a container sent to another tab arrives
  // laid out the way it was, not taken apart into its windows.
  Node subtree = *n;
  Path aim;
  detachAt(*src, src->focused, aim);
  src->extend = 0;
  src->focused = aim;
  refocus(*src, aim);
  insertNodeAuto(ensureTab(tabs, to), std::move(subtree), area, m);
  return true;
}

bool closeWindowEverywhere(std::vector<Tab>& tabs, const WindowRef& w) {
  bool any = false;
  for (Tab& t : tabs) any = removeWindow(t, w) || any;
  return any;
}

namespace {

json::Value windowToJson(const WindowRef& w) {
  json::Value v = json::makeObject();
  v.set("kind", json::makeString(w.kind == WindowRef::Kind::Overview
                                     ? "overview"
                                     : "panel"));
  if (w.kind == WindowRef::Kind::Panel) {
    v.set("unit", json::makeString(w.unit));
    v.set("panel", json::makeString(w.panel));
  }
  return v;
}

bool windowFromJson(const json::Value& v, WindowRef& out) {
  if (v.kind != json::Value::Kind::Object) return false;
  std::string kind = v.getString("kind", "panel");
  if (kind == "overview") {
    out = overviewWindow();
    return true;
  }
  WindowRef w;
  w.kind = WindowRef::Kind::Panel;
  w.unit = v.getString("unit");
  w.panel = v.getString("panel");
  if (w.panel.empty()) return false;  // a panel with no name names nothing
  out = std::move(w);
  return true;
}

json::Value nodeToJson(const Node& n) {
  if (n.kind == Node::Kind::Empty) return json::makeNull();
  json::Value v = json::makeObject();
  if (n.kind == Node::Kind::Leaf) {
    v.set("window", windowToJson(n.window));
    return v;
  }
  v.set("split", json::makeString(n.split == Split::H ? "h" : "v"));
  json::Value fr = json::makeArray(), ch = json::makeArray();
  for (double f : n.fractions) fr.array.push_back(json::makeNumber(f));
  for (const Node& c : n.children) ch.array.push_back(nodeToJson(c));
  v.set("fractions", std::move(fr));
  v.set("children", std::move(ch));
  return v;
}

// Reads a subtree, dropping anything malformed rather than failing the
// load: a container whose children all fell away disappears, and one
// left holding a single child collapses into it. `seen` keeps a window
// from appearing twice in one tab, which every operation here assumes.
Node nodeFromJson(const json::Value& v, std::set<std::string>& seen) {
  Node n;
  if (v.kind != json::Value::Kind::Object) return n;
  if (const json::Value* w = v.get("window")) {
    WindowRef ref;
    if (!windowFromJson(*w, ref)) return n;
    if (!seen.insert(windowId(ref)).second) return n;
    n.kind = Node::Kind::Leaf;
    n.window = std::move(ref);
    return n;
  }
  const json::Value* ch = v.get("children");
  if (!ch || ch->kind != json::Value::Kind::Array) return n;
  const json::Value* fr = v.get("fractions");
  Node split;
  split.kind = Node::Kind::Split;
  split.split = v.getString("split", "h") == "v" ? Split::V : Split::H;
  for (size_t i = 0; i < ch->array.size(); i++) {
    Node c = nodeFromJson(ch->array[i], seen);
    if (c.kind == Node::Kind::Empty) continue;
    double f = 0;
    if (fr && fr->kind == json::Value::Kind::Array && i < fr->array.size() &&
        fr->array[i].kind == json::Value::Kind::Number)
      f = fr->array[i].number;
    split.children.push_back(std::move(c));
    split.fractions.push_back(f);
  }
  if (split.children.empty()) return n;
  if (split.children.size() == 1) return std::move(split.children[0]);
  normalize(split);
  return split;
}

}  // namespace

Tab migratedTab(const std::vector<WindowRef>& all,
                const std::map<std::string, bool>& openWindows, Rect area) {
  Tab t;
  insertWindow(t, overviewWindow());
  for (const WindowRef& w : all) {
    if (w.kind == WindowRef::Kind::Overview) continue;
    auto it = openWindows.find(windowId(w));
    if (it == openWindows.end() || it->second) insertWindowAuto(t, w, area);
  }
  focusWindow(t, overviewWindow());
  return t;
}

json::Value toJson(const Tab& t) {
  json::Value v = json::makeObject();
  v.set("index", json::makeNumber(t.index));
  if (!t.name.empty()) v.set("name", json::makeString(t.name));
  json::Value focus = json::makeArray();
  for (int i : t.focused) focus.array.push_back(json::makeNumber(i));
  v.set("focused", std::move(focus));
  v.set("root", nodeToJson(t.root));
  return v;
}

bool tabFromJson(const json::Value& v, Tab& out) {
  if (v.kind != json::Value::Kind::Object) return false;
  int index = (int)v.getNumber("index", 0);
  if (index <= 0) return false;
  Tab t;
  t.index = index;
  t.name = v.getString("name");
  std::set<std::string> seen;
  if (const json::Value* root = v.get("root")) t.root = nodeFromJson(*root, seen);
  if (const json::Value* f = v.get("focused");
      f && f->kind == json::Value::Kind::Array)
    for (const json::Value& i : f->array)
      if (i.kind == json::Value::Kind::Number) t.focused.push_back((int)i.number);
  refocus(t, {});
  out = std::move(t);
  return true;
}

}  // namespace synth::devapp
