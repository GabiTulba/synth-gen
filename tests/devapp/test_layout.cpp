#include "layout.hpp"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include "test_framework.hpp"

using namespace synth;
using namespace synth::devapp;

namespace {

WindowRef panel(const std::string& name) {
  WindowRef w;
  w.kind = WindowRef::Kind::Panel;
  w.unit = ".";
  w.panel = name;
  return w;
}

// A tab holding the named panels, inserted one after another the way the
// UI inserts them (each beside the last, all in one row).
Tab row(std::initializer_list<const char*> names) {
  Tab t;
  for (const char* n : names) insertWindow(t, panel(n));
  return t;
}

std::string shape(const Node& n) {
  if (n.kind == Node::Kind::Empty) return "-";
  if (n.kind == Node::Kind::Leaf) return windowTitle(n.window);
  std::string s = n.split == Split::H ? "h(" : "v(";
  for (size_t i = 0; i < n.children.size(); i++)
    s += (i ? " " : "") + shape(n.children[i]);
  return s + ")";
}

}  // namespace

TEST(layout_first_window_becomes_the_root) {
  Tab t;
  CHECK(t.empty());
  CHECK(insertWindow(t, panel("Kick")));
  CHECK(!t.empty());
  CHECK(shape(t.root) == "Kick");
  CHECK(t.focused.empty());
  CHECK(focusedWindow(t)->panel == "Kick");
}

TEST(layout_second_window_wraps_the_root_in_a_split) {
  Tab t = row({"Kick", "Snare"});
  CHECK(shape(t.root) == "h(Kick Snare)");
  CHECK((t.focused == Path{1}));
  CHECK_NEAR(t.root.fractions[0], 0.5, 1e-12);
  CHECK_NEAR(t.root.fractions[1], 0.5, 1e-12);
}

TEST(layout_insert_takes_half_of_the_focused_window) {
  Tab t = row({"Kick", "Snare", "Hat"});
  CHECK(shape(t.root) == "h(Kick Snare Hat)");
  CHECK((t.focused == Path{2}));
  CHECK_NEAR(t.root.fractions[0], 0.5, 1e-12);
  CHECK_NEAR(t.root.fractions[1], 0.25, 1e-12);
  CHECK_NEAR(t.root.fractions[2], 0.25, 1e-12);
}

TEST(layout_inserting_a_window_twice_only_focuses_it) {
  Tab t = row({"Kick", "Snare"});
  CHECK(!insertWindow(t, panel("Kick")));
  CHECK(windowsIn(t).size() == 2);
  CHECK(focusedWindow(t)->panel == "Kick");
}

TEST(layout_pending_split_wraps_the_focused_leaf) {
  Tab t = row({"Kick", "Snare"});
  t.pendingSplit = Split::V;
  CHECK(insertWindow(t, panel("Hat")));
  CHECK(shape(t.root) == "h(Kick v(Snare Hat))");
  CHECK((t.focused == Path{1, 1}));
  CHECK(!t.pendingSplit.has_value());  // consumed by the one insertion
  CHECK_NEAR(t.root.fractions[1], 0.5, 1e-12);
}

TEST(layout_remove_collapses_a_container_of_one) {
  Tab t = row({"Kick", "Snare"});
  t.pendingSplit = Split::V;
  insertWindow(t, panel("Hat"));
  CHECK(removeWindow(t, panel("Hat")));
  CHECK(shape(t.root) == "h(Kick Snare)");
  CHECK(focusedWindow(t)->panel == "Snare");
}

TEST(layout_remove_hands_the_share_to_the_siblings) {
  Tab t = row({"Kick", "Snare", "Hat"});  // .5 / .25 / .25
  CHECK(removeWindow(t, panel("Snare")));
  CHECK(shape(t.root) == "h(Kick Hat)");
  CHECK_NEAR(t.root.fractions[0], 2.0 / 3.0, 1e-12);
  CHECK_NEAR(t.root.fractions[1], 1.0 / 3.0, 1e-12);
}

TEST(layout_removing_the_last_window_empties_the_tab) {
  Tab t = row({"Kick"});
  CHECK(removeWindow(t, panel("Kick")));
  CHECK(t.empty());
  CHECK(!focusedWindow(t).has_value());
  CHECK(!removeWindow(t, panel("Kick")));
}

TEST(layout_removing_the_focused_window_focuses_its_neighbour) {
  Tab t = row({"Kick", "Snare", "Hat"});
  focusWindow(t, panel("Snare"));
  removeWindow(t, panel("Snare"));
  CHECK(focusedWindow(t)->panel == "Hat");  // what took its place
}

TEST(layout_focus_climbs_out_and_descends_by_the_near_edge) {
  Tab t = row({"Kick", "Snare"});
  t.pendingSplit = Split::V;
  insertWindow(t, panel("Hat"));  // h(Kick v(Snare Hat))
  focusWindow(t, panel("Kick"));

  CHECK(focusDir(t, Dir::Right));
  CHECK(focusedWindow(t)->panel == "Snare");  // the top of the column
  CHECK(focusDir(t, Dir::Down));
  CHECK(focusedWindow(t)->panel == "Hat");
  CHECK(focusDir(t, Dir::Left));  // climbs past the column's split
  CHECK(focusedWindow(t)->panel == "Kick");
  CHECK(!focusDir(t, Dir::Left));  // nothing further that way
  CHECK(focusedWindow(t)->panel == "Kick");
}

TEST(layout_focus_descends_to_the_far_child_moving_left) {
  Tab t = row({"Kick", "Snare"});
  focusWindow(t, panel("Kick"));
  t.pendingSplit = Split::H;
  focusWindow(t, panel("Snare"));
  insertWindow(t, panel("Hat"));  // h(Kick h(Snare Hat))
  focusWindow(t, panel("Kick"));
  CHECK(focusDir(t, Dir::Right));
  CHECK(focusedWindow(t)->panel == "Snare");  // entered by its left edge
  focusWindow(t, panel("Kick"));
  CHECK(shape(t.root) == "h(Kick h(Snare Hat))");
}

TEST(layout_move_swaps_with_the_neighbour) {
  Tab t = row({"Kick", "Snare"});
  focusWindow(t, panel("Snare"));
  CHECK(moveDir(t, Dir::Left));
  CHECK(shape(t.root) == "h(Snare Kick)");
  CHECK(focusedWindow(t)->panel == "Snare");  // focus travels with it
}

TEST(layout_move_carries_the_fraction_along) {
  Tab t = row({"Kick", "Snare", "Hat"});  // .5 / .25 / .25
  focusWindow(t, panel("Kick"));
  CHECK(moveDir(t, Dir::Right));
  CHECK(shape(t.root) == "h(Snare Kick Hat)");
  CHECK_NEAR(t.root.fractions[0], 0.25, 1e-12);
  CHECK_NEAR(t.root.fractions[1], 0.5, 1e-12);
}

TEST(layout_move_at_the_edge_promotes_to_the_root) {
  Tab t = row({"Kick", "Snare"});
  focusWindow(t, panel("Snare"));
  CHECK(moveDir(t, Dir::Down));  // no vertical split anywhere: promote
  CHECK(shape(t.root) == "v(Kick Snare)");
  CHECK(focusedWindow(t)->panel == "Snare");
}

TEST(layout_move_out_of_a_nested_container_reaches_the_root_row) {
  Tab t = row({"Kick", "Snare"});
  t.pendingSplit = Split::V;
  insertWindow(t, panel("Hat"));  // h(Kick v(Snare Hat)), focus Hat
  CHECK(moveDir(t, Dir::Right));  // rightmost already: promote to the root
  CHECK(shape(t.root) == "h(Kick Snare Hat)");
  CHECK(focusedWindow(t)->panel == "Hat");
}

TEST(layout_a_lone_window_cannot_be_moved) {
  Tab t = row({"Kick"});
  CHECK(!moveDir(t, Dir::Right));
  CHECK(shape(t.root) == "Kick");
}

// --- focusing a container, and reshaping the tree --------------------

// h(Kick v(Snare Hat)), focused on Snare - the smallest tree with a
// container worth stepping out to.
Tab nested() {
  Tab t = row({"Kick", "Snare"});
  t.pendingSplit = Split::V;
  insertWindow(t, panel("Hat"));
  focusWindow(t, panel("Snare"));
  return t;
}

TEST(layout_focus_steps_out_to_the_container_and_back_in) {
  Tab t = nested();
  CHECK(!focusIsContainer(t));
  CHECK(focusedWindow(t)->panel == "Snare");

  CHECK(focusParent(t));
  CHECK(focusIsContainer(t));
  CHECK(!focusedWindow(t).has_value());  // a container is not one window
  std::vector<WindowRef> under = focusedWindows(t);
  CHECK(under.size() == 2);  // ...it is everything inside it
  CHECK(under[0].panel == "Snare" && under[1].panel == "Hat");

  CHECK(focusParent(t));  // out again: the whole tab
  CHECK((t.focused == Path{}));
  CHECK(!focusParent(t));  // the root has nothing outside it

  CHECK(focusChild(t));
  CHECK(focusedWindow(t)->panel == "Kick");
  CHECK(!focusChild(t));  // a leaf has nothing inside it
}

TEST(layout_a_focused_container_moves_and_closes_as_one) {
  Tab t = nested();
  focusParent(t);  // the column holding Snare and Hat
  CHECK(moveDir(t, Dir::Left));
  CHECK(shape(t.root) == "h(v(Snare Hat) Kick)");
  CHECK(focusIsContainer(t));  // the focus travelled with it

  CHECK(closeFocused(t));
  CHECK(shape(t.root) == "Kick");  // both of its windows went with it
  CHECK(focusedWindow(t)->panel == "Kick");
}

TEST(layout_a_new_window_joins_the_focused_container) {
  // i3's rule: with a container focused, the newcomer becomes one of
  // its children rather than a sibling of the whole thing.
  Tab t = nested();
  focusParent(t);
  CHECK(insertWindow(t, panel("Ride")));
  CHECK(shape(t.root) == "h(Kick v(Snare Hat Ride))");
  CHECK(focusedWindow(t)->panel == "Ride");
}

TEST(layout_selection_runs_along_the_container_it_is_in) {
  Tab t = row({"Kick", "Snare", "Hat"});
  focusWindow(t, panel("Kick"));
  Run r = focusedRun(t);
  CHECK(r.valid && r.count() == 1 && r.from == 0);

  CHECK(extendSelection(t, Dir::Right));
  r = focusedRun(t);
  CHECK(r.count() == 2 && r.from == 0 && r.to == 1);
  CHECK(inRun(r, Path{0}) && inRun(r, Path{1}) && !inRun(r, Path{2}));

  CHECK(extendSelection(t, Dir::Right));
  CHECK(focusedRun(t).count() == 3);
  CHECK(!extendSelection(t, Dir::Right));  // no fourth sibling
  CHECK(!extendSelection(t, Dir::Down));   // the row is not split that way
  CHECK(extendSelection(t, Dir::Left));    // and it pulls back again
  CHECK(focusedRun(t).count() == 2);
}

TEST(layout_moving_the_focus_drops_the_selection) {
  Tab t = row({"Kick", "Snare", "Hat"});
  focusWindow(t, panel("Kick"));
  extendSelection(t, Dir::Right);
  CHECK(focusedRun(t).count() == 2);
  focusDir(t, Dir::Right);
  CHECK(focusedRun(t).count() == 1);  // a run belongs to where you were
}

TEST(layout_grouping_gathers_the_run_into_a_container) {
  Tab t = row({"Kick", "Snare", "Hat"});  // .5 / .25 / .25
  focusWindow(t, panel("Snare"));
  extendSelection(t, Dir::Right);         // Snare and Hat
  CHECK(groupSelection(t, Split::V));
  CHECK(shape(t.root) == "h(Kick v(Snare Hat))");
  // The new container holds exactly what the two of them held.
  CHECK_NEAR(t.root.fractions[0], 0.5, 1e-12);
  CHECK_NEAR(t.root.fractions[1], 0.5, 1e-12);
  CHECK(focusIsContainer(t));  // ready to move, resize or flatten
  CHECK(focusedRun(t).count() == 1);  // the run is spent
}

TEST(layout_grouping_everything_re_orients_instead_of_nesting) {
  // Wrapping every child of a container in one more container would
  // leave a level that does nothing.
  Tab t = row({"Kick", "Snare"});
  focusWindow(t, panel("Kick"));
  extendSelection(t, Dir::Right);
  CHECK(groupSelection(t, Split::V));
  CHECK(shape(t.root) == "v(Kick Snare)");
}

TEST(layout_grouping_one_window_is_just_a_split) {
  // Nothing to gather: it decides where the next window goes, which is
  // i3's `split`.
  Tab t = row({"Kick", "Snare"});
  CHECK(groupSelection(t, Split::V));
  CHECK(shape(t.root) == "h(Kick Snare)");  // unchanged
  CHECK(t.pendingSplit == Split::V);
  insertWindow(t, panel("Hat"));
  CHECK(shape(t.root) == "h(Kick v(Snare Hat))");
}

TEST(layout_flattening_dissolves_a_container_into_its_parent) {
  Tab t = nested();  // h(Kick v(Snare Hat))
  resizeSplit(t, {}, 0, 0.2);  // Kick .7, the column .3
  focusParent(t);
  CHECK(focusIsContainer(t));
  CHECK(flattenFocused(t));
  CHECK(shape(t.root) == "h(Kick Snare Hat)");
  // The column's share is divided the way the column divided it: half
  // each of the 0.3 it had.
  CHECK_NEAR(t.root.fractions[0], 0.7, 1e-9);
  CHECK_NEAR(t.root.fractions[1], 0.15, 1e-9);
  CHECK_NEAR(t.root.fractions[2], 0.15, 1e-9);
  CHECK(focusedWindow(t)->panel == "Snare");
}

TEST(layout_flattening_needs_a_container_below_the_root) {
  Tab t = nested();
  CHECK(!flattenFocused(t));  // a leaf is not a container
  focusParent(t);
  focusParent(t);             // the root
  CHECK(!flattenFocused(t));  // ...has nowhere to dissolve into
  CHECK(shape(t.root) == "h(Kick v(Snare Hat))");
}

TEST(layout_group_then_flatten_comes_back_to_where_it_started) {
  Tab t = row({"Kick", "Snare", "Hat"});
  const Node before = t.root;
  focusWindow(t, panel("Snare"));
  extendSelection(t, Dir::Right);
  CHECK(groupSelection(t, Split::V));
  CHECK(flattenFocused(t));
  CHECK(t.root == before);  // fractions included
}

TEST(layout_place_reports_the_focus_and_the_run) {
  Tab t = row({"Kick", "Snare", "Hat"});
  focusWindow(t, panel("Snare"));
  extendSelection(t, Dir::Right);
  Placement p = place(t, Rect{0, 0, 300, 100});
  CHECK(!p.windows[0].selected);
  CHECK(p.windows[1].focused && p.windows[1].selected);
  CHECK(!p.windows[2].focused && p.windows[2].selected);

  Tab n = nested();
  focusParent(n);
  Placement q = place(n, Rect{0, 0, 300, 100});
  CHECK(q.splits.size() == 2);
  CHECK(!q.splits[0].focused && q.splits[1].focused);
  // The windows inside it are not focused, but they say they are in it.
  for (const PlacedWindow& w : q.windows) {
    CHECK(!w.focused);
    CHECK(w.inFocus == (w.window.panel != "Kick"));
  }
}

TEST(layout_a_container_travels_between_tabs_intact) {
  std::vector<Tab> tabs;
  Tab& one = ensureTab(tabs, 1);
  one = nested();
  one.index = 1;
  focusWindow(one, panel("Snare"));
  focusParent(findTab(tabs, 1)[0]);
  CHECK(sendFocusedToTab(tabs, 1, 2, Rect{0, 0, 1200, 800}));
  CHECK(shape(findTab(tabs, 1)->root) == "Kick");
  CHECK(shape(findTab(tabs, 2)->root) == "v(Snare Hat)");
  CHECK(tabHolding(tabs, panel("Hat")) == 2);
}

TEST(layout_place_tiles_the_area_exactly) {
  Tab t = row({"Kick", "Snare"});
  LayoutMetrics m;  // gap 6, pad 3, grab 8
  Placement p = place(t, Rect{0, 0, 100, 50}, m);
  CHECK(p.windows.size() == 2);
  CHECK(p.splits.size() == 1);
  CHECK((p.splits[0].rect == Rect{0, 0, 100, 50}));
  const PlacedWindow& a = p.windows[0];
  const PlacedWindow& b = p.windows[1];
  CHECK(a.window.panel == "Kick" && b.window.panel == "Snare");
  CHECK((a.rect == Rect{3, 3, 44, 44}));
  CHECK((b.rect == Rect{53, 3, 44, 44}));
  // the two windows plus the one gap fill the padded container
  CHECK_NEAR(a.rect.w + m.gap + b.rect.w, 100 - 2 * m.pad, 1e-4);
  CHECK(a.depth == 1 && b.depth == 1);
  CHECK(b.focused && !a.focused);
}

TEST(layout_place_nests_depth_and_dividers) {
  Tab t = row({"Kick", "Snare"});
  t.pendingSplit = Split::V;
  insertWindow(t, panel("Hat"));  // h(Kick v(Snare Hat))
  Placement p = place(t, Rect{0, 0, 200, 100});
  CHECK(p.windows.size() == 3);
  CHECK(p.splits.size() == 2);
  CHECK(p.dividers.size() == 2);
  // one divider per gap: the outer one vertical, the inner one horizontal
  CHECK(p.dividers[0].split == Split::H && p.dividers[0].path.empty());
  CHECK(p.dividers[1].split == Split::V);
  CHECK((p.dividers[1].path == Path{1}));
  CHECK(p.dividers[1].boundary == 0);
  for (const PlacedWindow& w : p.windows)
    CHECK(w.depth == (w.window.panel == "Kick" ? 1 : 2));
}

TEST(layout_resize_moves_one_boundary_and_clamps) {
  Tab t = row({"Kick", "Snare"});
  CHECK(resizeSplit(t, {}, 0, 0.2));
  CHECK_NEAR(t.root.fractions[0], 0.7, 1e-12);
  CHECK_NEAR(t.root.fractions[1], 0.3, 1e-12);
  CHECK(resizeSplit(t, {}, 0, 5.0));  // clamped, not rejected
  CHECK_NEAR(t.root.fractions[1], kMinFraction, 1e-12);
  CHECK(!resizeSplit(t, {}, 0, 5.0));  // already there: nothing to do
  CHECK(!resizeSplit(t, {}, 7, 0.1));  // no such boundary
}

TEST(layout_resize_focused_grows_the_window_either_way) {
  Tab t = row({"Kick", "Snare"});  // focus Snare, the last child
  CHECK(resizeFocused(t, Dir::Right, 0.1));
  CHECK_NEAR(t.root.fractions[1], 0.6, 1e-12);
  focusWindow(t, panel("Kick"));
  CHECK(resizeFocused(t, Dir::Right, 0.1));
  CHECK_NEAR(t.root.fractions[0], 0.5, 1e-12);
  CHECK(resizeFocused(t, Dir::Left, -0.1));  // shrink from the left
  CHECK_NEAR(t.root.fractions[0], 0.4, 1e-12);
  CHECK(!resizeFocused(t, Dir::Up, 0.1));  // no vertical split to move
}

TEST(layout_auto_orientation_divides_the_longer_side) {
  // i3's `default_orientation auto`: a wide window splits into columns,
  // a tall one into rows, so windows land as a grid.
  Rect wide{0, 0, 1200, 800};
  Tab t;
  insertWindowAuto(t, panel("A"), wide);
  insertWindowAuto(t, panel("B"), wide);
  CHECK(shape(t.root) == "h(A B)");   // the whole area is wider than tall
  insertWindowAuto(t, panel("C"), wide);
  CHECK(shape(t.root) == "h(A v(B C))");  // ...but half of it is not
  insertWindowAuto(t, panel("D"), wide);
  CHECK(shape(t.root) == "h(A v(B h(C D)))");
}

TEST(layout_auto_orientation_yields_to_an_asked_for_split) {
  Rect wide{0, 0, 1200, 800};
  Tab t;
  insertWindowAuto(t, panel("A"), wide);
  t.pendingSplit = Split::V;  // Alt+v
  insertWindowAuto(t, panel("B"), wide);
  CHECK(shape(t.root) == "v(A B)");
}

TEST(layout_migration_opens_what_the_old_panel_row_had_open) {
  std::vector<WindowRef> all = {overviewWindow(), panel("Kick"),
                                panel("Snare"), panel("Hat")};
  // A panel the map has never heard of opened by default in the old
  // shell, so it opens here too; one that was closed stays closed.
  std::map<std::string, bool> open = {{"./Snare", false}};
  Tab t = migratedTab(all, open, Rect{0, 0, 1200, 800});
  std::vector<WindowRef> got = windowsIn(t);
  CHECK(got.size() == 3);
  CHECK(hasWindow(t, overviewWindow()));
  CHECK(hasWindow(t, panel("Kick")));
  CHECK(!hasWindow(t, panel("Snare")));
  CHECK(hasWindow(t, panel("Hat")));
  CHECK(focusedWindow(t)->kind == WindowRef::Kind::Overview);
}

TEST(layout_tabs_are_kept_in_index_order) {
  std::vector<Tab> tabs;
  ensureTab(tabs, 3);
  ensureTab(tabs, 1);
  ensureTab(tabs, 2);
  CHECK(tabs.size() == 3);
  CHECK(tabs[0].index == 1 && tabs[1].index == 2 && tabs[2].index == 3);
  CHECK(&ensureTab(tabs, 2) == findTab(tabs, 2));  // idempotent
  CHECK(tabs.size() == 3);
  CHECK(nextFreeTabIndex(tabs) == 4);
  ensureTab(tabs, 5);
  CHECK(nextFreeTabIndex(tabs) == 4);  // the lowest free one, not the next
}

TEST(layout_sending_a_window_moves_it_between_tabs) {
  std::vector<Tab> tabs;
  Tab& one = ensureTab(tabs, 1);
  insertWindow(one, panel("Kick"));
  insertWindow(one, panel("Snare"));
  CHECK(tabHolding(tabs, panel("Snare")) == 1);
  Rect area{0, 0, 1200, 800};
  CHECK(sendWindowToTab(tabs, panel("Snare"), 2, area));
  CHECK(tabHolding(tabs, panel("Snare")) == 2);
  CHECK(shape(findTab(tabs, 1)->root) == "Kick");
  CHECK(shape(findTab(tabs, 2)->root) == "Snare");  // the tab was created
  CHECK(!sendWindowToTab(tabs, panel("Nope"), 2, area));
  // ...and it tiles on arrival like any other opening, so a window sent
  // to a tab that already has two lands beside the focused one.
  insertWindowAuto(*findTab(tabs, 2), panel("Hat"), area);
  CHECK(sendWindowToTab(tabs, panel("Kick"), 2, area));
  CHECK(windowsIn(*findTab(tabs, 2)).size() == 3);
  CHECK(shape(findTab(tabs, 2)->root) == "h(Snare v(Hat Kick))");
  CHECK(findTab(tabs, 1)->empty());  // the tab it left is emptied
  CHECK(closeWindowEverywhere(tabs, panel("Snare")));
  CHECK(windowsIn(*findTab(tabs, 2)).size() == 2);
}

TEST(layout_tab_label_shows_the_index_and_any_name) {
  Tab t;
  t.index = 2;
  CHECK(tabLabel(t) == "2");
  t.name = "drums";
  CHECK(tabLabel(t) == "2:drums");
}

TEST(layout_json_roundtrips_a_tree) {
  Tab t = row({"Kick", "Snare"});
  t.pendingSplit = Split::V;
  insertWindow(t, panel("Hat"));
  insertWindow(t, overviewWindow());
  t.index = 4;
  t.name = "mix";
  resizeSplit(t, {}, 0, 0.15);
  t.pendingSplit.reset();  // not persisted: it belongs to the next insert

  Tab back;
  CHECK(tabFromJson(toJson(t), back));
  CHECK(back == t);
}

TEST(layout_json_survives_a_malformed_tree) {
  json::Value v;
  std::string err;
  // A duplicate window, a child that is not a node, and a container left
  // holding one child: all repaired rather than fatal.
  CHECK(json::parse(R"({"index": 2, "focused": [9],
        "root": {"split": "v", "fractions": [0.5, 0.3, 0.2], "children": [
           {"window": {"kind": "panel", "unit": ".", "panel": "Kick"}},
           {"window": {"kind": "panel", "unit": ".", "panel": "Kick"}},
           {"split": "h", "children": [
              {"window": {"kind": "panel", "unit": ".", "panel": "Hat"}},
              17]}]}})",
                    v, err));
  Tab t;
  CHECK(tabFromJson(v, t));
  CHECK(t.index == 2);
  CHECK(shape(t.root) == "v(Kick Hat)");
  CHECK_NEAR(t.root.fractions[0] + t.root.fractions[1], 1.0, 1e-12);
  CHECK(focusedWindow(t).has_value());  // the out-of-range focus was repaired
}

TEST(layout_json_rejects_what_is_not_a_tab) {
  json::Value v;
  std::string err;
  Tab t;
  CHECK(json::parse(R"({"name": "no index"})", v, err));
  CHECK(!tabFromJson(v, t));
  CHECK(json::parse("[1, 2]", v, err));
  CHECK(!tabFromJson(v, t));
}

TEST(layout_window_identity_is_unit_qualified) {
  CHECK(windowId(panel("Kick")) == "./Kick");
  CHECK(windowId(overviewWindow()) == "@overview");
  CHECK(windowTitle(overviewWindow()) == "overview");
  WindowRef a = panel("Kick"), b = panel("Kick");
  b.unit = "lib/drums";
  CHECK(!(a == b));
}

// --- the invariants every operation has to leave standing -------------

namespace {

// A container always holds at least two children (one is a level that
// does nothing, and none is a window with nothing in it), fractions
// match the children and sum to 1, and the focus names a real node.
void checkInvariants(const Node& n, const std::string& where) {
  if (n.kind != Node::Kind::Split) return;
  if (n.children.size() < 2) {
    std::printf("    a container of %zu after %s\n", n.children.size(),
                where.c_str());
    CHECK(false);
    return;
  }
  CHECK(n.fractions.size() == n.children.size());
  double sum = 0;
  for (double f : n.fractions) {
    CHECK(f > 0);
    sum += f;
  }
  CHECK_NEAR(sum, 1.0, 1e-9);
  for (const Node& c : n.children) {
    if (c.kind == Node::Kind::Empty) {
      std::printf("    an empty child after %s\n", where.c_str());
      CHECK(false);
      return;
    }
    checkInvariants(c, where);
  }
}

void checkTab(const Tab& t, const std::string& where) {
  checkInvariants(t.root, where);
  if (t.empty()) return;
  if (!at(t.root, t.focused)) {
    std::printf("    focus dangles after %s\n", where.c_str());
    CHECK(false);
  }
  std::set<std::string> seen;
  for (const WindowRef& w : windowsIn(t)) CHECK(seen.insert(windowId(w)).second);
}

// Deterministic, so a failure is reproducible from the seed alone.
struct Rng {
  uint64_t s;
  uint64_t next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return s >> 33;
  }
  int pick(int n) { return (int)(next() % (uint64_t)n); }
};

}  // namespace

TEST(layout_every_operation_leaves_the_tree_well_formed) {
  const Rect area{0, 0, 1200, 800};
  for (uint64_t seed = 1; seed <= 200; seed++) {
    Rng rng{seed};
    Tab t;
    int made = 0;
    for (int step = 0; step < 60; step++) {
      int op = rng.pick(11);
      std::string where = "seed " + std::to_string(seed) + " step " +
                          std::to_string(step) + " op " + std::to_string(op);
      switch (op) {
        case 0:
        case 1:
          insertWindowAuto(t, panel("w" + std::to_string(made++)), area);
          break;
        case 2: {
          std::vector<WindowRef> ws = windowsIn(t);
          if (!ws.empty()) removeWindow(t, ws[(size_t)rng.pick((int)ws.size())]);
          break;
        }
        case 3: focusDir(t, (Dir)rng.pick(4)); break;
        case 4: moveDir(t, (Dir)rng.pick(4)); break;
        case 5: focusParent(t); break;
        case 6: focusChild(t); break;
        case 7: extendSelection(t, (Dir)rng.pick(4)); break;
        case 8: groupSelection(t, rng.pick(2) ? Split::H : Split::V); break;
        case 9: flattenFocused(t); break;
        case 10:
          if (rng.pick(4) == 0)
            closeFocused(t);
          else
            resizeFocused(t, (Dir)rng.pick(4), rng.pick(2) ? 0.1 : -0.1);
          break;
      }
      checkTab(t, where);
    }
  }
}
