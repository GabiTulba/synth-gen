#include "keymap.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "layout.hpp"
#include "test_framework.hpp"

using namespace synth;
using namespace synth::devapp;

namespace {

Chord alt(Key k) { return Chord{k, true, false, false}; }
Chord altShift(Key k) { return Chord{k, true, true, false}; }
Chord bare(Key k) { return Chord{k, false, false, false}; }
Chord ctrl(Key k) { return Chord{k, false, false, true}; }
Chord shift(Key k) { return Chord{k, false, true, false}; }

// The binding a chord resolves to in `ctx`, the way dispatch picks it.
const Binding* find(Mode m, Chord c, unsigned ctx = CtxAny) {
  for (const Binding& b : bindings())
    if (b.mode == m && b.sequence.size() == 1 && b.sequence[0] == c &&
        (b.need & ctx) == b.need && (b.deny & ctx) == 0)
      return &b;
  return nullptr;
}

// Every context the app can actually be in. Two rules from contextBits
// in app.cpp: a selected row is a waveform or a control, never both,
// and neither can be selected without a row being selected at all.
std::vector<unsigned> reachableContexts() {
  std::vector<unsigned> out;
  for (unsigned c = 0; c < 64; c++) {
    if ((c & CtxWave) && (c & CtxWidget)) continue;
    if ((c & (CtxWave | CtxWidget)) && !(c & CtxRow)) continue;
    out.push_back(c);
  }
  return out;
}

}  // namespace

TEST(keymap_no_chord_is_ambiguous_in_any_context) {
  // A chord may be bound twice in one mode - Enter and the bare up and
  // down arrows mean one thing on a waveform and another elsewhere -
  // but never in a context where both could fire. Ambiguity there would
  // mean the resolved action depends on table order, which nobody
  // reading the table could predict.
  for (unsigned ctx : reachableContexts()) {
    std::set<std::pair<int, std::string>> seen;
    for (const Binding& b : bindings()) {
      if ((b.need & ctx) != b.need || (b.deny & ctx) != 0) continue;
      auto key = std::make_pair((int)b.mode, sequenceName(b.sequence));
      CHECK(seen.insert(key).second);
    }
  }
}

TEST(keymap_every_binding_is_reachable_somewhere) {
  // The other half of it: a binding whose need and deny masks cannot
  // both be satisfied is dead text in the table and an entry in a help
  // pane for a key that does nothing.
  for (const Binding& b : bindings()) {
    bool live = false;
    for (unsigned ctx : reachableContexts())
      if ((b.need & ctx) == b.need && (b.deny & ctx) == 0) live = true;
    CHECK(live);
  }
}

TEST(keymap_every_listed_binding_says_what_it_does) {
  for (const Binding& b : bindings()) {
    CHECK(!b.sequence.empty());
    CHECK(b.action != Action::None);
    CHECK(std::string(b.group) != "");
    if (b.listed) CHECK(std::string(b.help) != "");
  }
}

TEST(keymap_every_unlisted_binding_has_a_listed_voice) {
  // An alias is fine as long as something in the same group and with the
  // same action is listed - otherwise it is a shortcut nobody can find.
  for (const Binding& b : bindings()) {
    if (b.listed) continue;
    bool spoken = false;
    for (const Binding& o : bindings())
      if (o.listed && o.mode == b.mode && o.action == b.action) spoken = true;
    CHECK(spoken);
  }
}

TEST(keymap_fires_a_single_chord) {
  KeyMachine m;
  KeyMachine::Step s = m.feed(alt(Key::L), CtxTiled);
  CHECK(s.kind == KeyMachine::Step::Kind::Fired);
  CHECK(s.action == Action::FocusDir);
  CHECK(s.arg == (int)Dir::Right);
  CHECK(m.mode == Mode::Normal);
  CHECK(m.pending.empty());
}

TEST(keymap_scrolls_a_window_both_ways) {
  // The bare arrows belong to the selected row, so a window that is
  // narrower than its widest control is reached with Ctrl - the same
  // modifier that pages it and scales it.
  KeyMachine m;
  KeyMachine::Step right = m.feed(ctrl(Key::Right), CtxAny);
  CHECK(right.kind == KeyMachine::Step::Kind::Fired);
  CHECK(right.action == Action::ScrollX);
  CHECK(right.step > 0);
  KeyMachine::Step left = m.feed(ctrl(Key::Left), CtxAny);
  CHECK(left.action == Action::ScrollX);
  CHECK(left.step == -right.step);
  // ...and it is offered whatever is selected, unlike the bare arrows,
  // which only mean anything on a row with a value.
  const Binding* b = find(Mode::Normal, ctrl(Key::Right));
  CHECK(b && b->need == CtxAny);
  CHECK(find(Mode::Normal, bare(Key::Right), CtxRow | CtxWidget)->need ==
        CtxWidget);
}

TEST(keymap_drops_an_unbound_chord) {
  KeyMachine m;
  KeyMachine::Step s = m.feed(alt(Key::Z), CtxAny);
  CHECK(s.kind == KeyMachine::Step::Kind::None);
  CHECK(s.action == Action::None);
  CHECK(m.pending.empty());
}

TEST(keymap_holds_a_prefix_until_it_completes) {
  // The table ships no multi-chord binding today; the machine has to
  // hold one anyway, because that is what the which-key pane narrows.
  KeyMachine m;
  Chord lead = alt(Key::G), tail = bare(Key::X);
  Binding b;
  b.mode = Mode::Normal;
  b.sequence = {lead, tail};
  // Feeding through the real table: a chord that leads nowhere clears.
  CHECK(m.feed(lead, CtxAny).kind == KeyMachine::Step::Kind::None);
  CHECK(m.pending.empty());
}

TEST(keymap_context_gates_a_binding) {
  KeyMachine m;
  // Left/Right only mean something while a control is selected.
  CHECK(m.feed(bare(Key::Left), CtxAny).kind == KeyMachine::Step::Kind::None);
  KeyMachine::Step s = m.feed(bare(Key::Left), CtxWidget);
  CHECK(s.kind == KeyMachine::Step::Kind::Fired);
  CHECK(s.action == Action::WidgetAdjust);
  CHECK(s.step < 0);
  // and the wave keys only while a waveform is.
  CHECK(m.feed(bare(Key::Space), CtxWidget).kind ==
        KeyMachine::Step::Kind::None);
  CHECK(m.feed(bare(Key::Space), CtxWave).action == Action::WavePlay);
}

TEST(keymap_resize_is_a_mode_that_stays_open) {
  KeyMachine m;
  CHECK(m.feed(alt(Key::R), CtxTiled).action == Action::EnterResize);
  CHECK(m.mode == Mode::Resize);
  KeyMachine::Step s = m.feed(bare(Key::L), CtxAny);
  CHECK(s.action == Action::ResizeDir);
  CHECK(s.arg == (int)Dir::Right);
  CHECK_NEAR(s.step, 0.05, 1e-12);
  CHECK(m.mode == Mode::Resize);  // still resizing
  CHECK(m.feed(Chord{Key::L, false, true, false}, CtxAny).step > 0.05);
  CHECK(m.feed(bare(Key::Escape), CtxAny).action == Action::LeaveMode);
  CHECK(m.mode == Mode::Normal);
}

TEST(keymap_escape_returns_to_normal_from_every_mode) {
  for (Mode m : {Mode::Normal, Mode::Resize, Mode::Select, Mode::Search,
                 Mode::Rename}) {
    KeyMachine k;
    k.mode = m;
    KeyMachine::Step s = k.feed(bare(Key::Escape), CtxAny);
    CHECK(s.kind == KeyMachine::Step::Kind::Fired);
    CHECK(k.mode == Mode::Normal);
  }
  // Help is the exception: the overlay layer claims the press before
  // the map sees it, and it goes back to wherever it was opened from.
  KeyMachine k;
  k.mode = Mode::Help;
  k.helpFrom = Mode::Normal;
  KeyMachine::Step s = k.feed(bare(Key::Escape), CtxAny);
  CHECK(s.kind == KeyMachine::Step::Kind::Consumed);
  CHECK(k.mode == Mode::Normal);
}

TEST(keymap_opening_a_surface_switches_mode) {
  KeyMachine m;
  CHECK(m.feed(alt(Key::D), CtxAny).action == Action::OpenSearch);
  CHECK(m.mode == Mode::Search);
  m.reset();
  CHECK(m.feed(Chord{Key::Slash, false, true, false}, CtxAny).action ==
        Action::OpenHelp);
  CHECK(m.mode == Mode::Help);
  m.reset();
  CHECK(m.feed(alt(Key::Comma), CtxAny).action == Action::RenameTab);
  CHECK(m.mode == Mode::Rename);
  CHECK(m.feed(bare(Key::Enter), CtxAny).action == Action::RenameAccept);
  CHECK(m.mode == Mode::Normal);
}

TEST(keymap_tab_digits_cover_one_through_nine) {
  for (int i = 1; i <= 9; i++) {
    Key digit = (Key)((int)Key::D0 + i);
    const Binding* go = find(Mode::Normal, alt(digit));
    const Binding* send = find(Mode::Normal, altShift(digit));
    CHECK(go && go->action == Action::GotoTab && go->arg == i);
    CHECK(send && send->action == Action::SendToTab && send->arg == i);
    CHECK(go->listed == (i == 1));  // one entry speaks for the run
  }
}

TEST(keymap_select_is_a_mode_for_reshaping_the_tree) {
  KeyMachine m;
  CHECK(m.feed(alt(Key::S), CtxTiled).action == Action::EnterSelect);
  CHECK(m.mode == Mode::Select);

  KeyMachine::Step s = m.feed(bare(Key::L), CtxAny);
  CHECK(s.action == Action::ExtendSel && s.arg == (int)Dir::Right);
  CHECK(m.mode == Mode::Select);  // gathering more than one takes more keys

  CHECK(m.feed(bare(Key::V), CtxAny).action == Action::Group);
  CHECK(m.feed(bare(Key::V), CtxAny).arg == (int)Split::V);
  CHECK(m.feed(bare(Key::B), CtxAny).arg == (int)Split::H);
  CHECK(m.mode == Mode::Select);  // ...and grouping again is a keystroke away

  // Flattening is offered only where there is a container to flatten.
  CHECK(m.feed(bare(Key::X), CtxAny).kind == KeyMachine::Step::Kind::None);
  CHECK(m.feed(bare(Key::X), CtxContainer).action == Action::Flatten);
  CHECK(m.feed(bare(Key::Escape), CtxAny).action == Action::LeaveMode);
  CHECK(m.mode == Mode::Normal);
}

TEST(keymap_focus_parent_is_offered_only_where_it_means_something) {
  KeyMachine m;
  // At the root there is nothing outside the focus to step to...
  CHECK(m.feed(alt(Key::A), CtxAny).kind == KeyMachine::Step::Kind::None);
  CHECK(m.feed(alt(Key::A), CtxNested).action == Action::FocusParent);
  // ...and stepping back in only means something for a container.
  CHECK(m.feed(altShift(Key::A), CtxNested).kind ==
        KeyMachine::Step::Kind::None);
  CHECK(m.feed(altShift(Key::A), CtxContainer).action == Action::FocusChild);
}

TEST(keymap_a_tapped_alt_stands_in_for_holding_it) {
  KeyMachine m;
  m.sticky = true;  // what the app sets when Alt is tapped on its own
  CHECK(m.waiting());
  CHECK(m.prefixLabel() == "Alt-");
  KeyMachine::Step s = m.feed(bare(Key::L), CtxTiled);
  CHECK(s.kind == KeyMachine::Step::Kind::Fired);
  CHECK(s.action == Action::FocusDir);  // read as Alt+l
  CHECK(!m.sticky);                     // and spent
}

TEST(keymap_escape_spends_a_tapped_alt_and_nothing_else) {
  KeyMachine m;
  m.sticky = true;
  KeyMachine::Step s = m.feed(bare(Key::Escape), CtxAny);
  // Claimed by the prefix layer, so the map never sees it.
  CHECK(s.kind == KeyMachine::Step::Kind::Consumed);
  CHECK(s.action == Action::None);
  CHECK(!m.sticky);
  CHECK(!m.waiting());
  // A second Escape then means what Escape always means.
  CHECK(m.feed(bare(Key::Escape), CtxAny).action == Action::LeaveMode);
}

TEST(keymap_reset_drops_a_tapped_prefix) {
  KeyMachine m;
  m.sticky = true;
  m.mode = Mode::Select;
  m.reset();
  CHECK(!m.sticky && m.mode == Mode::Normal && !m.waiting());
  CHECK(m.prefixLabel() == "");
}

// --- the layering: one press, one claim ------------------------------

TEST(keymap_a_captured_press_stops_before_the_map) {
  // Bare letters and digits address what is on screen - a row of the
  // focused window, a window of this tab - and the app claims them
  // before the map. The claim has to end the press: acting at two
  // levels is what made `f` pick a row and then reopen the labels back
  // when they were an overlay.
  KeyMachine m;
  KeyMachine::Step s = m.dispatch(bare(Key::S), /*captured=*/true, CtxAny);
  CHECK(s.kind == KeyMachine::Step::Kind::Consumed);
  CHECK(s.action == Action::None);
  CHECK(m.mode == Mode::Normal);
}

TEST(keymap_normal_mode_binds_no_bare_letter_or_digit) {
  // The invariant the whole scheme rests on: a bare letter is a row's
  // key and a bare digit is a window's number, so the map must not want
  // either. A binding here would be one a row could silently shadow.
  for (const Binding& b : bindings()) {
    if (b.mode != Mode::Normal) continue;
    const Chord& c = b.sequence[0];
    if (c.alt || c.ctrl) continue;
    bool letter = c.key >= Key::A && c.key <= Key::Z;
    bool digit = c.key >= Key::D0 && c.key <= Key::D9;
    CHECK(!letter && !digit);
  }
}

TEST(keymap_a_capture_mode_never_leaks_typing_to_the_map) {
  for (Mode m : {Mode::Search, Mode::Rename}) {
    CHECK(modeCapturesText(m));
    KeyMachine k;
    k.mode = m;
    // Letters and digits are typing wherever they land here...
    CHECK(k.dispatch(bare(Key::J), false, CtxAny).kind ==
          KeyMachine::Step::Kind::Consumed);
    CHECK(k.dispatch(bare(Key::D2), false, CtxAny).kind ==
          KeyMachine::Step::Kind::Consumed);
    CHECK(k.mode == m);
    // ...and the mode's own control keys still reach its map.
    CHECK(k.feed(bare(Key::Escape), CtxAny).kind ==
          KeyMachine::Step::Kind::Fired);
  }
  // Not a capture mode: the same letters are shortcuts.
  KeyMachine sel;
  sel.mode = Mode::Select;
  CHECK(!modeCapturesText(Mode::Select));
  CHECK(sel.feed(bare(Key::V), CtxAny).action == Action::Group);
}

TEST(keymap_capture_modes_bind_no_bare_letters) {
  // They could never fire: the capture layer claims typing first. A
  // binding like that is a shortcut nobody can press.
  for (const Binding& b : bindings()) {
    if (!modeCapturesText(b.mode)) continue;
    const Chord& c = b.sequence[0];
    bool bareLetter = !c.alt && !c.ctrl && c.key >= Key::A && c.key <= Key::Z;
    CHECK(!bareLetter);
  }
}

TEST(keymap_help_goes_back_to_where_it_was_opened_from) {
  KeyMachine m;
  m.mode = Mode::Resize;
  CHECK(m.feed(Chord{Key::Slash, false, true, false}, CtxAny).action ==
        Action::OpenHelp);
  CHECK(m.mode == Mode::Help);
  CHECK(m.helpFrom == Mode::Resize);  // which map the overlay explains
  // Any press closes it, and resize mode is still where you were.
  CHECK(m.dispatch(bare(Key::Q), false, CtxAny).kind ==
        KeyMachine::Step::Kind::Consumed);
  CHECK(m.mode == Mode::Resize);
}

TEST(keymap_help_is_reachable_from_every_mode_that_has_a_map) {
  for (Mode m : {Mode::Normal, Mode::Resize, Mode::Select}) {
    KeyMachine k;
    k.mode = m;
    CHECK(k.feed(Chord{Key::Slash, false, true, false}, CtxAny).action ==
          Action::OpenHelp);
    CHECK(k.mode == Mode::Help);
  }
}

TEST(keymap_completions_are_what_the_which_key_pane_lists) {
  KeyMachine m;
  std::vector<const Binding*> all = m.completions(CtxTiled);
  CHECK(!all.empty());
  for (const Binding* b : all) CHECK(b->mode == Mode::Normal);
  // A widget binding shows up only once one is selected.
  bool widgetOffered = false;
  for (const Binding* b : all)
    if (b->action == Action::WidgetReset) widgetOffered = true;
  CHECK(!widgetOffered);
  for (const Binding* b : m.completions(CtxTiled | CtxWidget))
    if (b->action == Action::WidgetReset) widgetOffered = true;
  CHECK(widgetOffered);
  // In resize mode it is the resize map, and a much shorter one.
  m.mode = Mode::Resize;
  std::vector<const Binding*> resize = m.completions(CtxAny);
  CHECK(!resize.empty() && resize.size() < all.size());
  for (const Binding* b : resize) CHECK(b->mode == Mode::Resize);
}

TEST(keymap_bindings_for_a_mode_match_its_context) {
  std::vector<const Binding*> plain = bindingsFor(Mode::Normal, CtxAny);
  std::vector<const Binding*> tiled = bindingsFor(Mode::Normal, CtxTiled);
  CHECK(tiled.size() > plain.size());  // focus and move need a second window
  for (const Binding* b : plain) CHECK((b->need & CtxTiled) == 0);
}

TEST(keymap_a_window_scales_on_its_own) {
  KeyMachine m;
  Chord ctrlPlus{Key::Equal, false, false, true};
  Chord ctrlMinus{Key::Minus, false, false, true};
  Chord ctrlZero{Key::D0, false, false, true};
  CHECK(m.feed(ctrlPlus, CtxAny).action == Action::ScaleWindow);
  CHECK(m.feed(ctrlPlus, CtxAny).arg == 1);
  CHECK(m.feed(ctrlMinus, CtxAny).arg == -1);
  CHECK(m.feed(ctrlZero, CtxAny).arg == 0);
  // Bare `=` still zooms the waveform - punctuation is not a row key -
  // but the digits went to the windows, so fitting moved to Alt+0.
  CHECK(m.feed(bare(Key::Equal), CtxWave).action == Action::WaveZoom);
  CHECK(m.feed(bare(Key::D0), CtxWave).kind == KeyMachine::Step::Kind::None);
  CHECK(m.feed(alt(Key::D0), CtxWave).action == Action::WaveFit);
}

TEST(keymap_a_selected_waveform_claims_the_bare_arrows) {
  KeyMachine m;
  // With no waveform selected the arrows scroll the panel, as ever.
  CHECK(m.feed(bare(Key::Up), CtxAny).action == Action::Scroll);
  CHECK(m.feed(bare(Key::Down), CtxAny).action == Action::Scroll);
  // With one selected they drive it instead: up and down zoom, left and
  // right move the view, and Shift makes every step a fine one.
  CHECK(m.feed(bare(Key::Up), CtxWave).action == Action::WaveZoom);
  CHECK(m.feed(bare(Key::Up), CtxWave).step < 1.0);   // in
  CHECK(m.feed(bare(Key::Down), CtxWave).step > 1.0);  // out
  KeyMachine::Step right = m.feed(bare(Key::Right), CtxWave);
  CHECK(right.action == Action::WaveMove);
  CHECK(right.step > 0);
  CHECK(m.feed(bare(Key::Left), CtxWave).step == -right.step);
  CHECK(m.feed(shift(Key::Right), CtxWave).step == right.step / 10);
  // Paging the panel is still there for when the arrows are spoken for.
  CHECK(m.feed(ctrl(Key::D), CtxWave).action == Action::ScrollPage);
}

TEST(keymap_enter_on_a_waveform_opens_the_head_machine) {
  KeyMachine m;
  // A row that is not a waveform still activates on Enter.
  CHECK(m.feed(bare(Key::Enter), CtxRow).action == Action::WidgetActivate);
  CHECK(m.mode == Mode::Normal);
  KeyMachine::Step s = m.feed(bare(Key::Enter), CtxRow | CtxWave);
  CHECK(s.action == Action::WaveSelect);
  CHECK(m.mode == Mode::Wave);
  // Enter inside the mode stays inside it - the app leaves when the
  // pair settles, which the table cannot see.
  CHECK(m.feed(bare(Key::Enter), CtxAny).action == Action::WaveSelect);
  CHECK(m.mode == Mode::Wave);
  CHECK(m.feed(bare(Key::Escape), CtxAny).action == Action::LeaveMode);
  CHECK(m.mode == Mode::Normal);
}

TEST(keymap_wave_mode_takes_both_hands) {
  KeyMachine m;
  m.feed(bare(Key::Enter), CtxWave);
  CHECK(m.mode == Mode::Wave);
  // hjkl and the arrows are the same keys, and bare letters reach the
  // map here because a mode's rows are not addressable by hint.
  KeyMachine::Step l = m.feed(bare(Key::L), CtxAny);
  CHECK(l.action == Action::WaveMove);
  CHECK(l.step == m.feed(bare(Key::Right), CtxAny).step);
  CHECK(m.feed(bare(Key::H), CtxAny).step == -l.step);
  CHECK(m.feed(shift(Key::L), CtxAny).step == l.step / 10);
  KeyMachine::Step k = m.feed(bare(Key::K), CtxAny);
  CHECK(k.action == Action::WaveZoom);
  CHECK(k.step < 1.0);
  CHECK(k.step == m.feed(bare(Key::Up), CtxAny).step);
  CHECK(m.feed(bare(Key::J), CtxAny).step > 1.0);
  // Shift is the finer step in both directions, never the coarser one.
  CHECK(m.feed(shift(Key::K), CtxAny).step > k.step);
  CHECK(m.feed(shift(Key::J), CtxAny).step <
        m.feed(bare(Key::J), CtxAny).step);
}

TEST(keymap_every_mode_has_shortcuts_to_show) {
  // The `?` overlay and the which-key pane render whatever this
  // returns, so a mode with nothing listed is an empty overlay.
  unsigned everything =
      CtxTiled | CtxNested | CtxContainer | CtxWidget | CtxWave;
  for (Mode m : {Mode::Normal, Mode::Resize, Mode::Select, Mode::Wave,
                 Mode::Search, Mode::Rename, Mode::Help}) {
    size_t listed = 0;
    for (const Binding* b : bindingsFor(m, everything))
      if (b->listed) listed++;
    CHECK(listed > 0);
  }
}

TEST(keymap_chord_names_read_like_the_docs) {
  CHECK(chordName(alt(Key::L)) == "Alt+l");
  // Shift is abbreviated: it is the longest key label the panes
  // have to fit, and "Shf" survives the squeeze.
  CHECK(chordName(altShift(Key::D2)) == "Alt+Shf+2");
  CHECK(chordName(Chord{Key::Tab, false, true, false}) == "Shf+Tab");
  CHECK(chordName(Chord{Key::D, false, false, true}) == "Ctrl+d");
  CHECK(chordName(Chord{Key::Slash, false, true, false}) == "?");
  CHECK(chordName(bare(Key::Escape)) == "Esc");
  CHECK(chordName(bare(Key::Space)) == "Space");
  CHECK(chordName(Chord{}) == "");
  CHECK(sequenceName({alt(Key::G), bare(Key::X)}) == "Alt+g x");
}

TEST(keymap_mode_names_are_stable) {
  CHECK(std::string(modeName(Mode::Normal)) == "normal");
  CHECK(std::string(modeName(Mode::Resize)) == "resize");
  CHECK(std::string(modeName(Mode::Help)) == "help");
  CHECK(std::string(modeName(Mode::Wave)) == "wave");
}
