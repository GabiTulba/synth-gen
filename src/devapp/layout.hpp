#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace synth::devapp {

// The dev app's window manager, modelled on i3: numbered tabs, each
// holding a tree of split containers whose leaves are windows. Nothing
// here draws or knows about ImGui - it is geometry and tree surgery, so
// every rule below is checked headless in tests/devapp/test_layout.cpp.

// What a leaf window shows. A panel is identified the way the rest of
// the app identifies one: the unit it belongs to (unitKey()) and the
// panel's name. There is exactly one Overview window, which is why it
// carries no unit - it lists every unit at once.
struct WindowRef {
  enum class Kind { Overview, Panel };
  Kind kind = Kind::Panel;
  std::string unit;
  std::string panel;
  bool operator==(const WindowRef&) const = default;
};

// "unit/panel" - stable across rebuilds, and what the panel-open map and
// the ImGui window ids are keyed by.
std::string windowId(const WindowRef& w);
// What the window's title bar says.
std::string windowTitle(const WindowRef& w);
WindowRef overviewWindow();

enum class Split { H, V };  // children side by side / stacked
enum class Dir { Left, Right, Up, Down };

Split axisOf(Dir d);         // Left/Right -> H, Up/Down -> V
int stepOf(Dir d);           // Left/Up -> -1, Right/Down -> +1

// A node is a window, a split with two or more children, or nothing at
// all (only ever the root of an empty tab). Splits carry one fraction
// per child, summing to 1.
struct Node {
  enum class Kind { Empty, Leaf, Split };
  Kind kind = Kind::Empty;
  WindowRef window;              // Leaf
  ::synth::devapp::Split split = Split::H;  // Split
  std::vector<Node> children;    // Split
  std::vector<double> fractions;  // Split, parallel to children
  bool operator==(const Node&) const = default;
};

// A path is the child index at each level from the root down; the empty
// path is the root itself.
using Path = std::vector<int>;
const Node* at(const Node& root, const Path& p);
Node* at(Node& root, const Path& p);

struct Tab {
  int index = 1;       // the number you press to reach it
  std::string name;    // user-set; empty shows the index alone
  Node root;
  // The focused node. Usually a leaf, but focusParent walks it up to a
  // container, and then everything - move, resize, close, send to a tab,
  // flatten - applies to that whole subtree, as in i3.
  Path focused;
  // How many siblings *after* the focused node are selected with it
  // (negative selects the ones before). A run of adjacent siblings is
  // all grouping ever needs, and it is the only shape the tree can
  // group without reordering anything.
  int extend = 0;
  // i3's `split h` / `split v`: set on the focused node, consumed by the
  // next insertion, which wraps that node in a new container.
  std::optional<Split> pendingSplit;
  bool empty() const { return root.kind == Node::Kind::Empty; }
  // Only what is worth saving: the selection and the pending split are
  // this moment's business, and comparing them would rewrite the
  // settings file over a keystroke that changed nothing on disk.
  bool operator==(const Tab& o) const {
    return index == o.index && name == o.name && root == o.root &&
           focused == o.focused;
  }
};

// The tab's label in the bar: "2" or "2:drums".
std::string tabLabel(const Tab& t);

// Every window in the tab, in layout order (left to right, top to
// bottom); the vector overload is every window on screen at all.
std::vector<WindowRef> windowsIn(const Tab& t);
std::vector<WindowRef> windowsIn(const std::vector<Tab>& tabs);
// The path of a window, or nullopt when the tab does not hold it.
std::optional<Path> findWindow(const Tab& t, const WindowRef& w);
bool hasWindow(const Tab& t, const WindowRef& w);
// The focused leaf's window - nullopt for an empty tab, and nullopt
// when a container is focused rather than one window.
std::optional<WindowRef> focusedWindow(const Tab& t);
// Every window under the focus, which for a leaf is the one window and
// for a container is everything it holds.
std::vector<WindowRef> focusedWindows(const Tab& t);
// Whether the focus is on a container rather than a single window.
bool focusIsContainer(const Tab& t);

// i3's `focus parent` / `focus child`: walk the focus up to the
// container holding it, and back down into it. False at the root and at
// a leaf respectively. Walking up clears any selection, since the run
// was a run of the level you just left.
bool focusParent(Tab& t);
bool focusChild(Tab& t);

// The run of siblings the focus covers: the focused node's parent, and
// the inclusive range of child indices. `count` is 1 unless the
// selection has been extended.
struct Run {
  Path parent;
  int from = 0, to = 0;
  bool valid = false;
  int count() const { return to - from + 1; }
};
Run focusedRun(const Tab& t);
// Whether `path` is one of the run's members. A run of one is just the
// focus, not a selection, so nothing is ever both focused and selected.
bool inRun(const Run& r, const Path& path);

// Extends (or pulls back) the selection to the next sibling that way.
// Only moves along the parent's own axis - a run of siblings has no
// meaning across it - and stops at the ends. False when it cannot.
bool extendSelection(Tab& t, Dir d);
void clearSelection(Tab& t);

// Wraps the selected run in a new container of the given orientation,
// which is what "group these two side by side" means. Three cases, all
// of which avoid leaving a redundant level in the tree:
//   - a run of one is i3's `split`: nothing is wrapped, and the next
//     window opened here goes that way instead;
//   - a run covering all of its parent's children re-orients the parent
//     rather than nesting a copy of it inside itself;
//   - anything else becomes a new container, focused, holding the run.
bool groupSelection(Tab& t, Split s);

// Dissolves the focused container into its parent: its children take
// its place, sharing out the space it had. This is the way back from a
// grouping. False unless a container with a parent is focused - the
// root has nowhere to dissolve into, and a leaf is not a container.
bool flattenFocused(Tab& t);
// Moves focus onto a window this tab holds. False if it does not.
bool focusWindow(Tab& t, const WindowRef& w);

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  bool operator==(const Rect&) const = default;
};

struct LayoutMetrics {
  float gap = 6;   // space between siblings, where a divider is dragged
  float pad = 3;   // inset a container takes for its border and gutter
  float grab = 8;  // how wide a divider's grab handle is
};

// --- tree surgery -----------------------------------------------------
//
// Inserts a window beside the focused leaf, i3-style: into an empty tab
// it becomes the root; with a pending split the focused leaf is wrapped
// in a new container holding both; otherwise it joins the focused leaf's
// parent right after it, taking half of that leaf's share. Focus follows
// the new window. False (with focus moved onto it) if the tab already
// holds this window.
bool insertWindow(Tab& t, const WindowRef& w);

// Puts a whole subtree where the focus says it goes - what a window
// insertion is made of, and what moving a container between tabs uses.
// With a container focused the newcomer joins it, as in i3.
bool insertNode(Tab& t, Node n);
bool insertNodeAuto(Tab& t, Node n, Rect area, const LayoutMetrics& m = {});

// insertWindow, picking the split for a new container the way i3's
// `default_orientation auto` does: along the longer side of the focused
// window as `area` would lay it out, so opening four windows gives a
// grid rather than four slivers in a row. An explicit pendingSplit
// still wins.
bool insertWindowAuto(Tab& t, const WindowRef& w, Rect area,
                      const LayoutMetrics& m = {});

// Drops a window: the leaf goes, its share is handed to its siblings, a
// container left with one child collapses into it, and focus falls to
// the nearest remaining leaf. False if the tab does not hold it.
bool removeWindow(Tab& t, const WindowRef& w);

// Moves focus one window in a direction: climb until an ancestor splits
// along that axis and has a neighbour that way, then descend to the leaf
// nearest the crossed edge. False when there is nothing that way.
bool focusDir(Tab& t, Dir d);

// Moves the focused window: it swaps with the neighbour that way if
// there is one; at the tree's edge along that axis it is promoted to the
// root instead - the root becomes (or already is) a split along the axis
// and the window is re-attached at that end. False only when the tab
// holds fewer than two windows.
bool moveDir(Tab& t, Dir d);

// No child of a split may be squeezed below this share of its parent.
inline constexpr double kMinFraction = 0.05;

// Moves `delta` of the parent's length across one boundary of a split,
// growing the child before it. Clamped so neither side goes below
// kMinFraction. `boundary` is the gap between children[boundary] and
// children[boundary + 1].
bool resizeSplit(Tab& t, const Path& splitPath, size_t boundary, double delta);

// Grows (delta > 0) or shrinks the focused window along a direction, by
// the nearest ancestor that splits that way.
bool resizeFocused(Tab& t, Dir d, double delta);

// --- placement --------------------------------------------------------

struct PlacedWindow {
  WindowRef window;
  Rect rect;
  int depth = 0;  // how many containers deep, for the gutter strips
  Path path;
  bool focused = false;   // this exact node has the focus
  bool selected = false;  // it is part of the selected run
  bool inFocus = false;   // it sits inside the focused container
};

struct PlacedSplit {
  Rect rect;
  Split split = Split::H;
  int depth = 0;
  Path path;
  bool focused = false;
  bool selected = false;
};

// One draggable boundary: the handle's rect, and which gap of which
// split it moves.
struct Divider {
  Rect rect;
  Split split = Split::H;
  Path path;
  size_t boundary = 0;
  // The length the fractions divide, so a drag in pixels becomes the
  // fraction this boundary should move by.
  float span = 0;
};

struct Placement {
  std::vector<PlacedWindow> windows;
  std::vector<PlacedSplit> splits;
  std::vector<Divider> dividers;
};

// Lays the tab out inside `area`. Leaf rects tile it exactly: siblings
// divide their container's length by fraction, minus one gap between
// each pair, and every container insets itself by `pad` first.
Placement place(const Tab& t, Rect area, const LayoutMetrics& m = {});

// --- the set of tabs --------------------------------------------------

Tab* findTab(std::vector<Tab>& tabs, int index);
const Tab* findTab(const std::vector<Tab>& tabs, int index);
// The tab with this index, created (in index order) if it is missing.
Tab& ensureTab(std::vector<Tab>& tabs, int index);
// The lowest index no tab is using.
int nextFreeTabIndex(const std::vector<Tab>& tabs);
// Which tab holds a window, or 0 for none.
int tabHolding(const std::vector<Tab>& tabs, const WindowRef& w);
// Takes a window out of whatever tab holds it and inserts it into `to`,
// creating that tab if needed and tiling it there the way
// insertWindowAuto does. False if no tab holds it.
bool sendWindowToTab(std::vector<Tab>& tabs, const WindowRef& w, int to,
                     Rect area, const LayoutMetrics& m = {});

// The same for whatever is focused in `from`, which may be a whole
// container: the subtree moves across intact rather than being taken
// apart into its windows.
bool sendFocusedToTab(std::vector<Tab>& tabs, int from, int to, Rect area,
                      const LayoutMetrics& m = {});

// Closes everything the focus covers - one window, or every window in
// the focused container. False when the tab is empty.
bool closeFocused(Tab& t);
// Drops a window from every tab that holds it.
bool closeWindowEverywhere(std::vector<Tab>& tabs, const WindowRef& w);

// --- persistence ------------------------------------------------------

// The tab to start from when a settings file predates tabs: the
// overview plus every window that was open, tiled. A window the map has
// never heard of counts as open, which is how the flat panel row it
// replaces behaved.
Tab migratedTab(const std::vector<WindowRef>& all,
                const std::map<std::string, bool>& openWindows, Rect area);

json::Value toJson(const Tab& t);
// False (leaving `out` untouched) when the value is not a tab; a
// malformed subtree is dropped rather than fatal, as everywhere else in
// the loader.
bool tabFromJson(const json::Value& v, Tab& out);

}  // namespace synth::devapp
