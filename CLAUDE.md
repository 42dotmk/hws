# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

    make            # builds the `hws` binary from hws.c
    make install    # symlinks it into ~/.local/bin
    make clean

Single C11 file, no tests, no lint target. Requires Xlib plus the Xrender, Xcomposite, Xdamage, Xfixes, Xrandr, and Xtst extension libraries (see LDLIBS in the makefile). The style is suckless-like: `-std=c11 -pedantic -Wall -Wextra`, static globals, fixed-size arrays (`MAXWIN`, `MAXWS`), config as `static const` values in `config.h` (included by hws.c) — keep changes in that idiom. Running `hws` requires a live X11 session with an EWMH window manager, so manual testing usually can't happen inside this sandbox.

## What it is

`hws` is a niri-style workspace/window overview for X11 — a standalone EWMH client (companion to the `hwm` window manager in ../hwm, but WM-agnostic). It shows a fullscreen overlay with one row per workspace containing live window thumbnails; picking one asks the WM to switch/focus, then exits.

## Architecture (all in hws.c, ~900 lines)

Two parallel data models, rebuilt on the fly:

- `thumbs[]` (`Thumb`): one entry per viewable client window, kept in bottom-to-top stacking order (that order is what makes painting and `pickthumb` hit-testing correct). Populated by `readmodel()`, which intersects `XQueryTree` order with `_NET_CLIENT_LIST`.
- `rows[]` (`Row`): one per workspace that has windows (plus the current desktop), sorted ascending, derived from thumbs by `buildrows()`. Each row stores the bounding box of its members in root coordinates.

The flow after any model change is always: mutate/rebuild state → `buildrows()` → `layout()` → set `dirty`; `render()` runs from the main loop, throttled to `fps`, drawing everything into `backbuf` and blitting once.

Key invariants and mechanisms to preserve:

- **hws never manages windows itself.** All actions are EWMH client messages to the root window (`_NET_ACTIVE_WINDOW`, `_NET_CURRENT_DESKTOP` via `sendroot`); the WM does the switching. hws exits right after activating.
- **Live thumbnails**: subwindows are redirected with `XCompositeRedirectSubwindows` (automatic mode), each thumb gets a named backing pixmap + XRender `Picture` (`namethumbpict`), scaled with a picture transform in `drawthumb`. XDamage events just set `dirty`. If Composite ≥0.2 is unavailable or a window has no pixmap, thumbs fall back to flat title placeholders — every drawing path must handle `t->pict == None`.
- **Layout normalizes per-row**: each row scales/centers its members relative to its own bounding box, so any uniform offset — including hwm parking hidden workspaces offscreen — cancels out. Don't introduce absolute root coordinates into layout.
- **Key forwarding** (`forwardkey`): keys with Ctrl/Alt/Super (and XF86 media keys) belong to the WM. The selected window is focused first, then the key is re-injected with XTEST while the keyboard grab is briefly released so the WM's passive grab fires. Modifier keys themselves are never forwarded (would corrupt held-modifier state); Tab and Escape are always handled locally.
- **The X error handler ignores everything** — source windows can vanish at any time, so calls against them are allowed to fail silently. Model consistency is instead maintained by reacting to events (`UnmapNotify`/`DestroyNotify` → `droptumb`, `PropertyNotify` on `_NET_CLIENT_LIST` → full `rebuild()` preserving the selection).
- The main loop (`run`) is a `select()` on the X fd with a frame-rate-limited redraw; there is no timer beyond that.
