#include "keymap.hpp"

#include <algorithm>

#include "layout.hpp"  // Dir, for the direction arguments

namespace synth::devapp {

namespace {

constexpr Chord ch(Key k) { return Chord{k, false, false, false}; }
constexpr Chord alt(Key k) { return Chord{k, true, false, false}; }
constexpr Chord altShift(Key k) { return Chord{k, true, true, false}; }
constexpr Chord shift(Key k) { return Chord{k, false, true, false}; }
constexpr Chord ctrl(Key k) { return Chord{k, false, false, true}; }

int dirArg(Dir d) { return (int)d; }

// The nine digit keys, so the tab bindings can be generated rather than
// spelled out nine times.
constexpr Key kDigits[9] = {Key::D1, Key::D2, Key::D3, Key::D4, Key::D5,
                            Key::D6, Key::D7, Key::D8, Key::D9};

void add(std::vector<Binding>& v, Mode mode, Chord c, Action a, int arg,
         double step, unsigned need, const char* group, const char* help,
         bool listed = true) {
  Binding b;
  b.mode = mode;
  b.sequence = {c};
  b.action = a;
  b.arg = arg;
  b.step = step;
  b.need = need;
  b.group = group;
  b.help = help;
  b.listed = listed;
  v.push_back(std::move(b));
}

std::vector<Binding> buildTable() {
  std::vector<Binding> v;
  const Mode N = Mode::Normal;

  // --- moving around ---------------------------------------------------
  add(v, N, alt(Key::H), Action::FocusDir, dirArg(Dir::Left), 0, CtxTiled,
      "focus", "focus the window to the left (or Alt+Left)");
  add(v, N, alt(Key::J), Action::FocusDir, dirArg(Dir::Down), 0, CtxTiled,
      "focus", "focus the window below");
  add(v, N, alt(Key::K), Action::FocusDir, dirArg(Dir::Up), 0, CtxTiled,
      "focus", "focus the window above");
  add(v, N, alt(Key::L), Action::FocusDir, dirArg(Dir::Right), 0, CtxTiled,
      "focus", "focus the window to the right");
  add(v, N, alt(Key::Left), Action::FocusDir, dirArg(Dir::Left), 0, CtxTiled,
      "focus", "", false);
  add(v, N, alt(Key::Down), Action::FocusDir, dirArg(Dir::Down), 0, CtxTiled,
      "focus", "", false);
  add(v, N, alt(Key::Up), Action::FocusDir, dirArg(Dir::Up), 0, CtxTiled,
      "focus", "", false);
  add(v, N, alt(Key::Right), Action::FocusDir, dirArg(Dir::Right), 0, CtxTiled,
      "focus", "", false);

  // --- rearranging -----------------------------------------------------
  add(v, N, altShift(Key::H), Action::MoveDir, dirArg(Dir::Left), 0, CtxTiled,
      "windows", "move this window left (or Alt+Shf+Left)");
  add(v, N, altShift(Key::J), Action::MoveDir, dirArg(Dir::Down), 0, CtxTiled,
      "windows", "move this window down");
  add(v, N, altShift(Key::K), Action::MoveDir, dirArg(Dir::Up), 0, CtxTiled,
      "windows", "move this window up");
  add(v, N, altShift(Key::L), Action::MoveDir, dirArg(Dir::Right), 0, CtxTiled,
      "windows", "move this window right");
  add(v, N, altShift(Key::Left), Action::MoveDir, dirArg(Dir::Left), 0, CtxTiled,
      "windows", "", false);
  add(v, N, altShift(Key::Down), Action::MoveDir, dirArg(Dir::Down), 0, CtxTiled,
      "windows", "", false);
  add(v, N, altShift(Key::Up), Action::MoveDir, dirArg(Dir::Up), 0, CtxTiled,
      "windows", "", false);
  add(v, N, altShift(Key::Right), Action::MoveDir, dirArg(Dir::Right), 0,
      CtxTiled, "windows", "", false);
  add(v, N, alt(Key::Q), Action::CloseWindow, 0, 0, CtxAny, "windows",
      "close this window (unticks its panel)");
  add(v, N, alt(Key::O), Action::ShowOverview, 0, 0, CtxAny, "windows",
      "put the overview in this tab");
  add(v, N, alt(Key::B), Action::SplitH, 0, 0, CtxAny, "windows",
      "the next window opens to the right of this one");
  add(v, N, alt(Key::V), Action::SplitV, 0, 0, CtxAny, "windows",
      "the next window opens below this one");
  add(v, N, alt(Key::R), Action::EnterResize, 0, 0, CtxTiled, "windows",
      "resize mode");

  // --- the tree ---------------------------------------------------------
  add(v, N, alt(Key::A), Action::FocusParent, 0, 0, CtxNested, "tree",
      "focus the container this is in - then move, resize or close it whole");
  add(v, N, altShift(Key::A), Action::FocusChild, 0, 0, CtxContainer, "tree",
      "focus back into the container");
  add(v, N, alt(Key::S), Action::EnterSelect, 0, 0, CtxTiled, "tree",
      "select mode: gather neighbours, group them, flatten a container");

  // --- tabs -------------------------------------------------------------
  for (int i = 0; i < 9; i++) {
    add(v, N, alt(kDigits[i]), Action::GotoTab, i + 1, 0, CtxAny, "tabs",
        i == 0 ? "go to tab 1 - 9 (creating it if it is not there yet)" : "",
        i == 0);
    add(v, N, altShift(kDigits[i]), Action::SendToTab, i + 1, 0, CtxAny, "tabs",
        i == 0 ? "send this window to tab 1 - 9" : "", i == 0);
  }
  add(v, N, alt(Key::N), Action::NewTab, 0, 0, CtxAny, "tabs",
      "new tab, with the overview in it");
  add(v, N, alt(Key::Comma), Action::RenameTab, 0, 0, CtxAny, "tabs",
      "rename this tab");

  // --- the app itself ---------------------------------------------------
  add(v, N, alt(Key::D), Action::OpenSearch, 0, 0, CtxAny, "app",
      "search tabs, windows and the things inside them (or Alt+/)");
  add(v, N, alt(Key::Slash), Action::OpenSearch, 0, 0, CtxAny, "app", "",
      false);
  add(v, N, shift(Key::Slash), Action::OpenHelp, 0, 0, CtxAny, "app",
      "? - the shortcuts that apply right here");
  add(v, N, alt(Key::T), Action::ToggleOutline, 0, 0, CtxAny, "app",
      "show the tab's layout as a tree");
  add(v, N, alt(Key::W), Action::ToggleWhichKey, 0, 0, CtxAny, "app",
      "show shortcuts as you type them");

  // --- inside the focused window ---------------------------------------
  add(v, N, ch(Key::F), Action::EnterHint, 0, 0, CtxAny, "window",
      "label every widget; type a label to pick one");
  add(v, N, ch(Key::J), Action::Scroll, 0, 3, CtxAny, "window",
      "scroll down");
  add(v, N, ch(Key::K), Action::Scroll, 0, -3, CtxAny, "window", "scroll up");
  add(v, N, ctrl(Key::D), Action::ScrollPage, 0, 0.5, CtxAny, "window",
      "half a page down");
  add(v, N, ctrl(Key::U), Action::ScrollPage, 0, -0.5, CtxAny, "window",
      "half a page up");
  add(v, N, ctrl(Key::Equal), Action::ScaleWindow, 1, 0, CtxAny, "window",
      "draw this window bigger (Ctrl+0 puts it back)");
  add(v, N, ctrl(Key::Minus), Action::ScaleWindow, -1, 0, CtxAny, "window",
      "draw it smaller");
  add(v, N, ctrl(Key::D0), Action::ScaleWindow, 0, 0, CtxAny, "window", "",
      false);
  add(v, N, ch(Key::Escape), Action::LeaveMode, 0, 0, CtxAny, "window",
      "let go of the selected widget");

  // --- a selected control ----------------------------------------------
  add(v, N, ch(Key::L), Action::WidgetAdjust, 0, 0.04, CtxWidget, "widget",
      "nudge it up (Shf for a fine step)");
  add(v, N, ch(Key::H), Action::WidgetAdjust, 0, -0.04, CtxWidget, "widget",
      "nudge it down");
  add(v, N, shift(Key::L), Action::WidgetAdjust, 0, 0.004, CtxWidget, "widget",
      "", false);
  add(v, N, shift(Key::H), Action::WidgetAdjust, 0, -0.004, CtxWidget, "widget",
      "", false);
  add(v, N, ch(Key::Right), Action::WidgetAdjust, 0, 0.04, CtxWidget, "widget",
      "", false);
  add(v, N, ch(Key::Left), Action::WidgetAdjust, 0, -0.04, CtxWidget, "widget",
      "", false);
  add(v, N, ch(Key::Enter), Action::WidgetActivate, 0, 0, CtxRow, "widget",
      "flip a tickbox, take the next option, or show/hide a panel");
  add(v, N, ch(Key::R), Action::WidgetReset, 0, 0, CtxWidget, "widget",
      "back to the value the source declares");
  add(v, N, ch(Key::Tab), Action::WidgetStep, 1, 0, CtxAny, "window",
      "select the next widget (Shf+Tab for the one before)");
  add(v, N, shift(Key::Tab), Action::WidgetStep, -1, 0, CtxAny, "window", "",
      false);

  // --- a selected waveform ---------------------------------------------
  add(v, N, ch(Key::Space), Action::WavePlay, 0, 0, CtxWave, "wave",
      "play the selection, or stop");
  add(v, N, ch(Key::Equal), Action::WaveZoom, 0, 1.0 / 1.5, CtxWave, "wave",
      "zoom in");
  add(v, N, ch(Key::Minus), Action::WaveZoom, 0, 1.5, CtxWave, "wave",
      "zoom out");
  add(v, N, ch(Key::D0), Action::WaveFit, 0, 0, CtxWave, "wave",
      "fit the whole artifact");
  add(v, N, ch(Key::P), Action::WaveLoop, 0, 0, CtxWave, "wave",
      "loop what you play");

  // --- resize mode ------------------------------------------------------
  const Mode R = Mode::Resize;
  struct { Key key; Dir dir; double step; const char* help; } resizes[] = {
      {Key::L, Dir::Right, 0.05, "wider"},
      {Key::H, Dir::Right, -0.05, "narrower"},
      {Key::J, Dir::Down, 0.05, "taller"},
      {Key::K, Dir::Down, -0.05, "shorter"},
  };
  for (auto& r : resizes) {
    add(v, R, ch(r.key), Action::ResizeDir, dirArg(r.dir), r.step, CtxAny,
        "resize", r.help);
    add(v, R, shift(r.key), Action::ResizeDir, dirArg(r.dir), r.step * 3,
        CtxAny, "resize", "", false);
  }
  add(v, R, ch(Key::Right), Action::ResizeDir, dirArg(Dir::Right), 0.05, CtxAny,
      "resize", "", false);
  add(v, R, ch(Key::Left), Action::ResizeDir, dirArg(Dir::Right), -0.05, CtxAny,
      "resize", "", false);
  add(v, R, ch(Key::Down), Action::ResizeDir, dirArg(Dir::Down), 0.05, CtxAny,
      "resize", "", false);
  add(v, R, ch(Key::Up), Action::ResizeDir, dirArg(Dir::Down), -0.05, CtxAny,
      "resize", "", false);
  add(v, R, shift(Key::Slash), Action::OpenHelp, 0, 0, CtxAny, "resize",
      "? - these shortcuts, written out");
  add(v, R, ch(Key::Escape), Action::LeaveMode, 0, 0, CtxAny, "resize",
      "done (or Enter)");
  add(v, R, ch(Key::Enter), Action::LeaveMode, 0, 0, CtxAny, "resize", "",
      false);

  // --- select mode ------------------------------------------------------
  // Everything about the shape of the tree, in one place: which
  // neighbours are gathered, and what becomes of them.
  const Mode S = Mode::Select;
  struct { Key key; Dir dir; const char* help; } extends[] = {
      {Key::L, Dir::Right, "take in the neighbour to the right"},
      {Key::H, Dir::Left, "take in the neighbour to the left"},
      {Key::J, Dir::Down, "take in the neighbour below"},
      {Key::K, Dir::Up, "take in the neighbour above"},
  };
  for (auto& e : extends)
    add(v, S, ch(e.key), Action::ExtendSel, dirArg(e.dir), 0, CtxAny, "select",
        e.help);
  add(v, S, ch(Key::Right), Action::ExtendSel, dirArg(Dir::Right), 0, CtxAny,
      "select", "", false);
  add(v, S, ch(Key::Left), Action::ExtendSel, dirArg(Dir::Left), 0, CtxAny,
      "select", "", false);
  add(v, S, ch(Key::Down), Action::ExtendSel, dirArg(Dir::Down), 0, CtxAny,
      "select", "", false);
  add(v, S, ch(Key::Up), Action::ExtendSel, dirArg(Dir::Up), 0, CtxAny,
      "select", "", false);
  add(v, S, ch(Key::B), Action::Group, (int)Split::H, 0, CtxAny, "select",
      "group what is selected side by side");
  add(v, S, ch(Key::V), Action::Group, (int)Split::V, 0, CtxAny, "select",
      "group it stacked");
  add(v, S, ch(Key::X), Action::Flatten, 0, 0, CtxContainer, "select",
      "flatten this container: its windows join the one outside it");
  add(v, S, ch(Key::A), Action::FocusParent, 0, 0, CtxNested, "select",
      "select the container this is in");
  add(v, S, shift(Key::A), Action::FocusChild, 0, 0, CtxContainer, "select",
      "select back into the container");
  add(v, S, shift(Key::Slash), Action::OpenHelp, 0, 0, CtxAny, "select",
      "? - these shortcuts, written out");
  add(v, S, ch(Key::Escape), Action::LeaveMode, 0, 0, CtxAny, "select",
      "done (or Enter)");
  add(v, S, ch(Key::Enter), Action::LeaveMode, 0, 0, CtxAny, "select", "",
      false);

  // --- the capture modes ------------------------------------------------
  // The app reads the typing itself; only these reach the machine.
  add(v, Mode::Hint, ch(Key::Escape), Action::LeaveMode, 0, 0, CtxAny, "hint",
      "give up on the labels");
  add(v, Mode::Search, ch(Key::Escape), Action::LeaveMode, 0, 0, CtxAny,
      "search", "close the search");
  add(v, Mode::Search, ch(Key::Enter), Action::SearchAccept, 0, 0, CtxAny,
      "search", "go to the highlighted result");
  add(v, Mode::Search, ch(Key::Down), Action::SearchStep, 1, 0, CtxAny,
      "search", "next result");
  add(v, Mode::Search, ch(Key::Up), Action::SearchStep, -1, 0, CtxAny, "search",
      "previous result");
  add(v, Mode::Rename, ch(Key::Escape), Action::LeaveMode, 0, 0, CtxAny,
      "rename", "keep the old name");
  add(v, Mode::Rename, ch(Key::Enter), Action::RenameAccept, 0, 0, CtxAny,
      "rename", "rename the tab");
  add(v, Mode::Help, ch(Key::Escape), Action::LeaveMode, 0, 0, CtxAny, "help",
      "close (any key does)");
  return v;
}

bool applies(const Binding& b, unsigned ctx) {
  return (b.need & ctx) == b.need;
}

bool startsWith(const std::vector<Chord>& seq, const std::vector<Chord>& pre) {
  if (seq.size() < pre.size()) return false;
  return std::equal(pre.begin(), pre.end(), seq.begin());
}

}  // namespace

const std::vector<Binding>& bindings() {
  static const std::vector<Binding> table = buildTable();
  return table;
}

std::vector<const Binding*> bindingsFor(Mode mode, unsigned ctx) {
  std::vector<const Binding*> out;
  for (const Binding& b : bindings())
    if (b.mode == mode && applies(b, ctx)) out.push_back(&b);
  return out;
}

const char* modeName(Mode m) {
  switch (m) {
    case Mode::Normal: return "normal";
    case Mode::Resize: return "resize";
    case Mode::Select: return "select";
    case Mode::Hint: return "hint";
    case Mode::Search: return "search";
    case Mode::Rename: return "rename";
    case Mode::Help: return "help";
  }
  return "normal";
}

bool modeCapturesText(Mode m) {
  return m == Mode::Hint || m == Mode::Search || m == Mode::Rename;
}

Mode modeAfter(Mode from, Action a) {
  switch (a) {
    case Action::EnterResize: return Mode::Resize;
    case Action::EnterSelect: return Mode::Select;
    case Action::EnterHint: return Mode::Hint;
    case Action::OpenSearch: return Mode::Search;
    case Action::OpenHelp: return Mode::Help;
    case Action::RenameTab: return Mode::Rename;
    case Action::LeaveMode:
    case Action::SearchAccept:
    case Action::RenameAccept: return Mode::Normal;
    // Everything else happens where it was asked for. Resize mode in
    // particular stays open across repeated resizes, which is the point
    // of it being a mode.
    default: return from;
  }
}

namespace {

// What a capture mode still lets through to its own small map:
// everything that is not typing.
bool controlKey(Key k) {
  return k == Key::Escape || k == Key::Enter || k == Key::Tab ||
         k == Key::Up || k == Key::Down || k == Key::Left || k == Key::Right;
}

}  // namespace

KeyMachine::Step KeyMachine::dispatch(Chord c, bool captured, unsigned ctx) {
  Step out;

  // 1. The mode read this press itself. It is spent: letting it fall
  //    through would have one press act at two levels - picking the
  //    hint labelled `f` and then reopening the labels.
  if (captured) {
    out.kind = Step::Kind::Consumed;
    return out;
  }
  if (!c.valid()) return out;

  // 2. The help page is not a mode with a map: any press closes it, and
  //    puts you back where you asked from.
  if (mode == Mode::Help) {
    mode = helpFrom;
    pending.clear();
    sticky = false;
    out.kind = Step::Kind::Consumed;
    return out;
  }

  // 3. A mode that reads its own typing keeps everything but its
  //    control keys, so a letter never reaches the map behind it.
  if (modeCapturesText(mode) && !controlKey(c.key)) {
    out.kind = Step::Kind::Consumed;
    return out;
  }

  // 4. A tapped Alt stands in for holding it, and Escape is exactly
  //    what cancels it: spend the prefix, do nothing else, and a second
  //    Escape then means what it always means.
  if (sticky) {
    sticky = false;
    if (c.key == Key::Escape && !c.alt && !c.ctrl && !c.shift) {
      out.kind = Step::Kind::Consumed;
      return out;
    }
    c.alt = true;
  }

  // 5. The map.
  std::vector<Chord> tried = pending;
  tried.push_back(c);

  const Binding* exact = nullptr;
  bool prefix = false;
  for (const Binding& b : bindings()) {
    if (b.mode != mode || !applies(b, ctx)) continue;
    if (!startsWith(b.sequence, tried)) continue;
    if (b.sequence.size() == tried.size()) {
      if (!exact) exact = &b;
    } else {
      prefix = true;
    }
  }

  if (exact) {
    pending.clear();
    out.kind = Step::Kind::Fired;
    out.binding = exact;
    out.action = exact->action;
    out.arg = exact->arg;
    out.step = exact->step;
    if (exact->action == Action::OpenHelp) helpFrom = mode;
    mode = modeAfter(mode, exact->action);
    return out;
  }
  if (prefix) {
    pending = std::move(tried);
    out.kind = Step::Kind::Pending;
    return out;
  }
  pending.clear();
  return out;
}

std::vector<const Binding*> KeyMachine::completions(unsigned ctx) const {
  std::vector<const Binding*> out;
  for (const Binding& b : bindings()) {
    if (b.mode != mode || !applies(b, ctx)) continue;
    if (b.sequence.size() <= pending.size()) continue;
    if (!startsWith(b.sequence, pending)) continue;
    out.push_back(&b);
  }
  return out;
}

void KeyMachine::reset() {
  mode = Mode::Normal;
  pending.clear();
  sticky = false;
}

std::string KeyMachine::prefixLabel() const {
  if (!pending.empty()) return sequenceName(pending) + " ...";
  if (sticky) return "Alt-";
  return "";
}

std::string keyName(Key k) {
  if (k >= Key::A && k <= Key::Z)
    return std::string(1, (char)('a' + ((int)k - (int)Key::A)));
  if (k >= Key::D0 && k <= Key::D9)
    return std::string(1, (char)('0' + ((int)k - (int)Key::D0)));
  switch (k) {
    case Key::Left: return "Left";
    case Key::Right: return "Right";
    case Key::Up: return "Up";
    case Key::Down: return "Down";
    case Key::Enter: return "Enter";
    case Key::Escape: return "Esc";
    case Key::Tab: return "Tab";
    case Key::Space: return "Space";
    case Key::Slash: return "/";
    case Key::Comma: return ",";
    case Key::Period: return ".";
    case Key::Minus: return "-";
    case Key::Equal: return "=";
    default: return "";
  }
}

std::string chordName(const Chord& c) {
  if (!c.valid()) return "";
  // The one chord everybody writes as a character rather than as a
  // modifier and a key.
  if (c.key == Key::Slash && c.shift && !c.alt && !c.ctrl) return "?";
  std::string s;
  if (c.ctrl) s += "Ctrl+";
  if (c.alt) s += "Alt+";
  // Abbreviated: a shift chord is the longest thing the which-key pane
  // has to fit in its key column, and it is the one modifier whose name
  // does not survive being squeezed.
  if (c.shift) s += "Shf+";
  return s + keyName(c.key);
}

std::string sequenceName(const std::vector<Chord>& s) {
  std::string out;
  for (const Chord& c : s) {
    if (!out.empty()) out += " ";
    out += chordName(c);
  }
  return out;
}

}  // namespace synth::devapp
