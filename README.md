# hws

A niri-style workspace/window overview for X11, built as a companion to
[hwm](https://github.com/42dotmk/hwm) but speaking plain EWMH — it works under any EWMH window
manager.

Run it and a fullscreen overlay shows every workspace as a row of window
thumbnails, stacked vertically. Pick a window and the WM switches and
focuses it.

Thumbnails are **live**: windows are redirected with XComposite and their
backing pixmaps are composited scaled with XRender; XDamage repaints them
as they change. hwm keeps hidden workspaces mapped (parked offscreen), so
even windows on other workspaces render live content. Under a WM that
unmaps hidden windows they fall back to flat title placeholders.

## Keys

| key                 | action                                  |
| ------------------- | --------------------------------------- |
| j/k (or Down/Up)    | select workspace row                    |
| h/l (or Left/Right) | select window within the row            |
| Tab                 | next window                             |
| 0..9                | jump to workspace row                   |
| Enter / Space       | focus the selection (or switch to an empty workspace) |
| Esc / q             | cancel, restoring the original focus    |
| mouse               | hover selects, click focuses, wheel scrolls rows; clicking a row's empty area switches to that workspace |

**Window-manager keys pass through.** Any key with Ctrl/Alt/Super (and the
XF86 media keys) is handed to the window manager, targeting the *selected*
window: the selection is focused first (`_NET_ACTIVE_WINDOW`), then the key
is re-injected with XTEST while the grab is briefly released, so the WM's
own binding fires. So Mod+f fullscreens the selected window, Mod+Shift+3
sends it to workspace 3, Mod+q closes it — all without leaving the
overview, which tracks the changes live. Tab and Escape always stay local.

## Build

    make && make install    # symlinks into ~/.local/bin

Needs Xlib, XComposite, XDamage, XRender, XRandR. Bind it in hwm's
`config.c`, e.g. `{ MODKEY, XK_Tab, spawn, { .v = switchercmd } }` with
`static const char *switchercmd[] = { "hws", NULL };`.

## How selection works

hws never touches windows itself: it sends `_NET_ACTIVE_WINDOW` (or
`_NET_CURRENT_DESKTOP` for an empty workspace) to the root window and
exits; the window manager does the switching, focusing, and scrolling.
