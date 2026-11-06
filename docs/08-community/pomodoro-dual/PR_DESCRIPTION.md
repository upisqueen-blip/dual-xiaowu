## Optional secondary display + minute-granularity refresh

Thanks for publishing this — the preset/cycle model and the partial-window
refresh trick are both nicer than what I had.

I run a **two-panel** e-paper device (one fast B/W panel for "right now", one
panel for "the day"), and I wanted the stats to live on the second panel
while the countdown stayed on the first. Rather than fork the drawing code,
I split it so that **single-panel builds are byte-identical to what you have
today** — everything new sits behind `EPD2_ENABLE` and a null sink pointer.

(Disclosure: the dual-panel device is a product I work on — an ESP32-C3/S3
e-ink note platform, 森穹小悟 / XiaowuAI — https://dual.xiaowuai.vip/ . It renders
content server-side rather than on the device, so it can't run this firmware;
the layout was reimplemented there independently. The abstraction here is what
I extracted after wanting the same split in both places. No code from this repo
is used in that product.)

Three commits, each one compiles and runs on its own:

### 1. `refactor(timer): expose a read-only TimerSnapshot`

`Timer` has eight states and a lot of fields that only its own layouts care
about. I added `Timer::snapshot()` returning a small POD (`TimerSnapshot`)
with just the things an outside renderer needs: phase, run state, elapsed /
remaining / overdue, cycle position, break lengths.

I deliberately did **not** reuse `TimerState` in the snapshot. The snapshot has
its own two enums (`TimerPhase`, `TimerRunState`) so the state machine stays
private and can change without breaking renderers.

### 2. `feat(display): optional second panel with a stats card`

```cpp
class SecondarySink {
public:
    virtual void onTimerSnapshot(const TimerSnapshot&, bool fullRedraw) = 0;
};
```

`Timer::loop()` calls it **after** `display.display(...)`, not at the top of
the loop. That ordering matters: if you notify before the refresh, the second
panel reads the timer one cycle earlier and you can see the two panels
disagree by a second while paused.

`fullRedraw` is forwarded so a second panel can sync its full refresh with the
main panel's, instead of accumulating ghosting at unrelated moments.

When no sink is registered the call site is a single null check — no virtual
call, no extra code in the hot path.

`SecondaryStats` is the working implementation: big number = pomodoros completed
in the current set, right side = cycle position, progress bar = distance to the
long break, footer = what happens next. It switches font scale at
`width >= 640`, so the same layout works on 400x300 and 800x480.

Refresh is rate-limited internally: it redraws when the minute changes, the
phase changes, the cycle changes, or the main panel did a full refresh. Eight
pomodoros a day works out to roughly twenty panel updates.

### 3. `feat(refresh): minute-granularity mode`

Optional `-DREFRESH_MINUTE_ONLY=1`, which drops the running redraw from 1 Hz to
once per minute boundary. For a battery build this is the difference between
~3600 partial refreshes and ~60 per focus session.

---

### Why nothing is hardcoded

I didn't put my panel model or pins in the source. Everything comes from
`build_flags` via `src/dual_config.h`:

```ini
build_flags =
    -DEPD2_ENABLE=1
    -DEPD2_DRIVER_CLASS=GxEPD2_420
    -DEPD2_CS=<yours>
    -DEPD2_DC=<shared with main>
    -DEPD2_BUSY=<yours>
    -DEPD2_RST=-1          ; shared reset line
    -DEPD2_3COLOR=1        ; only for tri-colour panels
```

Two notes that cost me time and might be worth a comment in your README:

- **`RST = -1` for the second panel.** Both panels share the reset line on my
  board. If the second `GxEPD2` instance drives RST itself, it resets the first
  panel right after the first one finished init. GxEPD2 already guards with
  `if (_rst >= 0)`, so `-1` is the clean answer.
- **`DC` must be the same pin number, not `-1`.** Both panels sit on one SPI
  bus and share DC; passing `-1` makes GxEPD2 write to pin `-1` and the panel
  never latches a command.

### License

There's no `LICENSE` file in the repo, and GitHub reports the licence as
`null` — which means the default is **all rights reserved**, not "public domain".

I'm not going to assume anything, so: if you take any of this, treat my
contribution as MIT-licensed. If you'd rather keep the repo
all-rights-reserved, that's fine too — the commits are yours to cherry-pick or
ignore, and I'll keep my copy in my own fork.

**Would you consider adding a LICENSE?** Not to get at your code — I'd rather
ask first — but because right now nobody can legally *use* this project, only
read it. It's at 1.1k stars and 39 forks with no open issues, which usually
means people are quietly building on it without a clear basis. MIT or
Apache-2.0 would fix that in one commit, and I'm happy to open that PR if you
don't want to pick one yourself.
