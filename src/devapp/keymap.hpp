#pragma once
#include <string>
#include <vector>

namespace synth::devapp {

// The dev app's shortcut logic, as a table plus a state machine. Nothing
// here draws, and nothing here mentions ImGui: `app.cpp` translates real
// key events into these chords and carries out the actions. The table is
// the single source of truth for behaviour, for the `?` help overlay and
// for the which-key pane, so a binding cannot exist without a
// description of what it does.

// The keys the app binds. Deliberately small: what the table uses and
// nothing else.
enum class Key {
  None,
  A, B, C, D, E, F, G, H, I, J, K, L, M,
  N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
  D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
  Left, Right, Up, Down,
  Enter, Escape, Tab, Space, Slash, Comma, Period, Minus, Equal,
};

struct Chord {
  Key key = Key::None;
  bool alt = false, shift = false, ctrl = false;
  auto operator<=>(const Chord&) const = default;
  bool valid() const { return key != Key::None; }
};

// What the app is showing right now, so a binding can be offered only
// where it means something. CtxRow says a row is selected; Widget and
// Wave say what kind, and at most one of them holds - a row that is
// neither (a linked group, a panel tickbox in the overview) can still
// be activated, just not nudged.
enum Ctx : unsigned {
  CtxAny = 0,
  CtxWave = 1u << 0,    // the selected element is a waveform
  CtxWidget = 1u << 1,  // the selected element is a control
  CtxTiled = 1u << 2,   // the tab holds more than one window
  CtxContainer = 1u << 3,  // the focus is on a container, not one window
  CtxNested = 1u << 4,     // the focused node sits inside a container
  CtxRow = 1u << 5,        // some row of the focused window is selected
};

// The app's modes. Normal is the resting state; Resize is i3's resize
// mode; Hint, Search and Rename capture typing, so only the bindings
// listed for them (Escape, Enter, the arrows) ever reach the machine.
enum class Mode { Normal, Resize, Select, Hint, Search, Rename, Help };
const char* modeName(Mode m);

// Whether the mode reads the typing itself - hint labels, a search
// query, a tab's new name. A key the app consumes there must not also
// reach the map, or one press acts at two levels; and such a mode must
// not bind bare letters, because it will never see them.
bool modeCapturesText(Mode m);

enum class Action {
  None,
  // focus and windows
  FocusDir, MoveDir, CloseWindow, ShowOverview,
  // tabs
  GotoTab, SendToTab, NewTab, RenameTab,
  // layout
  SplitH, SplitV, EnterResize, ResizeDir,
  // the tree itself
  EnterSelect, FocusParent, FocusChild, ExtendSel, Group, Flatten,
  // app surfaces
  OpenSearch, OpenHelp, ToggleOutline, ToggleWhichKey, EnterHint, LeaveMode,
  // the focused window's contents
  Scroll, ScrollPage, WidgetAdjust, WidgetActivate, WidgetReset, WidgetStep,
  WavePlay, WaveZoom, WaveFit, WaveLoop, ScaleWindow,
  // the capture modes
  SearchAccept, SearchStep, RenameAccept,
};

struct Binding {
  Mode mode = Mode::Normal;
  std::vector<Chord> sequence;  // usually one chord; prefixes are allowed
  Action action = Action::None;
  int arg = 0;       // a Dir, a tab number, a step count
  double step = 0;   // a resize fraction, a widget nudge, a zoom factor
  unsigned need = CtxAny;  // every bit must hold for this to apply
  const char* group = "";  // the heading it is listed under
  const char* help = "";   // what it does, in the user's words
  // An alias or one of a numbered run (Alt+2 … Alt+9): it works, but the
  // help and which-key panes show the one entry that speaks for it.
  bool listed = true;
};

// The default keymap. Built once, in listing order.
const std::vector<Binding>& bindings();

// The bindings that apply in `mode` given `ctx`, in table order.
std::vector<const Binding*> bindingsFor(Mode mode, unsigned ctx);

// Where a mode goes after an action fires.
Mode modeAfter(Mode from, Action a);

// The whole shortcut machine: one press in, one outcome out.
//
// It is a stack of small machines, tried in a fixed order, and the first
// to claim a press ends it:
//
//   1. capture   - the mode read the press itself (a hint label, a
//                  search query, a tab's new name)
//   2. overlay   - the help page, which any press closes
//   3. capture's leftovers - in such a mode, only its control keys go on
//   4. prefix    - a tapped Alt, or a part-typed sequence
//   5. map       - the binding table for the current mode
//
// Every rule about whether a press is spent lives here, in that order,
// so a press cannot act at two levels: nothing outside gets a second
// say. `dispatch` is the only way in.
struct KeyMachine {
  Mode mode = Mode::Normal;
  // The mode `?` was opened from, and the one closing it returns to:
  // reading the resize keys should not drop you out of resize mode.
  Mode helpFrom = Mode::Normal;
  std::vector<Chord> pending;
  // Alt tapped on its own, emacs-style: the next key is read as though
  // Alt were still held, so a chord is two presses rather than a hold.
  // Escape spends it without doing anything else, which is what makes
  // Esc the way out of a prefix you did not mean to start.
  bool sticky = false;

  struct Step {
    // None: nothing claimed it. Consumed: a layer above the map took
    // it. Pending: it began a sequence. Fired: a binding matched.
    enum class Kind { None, Consumed, Pending, Fired } kind = Kind::None;
    const Binding* binding = nullptr;  // set when Fired
    Action action = Action::None;
    int arg = 0;
    double step = 0;
  };

  // The single entry point. `captured` is what the mode's own reader
  // made of the press - only the app can know that a letter was a hint
  // label - and a captured press stops here, whatever else it might
  // have meant.
  Step dispatch(Chord c, bool captured, unsigned ctx);

  // A press nobody captured: the map layer on its own. `dispatch` with
  // `captured` false.
  Step feed(Chord c, unsigned ctx) { return dispatch(c, false, ctx); }

  // What could come next: everything that extends the held prefix, which
  // is the whole mode's map when nothing is held. This is what the
  // which-key pane lists.
  std::vector<const Binding*> completions(unsigned ctx) const;

  // Back to Normal with nothing held.
  void reset();

  // Whether a prefix is waiting for its next key - a held sequence, or
  // a tapped Alt.
  bool waiting() const { return sticky || !pending.empty(); }
  // What the pane and the mode line show for what has been typed so
  // far: "Alt-", "Alt+g ...", or empty.
  std::string prefixLabel() const;
};

// "Alt+Shf+l", "Ctrl+d", "?", "Esc". `sequenceName` joins a multi-chord
// sequence with spaces.
std::string keyName(Key k);
std::string chordName(const Chord& c);
std::string sequenceName(const std::vector<Chord>& s);

}  // namespace synth::devapp
