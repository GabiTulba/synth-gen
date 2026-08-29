#include "search.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "build.hpp"
#include "layout.hpp"
#include "manifest_helpers.hpp"
#include "test_framework.hpp"

using namespace synth;
using namespace synth::devapp;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path dir;
  TempDir() {
    static int counter = 0;
    dir = fs::temp_directory_path() /
          ("synthgraph-search-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void write(const std::string& name, const std::string& text) {
    std::ofstream out(dir / name);
    out << text;
  }
};

WindowRef panel(const std::string& name) {
  WindowRef w;
  w.kind = WindowRef::Kind::Panel;
  w.unit = ".";
  w.panel = name;
  return w;
}

// A project with one panel holding a knob, a two-lane group, a nested
// component and a rendered target - one of each thing a window can show.
ProjectMeta buildDemo(TempDir& tp) {
  tp.write("a.synth", R"(
open Core open Core.Control open Core.Arrange open Core.Render open Core.Sig
let gain : Scalar Control =
  Control.knob ~name:"gain" ~min:0.0 ~max:1.0 ~default:0.5 ;;
let trim : Scalar Control =
  Control.slider ~name:"trim" ~min:0.0 ~max:1.0 ~default:0.5 ;;
let pair : Scalar Control =
  Control.nest ~value:(gain.value *. trim.value) ~parts:[gain.ui; trim.ui] ;;
let env : Scalar list Control =
  Control.multi_slider ~name:"env" ~sum_min:0.0 ~sum_max:1.0
    ~lanes:[ { name = "attack"; min = 0.0; max = 0.5; default = 0.05 };
             { name = "decay";  min = 0.0; max = 0.5; default = 0.15 } ] ;;
let _ = render "demo" 8000.0 (sample (constant pair.value) 0s 50ms) ;;
let _ = Ui.panel ~name:"Voice" ~controls:[pair.ui; env.ui] ~targets:["demo"] ;;
)");
  tp.write("build.json", projectManifest("search-demo", {"a.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);
  MetadataLoadResult m = loadProjectMetadata(r.metadataPath);
  CHECK(m.ok);
  return m.meta;
}

}  // namespace

TEST(search_window_elements_follow_the_panel_in_order) {
  TempDir tp;
  ProjectMeta meta = buildDemo(tp);
  CHECK(meta.panels.size() == 1);
  std::vector<WindowElement> es = windowElements(meta.panels[0], meta);
  CHECK(es.size() == 6);
  CHECK(es[0].kind == WindowElement::Kind::Control && es[0].name == "gain");
  CHECK(es[0].depth == 0);
  CHECK(es[1].name == "trim" && es[1].depth == 1);  // the component's part
  // A group is a heading - the budget bar - and each lane under it is a
  // row of its own, because a lane is what has a value to select and
  // nudge.
  CHECK(es[2].kind == WindowElement::Kind::Group && es[2].name == "env");
  CHECK(es[3].kind == WindowElement::Kind::Lane && es[3].name == "env.attack");
  CHECK(es[3].depth == 1);
  CHECK(es[4].kind == WindowElement::Kind::Lane && es[4].name == "env.decay");
  CHECK(es[5].kind == WindowElement::Kind::Target && es[5].name == "demo");
}

TEST(search_window_elements_skip_members_naming_nothing) {
  ProjectMeta meta;
  PanelMeta p;
  p.name = "P";
  p.controls = {PanelMember{"ghost", 0, ""}};
  p.targets = {"nowhere"};
  CHECK(windowElements(p, meta).empty());
}

TEST(search_index_covers_tabs_windows_and_elements) {
  TempDir tp;
  ProjectMeta meta = buildDemo(tp);
  std::vector<Tab> tabs;
  Tab& one = ensureTab(tabs, 1);
  insertWindow(one, overviewWindow());
  insertWindow(one, panel("Voice"));
  ensureTab(tabs, 2).name = "drums";

  UnitIndex u;
  u.unit = ".";
  u.meta = &meta;
  u.panels = resolvePanels(meta);
  std::vector<SearchItem> index = buildSearchIndex(tabs, {u});

  size_t tabsSeen = 0, windows = 0, elements = 0;
  for (const SearchItem& it : index) {
    if (it.kind == SearchItem::Kind::Tab) tabsSeen++;
    if (it.kind == SearchItem::Kind::Window) windows++;
    if (it.kind == SearchItem::Kind::Element) elements++;
  }
  CHECK(tabsSeen == 2);
  CHECK(windows == 2);   // the overview and the one panel
  CHECK(elements == 6);  // gain, trim, env + its two lanes, demo

  for (const SearchItem& it : index) {
    if (it.kind == SearchItem::Kind::Window && it.window.panel == "Voice") {
      CHECK(it.tab == 1);
      CHECK(it.detail == "tab 1");
    }
    if (it.element == "gain") {
      CHECK(it.window.panel == "Voice");
      CHECK(it.detail == "Voice - control");
    }
    if (it.element == "demo") CHECK(it.detail == "Voice - waveform");
    if (it.element == "env") CHECK(it.detail == "Voice - lanes");
    // A lane is searchable in its own right now.
    if (it.element == "env.attack") CHECK(it.detail == "Voice - lane");
  }
}

TEST(search_overview_rows_are_the_panels_of_every_unit) {
  // The overview's tickboxes are rows like any other: hint-labelled,
  // stepped through with Tab, ticked with Enter.
  TempDir tp;
  ProjectMeta meta = buildDemo(tp);
  UnitIndex u{".", &meta, resolvePanels(meta)};
  std::vector<WindowElement> rows = overviewElements({u});
  CHECK(rows.size() == u.panels.size());
  CHECK(rows[0].kind == WindowElement::Kind::Panel);
  // Keyed by window id, because two units may each declare a "Kick".
  CHECK(rows[0].name == "./Voice");
  CHECK(overviewElements({}).empty());

  UnitIndex other{"lib/drums", &meta, resolvePanels(meta)};
  std::vector<WindowElement> both = overviewElements({u, other});
  CHECK(both.size() == rows.size() * 2);
  CHECK(both[rows.size()].name == "lib/drums/Voice");
}

TEST(search_finds_a_panel_no_tab_is_showing) {
  TempDir tp;
  ProjectMeta meta = buildDemo(tp);
  std::vector<Tab> tabs;
  ensureTab(tabs, 1);
  UnitIndex u{".", &meta, resolvePanels(meta)};
  std::vector<SearchItem> index = buildSearchIndex(tabs, {u});
  bool found = false;
  for (const SearchItem& it : index)
    if (it.kind == SearchItem::Kind::Window && it.window.panel == "Voice") {
      found = true;
      CHECK(it.tab == 0);
      CHECK(it.detail == "closed");  // finding it is how you open it
    }
  CHECK(found);
}

TEST(search_matches_a_subsequence_and_ranks_the_close_ones_first) {
  std::vector<SearchItem> items = {
      {SearchItem::Kind::Element, "adsr sustain amp", "", 0, {}, "a"},
      {SearchItem::Kind::Element, "sustain", "", 0, {}, "b"},
      {SearchItem::Kind::Element, "noise amp", "", 0, {}, "c"},
  };
  std::vector<Match> m = searchItems(items, "sust");
  CHECK(m.size() == 2);
  CHECK(items[m[0].item].element == "b");  // the name that starts with it
  CHECK(items[m[1].item].element == "a");
  CHECK(searchItems(items, "zzz").empty());
  // spaces in the query are ignored, so "nsam" finds "noise amp"
  CHECK(searchItems(items, "n s amp").size() == 1);
}

TEST(search_an_empty_query_keeps_the_index_order) {
  std::vector<SearchItem> items = {
      {SearchItem::Kind::Element, "zeta", "", 0, {}, "z"},
      {SearchItem::Kind::Element, "alpha", "", 0, {}, "a"},
  };
  std::vector<Match> m = searchItems(items, "");
  CHECK(m.size() == 2);
  CHECK(m[0].item == 0 && m[1].item == 1);
}

TEST(search_a_number_finds_the_tab_with_that_index) {
  std::vector<Tab> tabs;
  ensureTab(tabs, 1).name = "main";
  ensureTab(tabs, 3).name = "mix";
  std::vector<SearchItem> index = buildSearchIndex(tabs, {});
  std::vector<Match> m = searchItems(index, "3");
  CHECK(!m.empty());
  const SearchItem& top = index[m[0].item];
  CHECK(top.kind == SearchItem::Kind::Tab);
  CHECK(top.tab == 3);
  // and by name, the same tab
  std::vector<Match> byName = searchItems(index, "mix");
  CHECK(!byName.empty());
  CHECK(index[byName[0].item].tab == 3);
}

TEST(search_tabs_report_how_much_they_hold) {
  std::vector<Tab> tabs;
  Tab& t = ensureTab(tabs, 1);
  insertWindow(t, overviewWindow());
  std::vector<SearchItem> index = buildSearchIndex(tabs, {});
  CHECK(index[0].kind == SearchItem::Kind::Tab);
  CHECK(index[0].detail == "1 window");
  insertWindow(t, panel("Kick"));
  CHECK(buildSearchIndex(tabs, {})[0].detail == "2 windows");
}

TEST(search_reserved_keys_are_kept_and_the_rest_fill_in_around_them) {
  auto row = [](const char* name, const char* key) {
    return WindowElement{WindowElement::Kind::Control, name, 0, key};
  };
  // "s" is spoken for, so the automatic labels skip it rather than
  // handing it out twice - and no other row loses its place in the
  // order because of the reservation.
  std::vector<WindowElement> es = {row("attack", ""), row("sustain", "s"),
                                   row("release", ""), row("amount", "z")};
  std::vector<std::string> labels = rowKeys(es);
  CHECK(labels.size() == 4);
  CHECK(labels[1] == "s");
  CHECK(labels[3] == "z");
  CHECK(labels[0] == "a");  // the first free one, in panel order
  CHECK(labels[2] == "d");  // "s" is taken, so this one skips it
  std::set<std::string> seen;
  for (const std::string& l : labels) CHECK(seen.insert(l).second);
}

TEST(search_a_reserved_key_is_never_a_prefix_of_an_automatic_one) {
  // Past 26 rows the automatic labels are pairs. A single reserved "a"
  // beside an automatic "as" would make "as" unreachable, since "a"
  // matches first.
  std::vector<WindowElement> es;
  for (int i = 0; i < 30; i++)
    es.push_back(WindowElement{WindowElement::Kind::Control,
                               "c" + std::to_string(i), 0, ""});
  es[0].key = "a";
  std::vector<std::string> labels = rowKeys(es);
  CHECK(labels[0] == "a");
  std::set<std::string> seen;
  for (const std::string& l : labels) {
    CHECK(!l.empty());
    CHECK(seen.insert(l).second);
    if (l != "a") CHECK(l.rfind("a", 0) != 0 || l.size() == 1);
  }
  // ...and every label still resolves to exactly the row it belongs to.
  for (size_t i = 0; i < labels.size(); i++) {
    size_t at = 999;
    CHECK(matchKey(labels, labels[i], at) == KeyMatch::Exact);
    CHECK(at == i);
  }
}

TEST(search_unkeyed_rows_label_exactly_as_before) {
  std::vector<WindowElement> es;
  for (int i = 0; i < 6; i++)
    es.push_back(WindowElement{WindowElement::Kind::Control,
                               "c" + std::to_string(i), 0, ""});
  CHECK(rowKeys(es) == autoKeys(6));
}

TEST(search_tab_steps_over_a_group_header) {
  // The group's row is its budget bar - nothing on it to nudge - so
  // stepping lands on the lanes instead.
  auto row = [](WindowElement::Kind k, const char* name) {
    return WindowElement{k, name, 0, ""};
  };
  std::vector<WindowElement> es = {
      row(WindowElement::Kind::Control, "gain"),
      row(WindowElement::Kind::Group, "env"),
      row(WindowElement::Kind::Lane, "env.attack"),
      row(WindowElement::Kind::Lane, "env.decay"),
      row(WindowElement::Kind::Target, "demo")};
  std::vector<size_t> stops = tabStops(es);
  CHECK(stops.size() == 4);
  CHECK(stops[0] == 0 && stops[1] == 2 && stops[2] == 3 && stops[3] == 4);
}

TEST(search_a_group_heading_is_given_no_key) {
  // Nothing on a heading answers to a key, so it is given none - and it
  // does not spend one either: the letters run straight past it to the
  // lanes, which is what the window's gutter shows.
  auto row = [](WindowElement::Kind k, const char* name) {
    return WindowElement{k, name, 0, ""};
  };
  std::vector<WindowElement> es = {
      row(WindowElement::Kind::Control, "gain"),
      row(WindowElement::Kind::Group, "env"),
      row(WindowElement::Kind::Lane, "env.attack"),
      row(WindowElement::Kind::Lane, "env.decay"),
      row(WindowElement::Kind::Target, "demo")};
  std::vector<std::string> keys = rowKeys(es);
  CHECK(keys.size() == 5);
  CHECK(keys[1].empty());
  std::vector<std::string> four = autoKeys(4);
  CHECK(keys[0] == four[0] && keys[2] == four[1] && keys[3] == four[2] &&
        keys[4] == four[3]);
}

TEST(search_key_matching_tells_a_pick_from_a_dead_end) {
  std::vector<std::string> single = autoKeys(6);  // a s d f g h
  size_t at = 99;
  CHECK(matchKey(single, "f", at) == KeyMatch::Exact);
  CHECK(at == 3);
  CHECK(matchKey(single, "z", at) == KeyMatch::None);
  CHECK(matchKey(single, "", at) == KeyMatch::Prefix);

  std::vector<std::string> pairs = autoKeys(40);  // aa as ad ...
  CHECK(matchKey(pairs, "a", at) == KeyMatch::Prefix);
  CHECK(matchKey(pairs, "as", at) == KeyMatch::Exact);
  CHECK(at == 1);
  CHECK(matchKey(pairs, "az", at) == KeyMatch::Exact);  // a? covers a-z
  CHECK(at == 19);
  // 40 labels is every "a?" and the first few "s?", so nothing starts
  // with the third letter of the alphabet yet.
  CHECK(matchKey(pairs, "d", at) == KeyMatch::None);
  CHECK(matchKey({}, "a", at) == KeyMatch::None);
}

TEST(search_auto_keys_are_unique_and_never_a_prefix_of_each_other) {
  for (size_t n : {size_t(0), size_t(1), size_t(9), size_t(26), size_t(27),
                   size_t(60)}) {
    std::vector<std::string> ls = autoKeys(n);
    CHECK(ls.size() == n);
    std::set<std::string> seen;
    size_t width = ls.empty() ? 0 : ls[0].size();
    for (const std::string& l : ls) {
      CHECK(seen.insert(l).second);
      CHECK(l.size() == width);  // one length throughout: no ambiguity
    }
  }
  // Stable across calls, and the home row comes first.
  CHECK(autoKeys(3)[0] == "a");
  CHECK(autoKeys(3)[1] == "s");
  CHECK(autoKeys(30)[0] == "aa");
}
