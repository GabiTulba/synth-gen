#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "layout.hpp"
#include "metadata.hpp"

namespace synth::devapp {

// What is inside a window, what can be searched for, and how the hint
// labels are handed out. One enumeration serves all three, so the labels
// a window shows, the things search can find in it and the widgets the
// keyboard steps through can never disagree.

// One row of a window, in the order it is drawn. Panel windows are made
// of controls, groups, the lanes inside a group, and targets; the
// overview is made of the panels themselves, one tickbox each.
//
// A group is a row of its own - the budget bar it draws - and each of
// its lanes is a row under it, because a lane is the thing with a value
// to select and nudge.
struct WindowElement {
  enum class Kind { Control, Group, Lane, Target, Panel };
  Kind kind = Kind::Control;
  // The control, group or target's name - or, for a panel row, the
  // window id of the panel it ticks, since two units may each declare
  // one called "Kick".
  std::string name;
  int depth = 0;  // the panel member's depth, for the indent
  // The key the panel reserved for this row (Core.Ui.key), or empty for
  // the rows that take whatever is left.
  std::string key;
  bool operator==(const WindowElement&) const = default;
};

// The rows of one panel: its control members (a plain control or a whole
// group), then its targets. Members naming nothing are skipped, exactly
// as the drawing code skips them.
std::vector<WindowElement> windowElements(const PanelMeta& panel,
                                          const ProjectMeta& meta);

// One unit's share of the index: its key ("." or the rule path), its
// metadata, and the panels resolvePanels() made of it.
struct UnitIndex {
  std::string unit;
  const ProjectMeta* meta = nullptr;
  std::vector<PanelMeta> panels;
};

// The overview's rows: every panel every unit declares, in the order
// the overview lists them.
std::vector<WindowElement> overviewElements(const std::vector<UnitIndex>& units);

struct SearchItem {
  enum class Kind { Tab, Window, Element };
  Kind kind = Kind::Window;
  std::string label;   // what is matched against and shown
  std::string detail;  // where it is - "tab 2", "Kick - slider"
  int tab = 0;         // the tab it is in, or 0 when nothing holds it
  WindowRef window;    // the window it is, or the one that holds it
  std::string element;  // the element's name, for Kind::Element
};

// Everything reachable: every tab, every panel of every unit (open or
// not), and every element of every panel.
std::vector<SearchItem> buildSearchIndex(const std::vector<Tab>& tabs,
                                         const std::vector<UnitIndex>& units);

struct Match {
  size_t item = 0;
  int score = 0;
};

// Fuzzy subsequence matching over the label, ranked: a run at the start
// or on a word boundary beats one in the middle, tabs beat windows beat
// elements, and ties break by label so the list never reorders itself
// under the cursor. An empty query keeps the index's own order.
std::vector<Match> searchItems(const std::vector<SearchItem>& items,
                               std::string_view query);

// Keys for `n` rows, home row first. All are the same length, so no key
// is a prefix of another and the typing is never ambiguous.
std::vector<std::string> autoKeys(size_t n);

// The key each row of a window answers to, shown at the head of its
// name. A row the panel reserved one for (Core.Ui.key) keeps it, and
// the rest take what is left over, in order - so a reservation costs no
// other row its key, and the panel's own order decides the rest.
// Reservations are unique by the time they are here: the build refuses
// a panel that asks for one twice.
std::vector<std::string> rowKeys(const std::vector<WindowElement>& es);

// The rows Tab steps between: everything with something to do on it. A
// group's own row is the budget bar it draws and holds no value, so
// stepping passes over it and lands on its first lane - its key still
// selects it, for anyone who wants it.
std::vector<size_t> tabStops(const std::vector<WindowElement>& es);

// What the keys typed so far amount to: one row exactly, the start of
// at least one, or nothing that can ever match. On Exact, `index` is
// the row picked.
enum class KeyMatch { None, Prefix, Exact };
KeyMatch matchKey(const std::vector<std::string>& keys,
                  std::string_view typed, size_t& index);

}  // namespace synth::devapp
