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
// of controls, groups and targets; the overview is made of the panels
// themselves, one tickbox each.
struct WindowElement {
  enum class Kind { Control, Group, Target, Panel };
  Kind kind = Kind::Control;
  // The control, group or target's name - or, for a panel row, the
  // window id of the panel it ticks, since two units may each declare
  // one called "Kick".
  std::string name;
  int depth = 0;  // the panel member's depth, for the indent
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

// The hint labels for `n` elements, home row first. All labels are the
// same length, so no label is a prefix of another and typing is never
// ambiguous.
std::vector<std::string> hintLabels(size_t n);

// What the letters typed so far amount to: one label exactly, the start
// of at least one, or nothing that can ever match. On Exact, `index` is
// the element picked.
enum class HintMatch { None, Prefix, Exact };
HintMatch matchHint(const std::vector<std::string>& labels,
                    std::string_view typed, size_t& index);

}  // namespace synth::devapp
