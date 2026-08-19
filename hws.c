/* hws - a niri-style workspace/window overview switcher for X11.
 *
 * A standalone EWMH client: it reads the window model from the root window
 * (_NET_CLIENT_LIST, _NET_WM_DESKTOP, ...), shows every workspace as a row
 * of window thumbnails on a fullscreen overlay, and activates the selection
 * with a _NET_ACTIVE_WINDOW client message — the window manager does the
 * actual switching and focusing.
 *
 * Thumbnails are live: windows are redirected offscreen with XComposite and
 * their backing pixmaps composited scaled with XRender; XDamage repaints
 * them while they change. Built for hwm, which keeps hidden workspaces
 * mapped (parked offscreen), so even their thumbnails have live content;
 * any EWMH WM works, but unmapped windows show as flat placeholders.
 */
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

/* config */
static const char col_bg[] = "#1a1b26";     /* overlay background */
static const char col_row[] = "#24283b";    /* selected row slab */
static const char col_sel[] = "#7aa2f7";    /* selected thumbnail border */
static const char col_unsel[] = "#3b4261";  /* other thumbnail borders */
static const char col_fg[] = "#c0caf5";     /* text */
static const char fontname[] = "10x20";     /* core X font ("fixed" fallback) */
static const int pad = 32;                  /* overlay margin */
static const int rowgap = 20;               /* px between workspace rows */
static const int labelw = 56;               /* workspace number gutter */
static const int minrowh = 72;              /* rows never get smaller */
static const int thumbbw = 2;               /* thumbnail border width */
static const float maxscale = 0.30f;        /* thumbnails never get bigger */
static const int fps = 30;                  /* live refresh limit */
/* keys carrying one of these modifiers are not ours: they are forwarded to
 * the window manager, targeting the selected window (see forwardkey) */
static const unsigned int fwdmods = ControlMask | Mod1Mask | Mod4Mask;

#define MAXWIN 256
#define MAXWS 64
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))

typedef struct {
  Window win;
  int x, y, w, h; /* root-relative geometry incl. border */
  long desk;
  Visual *vis;
  Pixmap pix;     /* composite backing pixmap, None without redirection */
  Picture pict;
  Damage dmg;
  XRectangle dst; /* where the thumbnail sits on the overlay */
  char title[256];
} Thumb;

typedef struct {
  long desk;
  int y, h;               /* slab on the overlay */
  int bx, by, bbw, bbh;   /* bounding box of member windows (root coords) */
  float scale;
} Row;

static Display *dpy;
static int screen, xfd;
static Window root, overlay;
static int mx, my, mw, mh; /* monitor the overlay covers */
static Pixmap backbuf;
static Picture backpict;
static GC gc;
static XFontStruct *font;
static unsigned long bgpx, rowpx, selpx, unselpx, fgpx;
static Thumb thumbs[MAXWIN]; /* bottom-to-top stacking order */
static int nthumbs;
static Row rows[MAXWS]; /* one per used workspace, ascending */
static int nrows;
static int selrow;
static int selthumb = -1; /* index into thumbs[], -1 = none/empty row */
static int rowoff;        /* vertical scroll of the row list */
static long curdesk;
static Window initialactive; /* focus to restore when cancelling */
static Window lastfocused;   /* what the WM currently focuses */
static Window pausewin;      /* OR dialog (hmenu, ...) we yielded input to */
static int damagebase = -1;
static int redirected;
static int titleh;
static int dirty = 1;
static int running = 1;
static Atom netclientlist, netwmdesktop, netcurdesktop, netactivewindow,
    netwmname, netwmwindowtype, netwmtypedialog, utf8;

static void die(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  exit(1);
}

/* source windows come and go under us; ignore everything */
static int xerror(Display *d, XErrorEvent *ee) {
  (void)d;
  (void)ee;
  return 0;
}

static long msnow(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned long getcolor(const char *name) {
  XColor c, exact;

  if (!XAllocNamedColor(dpy, DefaultColormap(dpy, screen), name, &c, &exact))
    die("hws: cannot allocate color");
  return c.pixel;
}

static long getcard(Window w, Atom prop, long def) {
  Atom real;
  int fmt;
  unsigned long n, extra;
  unsigned char *p = NULL;
  long v = def;

  if (XGetWindowProperty(dpy, w, prop, 0L, 1L, False, XA_CARDINAL, &real, &fmt,
                         &n, &extra, &p) == Success &&
      p) {
    if (n)
      v = *(long *)p;
    XFree(p);
  }
  return v;
}

static void gettitle(Window w, char *buf, size_t len) {
  Atom real;
  int fmt, i;
  unsigned long n, extra;
  unsigned char *p = NULL;
  char *name = NULL;

  buf[0] = '\0';
  if (XGetWindowProperty(dpy, w, netwmname, 0L, 64L, False, utf8, &real, &fmt,
                         &n, &extra, &p) == Success &&
      p) {
    strncpy(buf, (char *)p, len - 1);
    buf[len - 1] = '\0';
    XFree(p);
  } else if (XFetchName(dpy, w, &name) && name) {
    strncpy(buf, name, len - 1);
    buf[len - 1] = '\0';
    XFree(name);
  }
  for (i = 0; buf[i]; i++) /* core fonts can't draw multibyte text */
    if ((unsigned char)buf[i] > 126 || (unsigned char)buf[i] < 32)
      buf[i] = '?';
}

static int isdialog(Window w) {
  Atom real;
  int fmt, found = 0;
  unsigned long n, i, extra;
  unsigned char *p = NULL;

  if (XGetWindowProperty(dpy, w, netwmwindowtype, 0L, 8L, False, XA_ATOM,
                         &real, &fmt, &n, &extra, &p) == Success &&
      p) {
    for (i = 0; i < n; i++)
      if (((Atom *)p)[i] == netwmtypedialog)
        found = 1;
    XFree(p);
  }
  return found;
}

static int thumbidx(Window w) {
  int i;

  for (i = 0; i < nthumbs; i++)
    if (thumbs[i].win == w)
      return i;
  return -1;
}

static int rowof(long desk) {
  int i;

  for (i = 0; i < nrows; i++)
    if (rows[i].desk == desk)
      return i;
  return -1;
}

static void freethumbpict(Thumb *t) {
  if (t->pict)
    XRenderFreePicture(dpy, t->pict);
  if (t->pix)
    XFreePixmap(dpy, t->pix);
  t->pict = None;
  t->pix = None;
}

static void namethumbpict(Thumb *t) {
  XRenderPictFormat *f = XRenderFindVisualFormat(dpy, t->vis);

  freethumbpict(t);
  if (!redirected || !f)
    return;
  t->pix = XCompositeNameWindowPixmap(dpy, t->win);
  if (t->pix)
    t->pict = XRenderCreatePicture(dpy, t->pix, f, 0, NULL);
}

/* workspace rows: one per desktop that has windows, plus the current one */
static void buildrows(void) {
  Thumb *t;
  Row *r;
  int i, j;

  nrows = 0;
  for (i = 0; i < nthumbs; i++) {
    if (rowof(thumbs[i].desk) >= 0 || nrows == MAXWS)
      continue;
    for (j = nrows; j > 0 && rows[j - 1].desk > thumbs[i].desk; j--)
      rows[j] = rows[j - 1];
    rows[j] = (Row){.desk = thumbs[i].desk};
    nrows++;
  }
  if (rowof(curdesk) < 0 && nrows < MAXWS) {
    for (j = nrows; j > 0 && rows[j - 1].desk > curdesk; j--)
      rows[j] = rows[j - 1];
    rows[j] = (Row){.desk = curdesk};
    nrows++;
  }
  for (i = 0; i < nrows; i++) {
    r = &rows[i];
    r->bx = r->by = r->bbw = r->bbh = 0;
    for (j = 0; j < nthumbs; j++) {
      t = &thumbs[j];
      if (t->desk != r->desk)
        continue;
      if (!r->bbw) { /* first member */
        r->bx = t->x;
        r->by = t->y;
        r->bbw = t->w;
        r->bbh = t->h;
        continue;
      }
      if (t->x < r->bx) {
        r->bbw += r->bx - t->x;
        r->bx = t->x;
      }
      if (t->y < r->by) {
        r->bbh += r->by - t->y;
        r->by = t->y;
      }
      r->bbw = MAX(r->bbw, t->x + t->w - r->bx);
      r->bbh = MAX(r->bbh, t->y + t->h - r->by);
    }
  }
}

/* read the EWMH model; thumbs end up in bottom-to-top stacking order */
static void readmodel(void) {
  Window d1, d2, *tree = NULL, *list = NULL;
  XWindowAttributes wa;
  Atom real;
  int fmt, k;
  unsigned int i, ntree = 0;
  unsigned long j, nlist = 0, extra;
  unsigned char *p = NULL;
  Thumb *t;

  for (k = 0; k < nthumbs; k++) {
    freethumbpict(&thumbs[k]);
    if (thumbs[k].dmg)
      XDamageDestroy(dpy, thumbs[k].dmg);
  }
  curdesk = getcard(root, netcurdesktop, 0);
  if (XGetWindowProperty(dpy, root, netclientlist, 0L, MAXWIN, False,
                         XA_WINDOW, &real, &fmt, &nlist, &extra,
                         &p) == Success &&
      p)
    list = (Window *)p;
  nthumbs = 0;
  if (XQueryTree(dpy, root, &d1, &d2, &tree, &ntree)) {
    for (i = 0; i < ntree && nthumbs < MAXWIN; i++) {
      for (j = 0; j < nlist && list[j] != tree[i]; j++)
        ;
      if (j == nlist)
        continue;
      if (!XGetWindowAttributes(dpy, tree[i], &wa) ||
          wa.map_state != IsViewable)
        continue;
      t = &thumbs[nthumbs++];
      memset(t, 0, sizeof(*t));
      t->win = tree[i];
      t->x = wa.x;
      t->y = wa.y;
      t->w = wa.width + 2 * wa.border_width;
      t->h = wa.height + 2 * wa.border_width;
      t->vis = wa.visual;
      t->desk = getcard(t->win, netwmdesktop, 0);
      gettitle(t->win, t->title, sizeof(t->title));
      namethumbpict(t);
      XSelectInput(dpy, t->win, StructureNotifyMask | PropertyChangeMask);
      if (damagebase >= 0)
        t->dmg = XDamageCreate(dpy, t->win, XDamageReportNonEmpty);
    }
    if (tree)
      XFree(tree);
  }
  if (list)
    XFree(list);
  buildrows();
}

static void layout(void);
static void selectrow(int nr);
static void grabkb(void);
static void grabinput(void);

/* re-read everything (new/closed windows), keeping the selection */
static void rebuild(void) {
  Window selwin = selthumb >= 0 ? thumbs[selthumb].win : None;
  long desk = nrows ? rows[selrow].desk : curdesk;
  int i;

  readmodel();
  selthumb = thumbidx(selwin);
  i = selthumb >= 0 ? rowof(thumbs[selthumb].desk) : rowof(desk);
  selrow = MAX(0, i);
  if (selthumb < 0)
    selectrow(selrow);
  layout();
}

static void droptumb(int i) {
  freethumbpict(&thumbs[i]);
  if (thumbs[i].dmg)
    XDamageDestroy(dpy, thumbs[i].dmg);
  memmove(&thumbs[i], &thumbs[i + 1], (nthumbs - i - 1) * sizeof(Thumb));
  nthumbs--;
  if (selthumb == i)
    selthumb = -1;
  else if (selthumb > i)
    selthumb--;
  buildrows();
}

/* the parked/scrolled position of a strip is irrelevant: every row is
 * normalized by its own bounding box, so any uniform offset (including
 * hwm's offscreen parking) cancels out */
static void layout(void) {
  Row *r;
  Thumb *t;
  int i, j, rowh, aw, ah, rowtop, stripw, striph;
  float s;

  aw = mw - 2 * pad - labelw;
  ah = mh - 2 * pad - titleh;
  rowh = nrows ? (ah - (nrows - 1) * rowgap) / nrows : ah;
  rowh = MAX(minrowh, rowh);
  rowtop = pad + selrow * (rowh + rowgap);
  if (rowtop - rowoff < pad)
    rowoff = rowtop - pad;
  if (rowtop + rowh - rowoff > pad + ah)
    rowoff = rowtop + rowh - pad - ah;
  for (i = 0; i < nrows; i++) {
    r = &rows[i];
    r->y = pad + i * (rowh + rowgap) - rowoff;
    r->h = rowh;
    if (!r->bbw)
      continue;
    s = MIN((float)aw / (float)r->bbw,
            (float)(rowh - 2 * thumbbw) / (float)r->bbh);
    s = MIN(s, maxscale);
    r->scale = s;
    stripw = (int)((float)r->bbw * s);
    striph = (int)((float)r->bbh * s);
    for (j = 0; j < nthumbs; j++) {
      t = &thumbs[j];
      if (t->desk != r->desk)
        continue;
      t->dst.x = (short)(pad + labelw + (aw - stripw) / 2 +
                         (int)((float)(t->x - r->bx) * s));
      t->dst.y = (short)(r->y + (rowh - striph) / 2 +
                         (int)((float)(t->y - r->by) * s));
      t->dst.width = (unsigned short)MAX(1, (int)((float)t->w * s));
      t->dst.height = (unsigned short)MAX(1, (int)((float)t->h * s));
    }
  }
  dirty = 1;
}

static void drawtext(int x, int y, unsigned long px, const char *s) {
  XSetForeground(dpy, gc, px);
  XDrawString(dpy, backbuf, gc, x, y, s, (int)strlen(s));
}

static void drawthumb(Thumb *t, float s, int issel) {
  XTransform xf = {{{XDoubleToFixed(1.0 / s), 0, 0},
                    {0, XDoubleToFixed(1.0 / s), 0},
                    {0, 0, XDoubleToFixed(1.0)}}};

  if (t->pict) {
    XRenderSetPictureFilter(dpy, t->pict, FilterBilinear, NULL, 0);
    XRenderSetPictureTransform(dpy, t->pict, &xf);
    XRenderComposite(dpy, PictOpOver, t->pict, None, backpict, 0, 0, 0, 0,
                     t->dst.x, t->dst.y, t->dst.width, t->dst.height);
  } else { /* no composite pixmap: flat placeholder with the title */
    XSetForeground(dpy, gc, rowpx);
    XFillRectangle(dpy, backbuf, gc, t->dst.x, t->dst.y, t->dst.width,
                   t->dst.height);
    if (t->dst.height > font->ascent + font->descent + 4)
      drawtext(t->dst.x + 4, t->dst.y + (t->dst.height + font->ascent) / 2,
               fgpx, t->title);
  }
  XSetForeground(dpy, gc, issel ? selpx : unselpx);
  XSetLineAttributes(dpy, gc, thumbbw, LineSolid, CapButt, JoinMiter);
  XDrawRectangle(dpy, backbuf, gc, t->dst.x - thumbbw / 2 - 1,
                 t->dst.y - thumbbw / 2 - 1, t->dst.width + thumbbw + 1,
                 t->dst.height + thumbbw + 1);
}

static void render(void) {
  Row *r;
  char label[32];
  const char *title;
  int i, j, ty;

  if (!pausewin) /* forwarded commands may raise windows; stay on top,
                  * but never above a dialog we yielded input to */
    XRaiseWindow(dpy, overlay);
  XSetForeground(dpy, gc, bgpx);
  XFillRectangle(dpy, backbuf, gc, 0, 0, (unsigned int)mw, (unsigned int)mh);
  for (i = 0; i < nrows; i++) {
    r = &rows[i];
    if (r->y + r->h < 0 || r->y > mh)
      continue;
    if (i == selrow) {
      XSetForeground(dpy, gc, rowpx);
      XFillRectangle(dpy, backbuf, gc, pad / 2, r->y - rowgap / 2,
                     (unsigned int)(mw - pad), (unsigned int)(r->h + rowgap));
    }
    snprintf(label, sizeof(label), "%ld%s", r->desk,
             r->desk == curdesk ? "*" : "");
    drawtext(pad, r->y + (r->h + font->ascent) / 2,
             i == selrow ? selpx : unselpx, label);
    for (j = 0; j < nthumbs; j++)
      if (thumbs[j].desk == r->desk)
        drawthumb(&thumbs[j], r->scale, j == selthumb);
  }
  title = selthumb >= 0 ? thumbs[selthumb].title : "(empty workspace)";
  ty = mh - titleh + (titleh + font->ascent) / 2 - font->descent;
  drawtext(pad, ty, fgpx, title);
  XCopyArea(dpy, backbuf, overlay, gc, 0, 0, (unsigned int)mw,
            (unsigned int)mh, 0, 0);
  XSync(dpy, False);
}

static int pickthumb(int x, int y) {
  int i;

  for (i = nthumbs - 1; i >= 0; i--) /* topmost first */
    if (x >= thumbs[i].dst.x && x < thumbs[i].dst.x + thumbs[i].dst.width &&
        y >= thumbs[i].dst.y && y < thumbs[i].dst.y + thumbs[i].dst.height)
      return i;
  return -1;
}

static int rowat(int y) {
  int i;

  for (i = 0; i < nrows; i++)
    if (y >= rows[i].y - rowgap / 2 && y < rows[i].y + rows[i].h + rowgap / 2)
      return i;
  return -1;
}

/* pick the row's member nearest to the previous selection's x */
static void selectrow(int nr) {
  int i, best = -1, bx, d, bd = 0;

  if (nr < 0 || nr >= nrows)
    return;
  selrow = nr;
  bx = selthumb >= 0 ? thumbs[selthumb].dst.x + thumbs[selthumb].dst.width / 2
                     : 0;
  for (i = 0; i < nthumbs; i++) {
    if (thumbs[i].desk != rows[nr].desk)
      continue;
    d = thumbs[i].dst.x + thumbs[i].dst.width / 2 - bx;
    if (d < 0)
      d = -d;
    if (best < 0 || d < bd) {
      best = i;
      bd = d;
    }
  }
  selthumb = best;
  layout();
}

/* step to the spatially adjacent thumbnail in the selected row */
static void stepthumb(int dir) {
  long desk = rows[selrow].desk;
  int i, best = -1, sx, d, bd = 0;

  if (selthumb < 0)
    return;
  sx = thumbs[selthumb].dst.x;
  for (i = 0; i < nthumbs; i++) {
    if (thumbs[i].desk != desk || i == selthumb)
      continue;
    d = dir > 0 ? thumbs[i].dst.x - sx : sx - thumbs[i].dst.x;
    if (d <= 0)
      continue;
    if (best < 0 || d < bd) {
      best = i;
      bd = d;
    }
  }
  if (best >= 0) {
    selthumb = best;
    dirty = 1;
  }
}

static void sendroot(Atom type, Window w, long l0) {
  XEvent ev;

  memset(&ev, 0, sizeof(ev));
  ev.xclient.type = ClientMessage;
  ev.xclient.window = w;
  ev.xclient.message_type = type;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = l0;
  XSendEvent(dpy, root, False,
             SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

static void activate(void) {
  if (selthumb >= 0)
    sendroot(netactivewindow, thumbs[selthumb].win, 2 /* source: pager */);
  else
    sendroot(netcurdesktop, root, rows[selrow].desk);
  XSync(dpy, False);
  running = 0;
}

/* hand a key to the window manager, targeting the selected window: focus it
 * first (via _NET_ACTIVE_WINDOW), then re-inject the key with XTEST while
 * briefly ungrabbed so the WM's passive grab catches it. The held modifiers
 * are the user's real device state, so the combo arrives intact. */
static void forwardkey(XKeyEvent *ev) {
  if (selthumb >= 0 && thumbs[selthumb].win != lastfocused) {
    sendroot(netactivewindow, thumbs[selthumb].win, 2);
    lastfocused = thumbs[selthumb].win;
  }
  XUngrabKeyboard(dpy, CurrentTime);
  /* the user still holds the key down: a fake press of an already-down key
   * counts as autorepeat, and repeats never activate passive grabs, so the
   * WM would miss it. Fake a release first to make the press genuine. */
  XTestFakeKeyEvent(dpy, ev->keycode, False, 0);
  XTestFakeKeyEvent(dpy, ev->keycode, True, 0);
  XTestFakeKeyEvent(dpy, ev->keycode, False, 0);
  XSync(dpy, False);
  grabkb();
}

static void keypress(XKeyEvent *ev) {
  KeySym sym = XkbKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0, 0);

  if (IsModifierKey(sym)) /* never forward these: replaying a modifier
                             press+release would corrupt the held state */
    return;
  /* Tab and Escape are always ours (Mod is often still held from the
   * launch chord); everything else with a command modifier, plus the
   * XF86 media keys, belongs to the window manager */
  if (sym != XK_Tab && sym != XK_Escape &&
      ((ev->state & fwdmods) || (sym >> 8) == 0x1008ff)) {
    forwardkey(ev);
    return;
  }
  if (sym == XK_Escape || sym == XK_q) {
    if (initialactive)
      sendroot(netactivewindow, initialactive, 2); /* undo focus browsing */
    running = 0;
    return;
  }
  switch (sym) {
  case XK_j:
  case XK_Down:
    selectrow(selrow + 1);
    break;
  case XK_k:
  case XK_Up:
    selectrow(selrow - 1);
    break;
  case XK_h:
  case XK_Left:
    stepthumb(-1);
    break;
  case XK_l:
  case XK_Right:
    stepthumb(+1);
    break;
  case XK_Tab:
    if (selthumb >= 0 && thumbs[(selthumb + 1) % MAX(nthumbs, 1)].desk ==
                             rows[selrow].desk) {
      selthumb = (selthumb + 1) % nthumbs;
      selrow = rowof(thumbs[selthumb].desk);
      dirty = 1;
    } else {
      selectrow((selrow + 1) % nrows);
    }
    break;
  case XK_Return:
  case XK_KP_Enter:
  case XK_space:
    activate();
    break;
  default:
    if (sym >= XK_0 && sym <= XK_9) {
      int r = rowof((long)(sym - XK_0));
      if (r >= 0)
        selectrow(r);
    }
    break;
  }
}

static void buttonpress(XButtonEvent *ev) {
  int i, r;

  switch (ev->button) {
  case Button1:
    i = pickthumb(ev->x, ev->y);
    if (i >= 0) {
      selthumb = i;
      selrow = rowof(thumbs[i].desk);
      activate();
    } else if ((r = rowat(ev->y)) >= 0) {
      selectrow(r);
      selthumb = -1;
      activate(); /* bare row: just switch to that workspace */
    }
    break;
  case Button4:
    selectrow(selrow - 1);
    break;
  case Button5:
    selectrow(selrow + 1);
    break;
  }
}

static void handle(XEvent *ev) {
  XDamageNotifyEvent *dev;
  int i;

  if (damagebase >= 0 && ev->type == damagebase + XDamageNotify) {
    dev = (XDamageNotifyEvent *)ev;
    XDamageSubtract(dpy, dev->damage, None, None);
    dirty = 1;
    return;
  }
  switch (ev->type) {
  case Expose:
    if (!ev->xexpose.count)
      dirty = 1;
    break;
  case KeyPress:
    keypress(&ev->xkey);
    break;
  case ButtonPress:
    buttonpress(&ev->xbutton);
    break;
  case MotionNotify:
    i = pickthumb(ev->xmotion.x, ev->xmotion.y);
    if (i >= 0 && i != selthumb) {
      selthumb = i;
      selrow = rowof(thumbs[i].desk);
      dirty = 1;
    }
    break;
  case ConfigureNotify: /* a source window moved or resized */
    i = thumbidx(ev->xconfigure.window);
    if (i >= 0) {
      thumbs[i].x = ev->xconfigure.x;
      thumbs[i].y = ev->xconfigure.y;
      thumbs[i].w = ev->xconfigure.width + 2 * ev->xconfigure.border_width;
      thumbs[i].h = ev->xconfigure.height + 2 * ev->xconfigure.border_width;
      namethumbpict(&thumbs[i]);
      buildrows();
      layout();
    }
    break;
  case MapNotify:
    /* an override-redirect dialog (hmenu, a pinentry, ...) mapping on top
     * of the overlay gets the input: drop our grabs until it goes away */
    if (!pausewin && ev->xmap.override_redirect &&
        ev->xmap.window != overlay && isdialog(ev->xmap.window)) {
      pausewin = ev->xmap.window;
      XUngrabKeyboard(dpy, CurrentTime);
      XUngrabPointer(dpy, CurrentTime);
    }
    break;
  case UnmapNotify:
  case DestroyNotify:
    if (pausewin == (ev->type == UnmapNotify ? ev->xunmap.window
                                             : ev->xdestroywindow.window)) {
      pausewin = None;
      grabinput();
      dirty = 1;
      break;
    }
    i = thumbidx(ev->type == UnmapNotify ? ev->xunmap.window
                                         : ev->xdestroywindow.window);
    if (i >= 0) {
      droptumb(i);
      if (selrow >= nrows)
        selrow = MAX(0, nrows - 1);
      layout();
    }
    break;
  case PropertyNotify:
    if (ev->xproperty.window == root) {
      if (ev->xproperty.atom == netclientlist)
        rebuild(); /* windows opened or closed under us */
      else if (ev->xproperty.atom == netcurdesktop) {
        curdesk = getcard(root, netcurdesktop, 0);
        buildrows();
        layout();
      } else if (ev->xproperty.atom == netactivewindow) {
        lastfocused = None; /* re-read lazily before the next forward */
      }
    } else if (ev->xproperty.atom == netwmdesktop) {
      i = thumbidx(ev->xproperty.window); /* window sent to another ws */
      if (i >= 0) {
        thumbs[i].desk = getcard(thumbs[i].win, netwmdesktop, 0);
        buildrows();
        if (i == selthumb)
          selrow = MAX(0, rowof(thumbs[i].desk));
        if (selrow >= nrows)
          selrow = MAX(0, nrows - 1);
        layout();
      }
    } else if (ev->xproperty.atom == netwmname) {
      i = thumbidx(ev->xproperty.window);
      if (i >= 0) {
        gettitle(thumbs[i].win, thumbs[i].title, sizeof(thumbs[i].title));
        dirty = 1;
      }
    }
    break;
  }
}

/* the launching keybinding usually still holds keys down; retry like dmenu */
static void grabkb(void) {
  struct timespec ts = {0, 10 * 1000000};
  int i;

  for (i = 0; i < 100; i++) {
    if (XGrabKeyboard(dpy, overlay, True, GrabModeAsync, GrabModeAsync,
                      CurrentTime) == GrabSuccess)
      return;
    nanosleep(&ts, NULL);
  }
  die("hws: cannot grab keyboard");
}

static void grabinput(void) {
  grabkb();
  XGrabPointer(dpy, overlay, False, ButtonPressMask | PointerMotionMask,
               GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}

static void pickmonitor(void) {
  XRRMonitorInfo *info;
  Window dw;
  int px = 0, py = 0, di, i, n = 0;
  unsigned int dui;

  mx = my = 0;
  mw = DisplayWidth(dpy, screen);
  mh = DisplayHeight(dpy, screen);
  XQueryPointer(dpy, root, &dw, &dw, &px, &py, &di, &di, &dui);
  info = XRRGetMonitors(dpy, root, True, &n);
  for (i = 0; i < n; i++)
    if (px >= info[i].x && px < info[i].x + info[i].width && py >= info[i].y &&
        py < info[i].y + info[i].height) {
      mx = info[i].x;
      my = info[i].y;
      mw = info[i].width;
      mh = info[i].height;
      break;
    }
  if (info)
    XRRFreeMonitors(info);
}

static void setup(void) {
  XSetWindowAttributes swa;
  Window active;
  Atom real;
  int evbase, errbase, fmt, i, maj = 0, min = 2;
  unsigned long n, extra;
  unsigned char *p = NULL;

  if (!(dpy = XOpenDisplay(NULL)))
    die("hws: cannot open display");
  XSetErrorHandler(xerror);
  screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);
  xfd = ConnectionNumber(dpy);
  netclientlist = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
  netwmdesktop = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
  netcurdesktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
  netactivewindow = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
  netwmname = XInternAtom(dpy, "_NET_WM_NAME", False);
  netwmwindowtype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
  netwmtypedialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
  utf8 = XInternAtom(dpy, "UTF8_STRING", False);
  /* NameWindowPixmap needs Composite >= 0.2 */
  if (XCompositeQueryExtension(dpy, &evbase, &errbase) &&
      XCompositeQueryVersion(dpy, &maj, &min) && (maj > 0 || min >= 2)) {
    XCompositeRedirectSubwindows(dpy, root, CompositeRedirectAutomatic);
    XSync(dpy, False); /* errors (a manual compositor) are swallowed */
    redirected = 1;
  }
  if (!XDamageQueryExtension(dpy, &damagebase, &errbase))
    damagebase = -1;
  bgpx = getcolor(col_bg);
  rowpx = getcolor(col_row);
  selpx = getcolor(col_sel);
  unselpx = getcolor(col_unsel);
  fgpx = getcolor(col_fg);
  font = XLoadQueryFont(dpy, fontname);
  if (!font)
    font = XLoadQueryFont(dpy, "fixed");
  if (!font)
    die("hws: cannot load font");
  titleh = 2 * (font->ascent + font->descent);
  pickmonitor();
  swa.override_redirect = True;
  swa.background_pixel = bgpx;
  swa.event_mask =
      ExposureMask | KeyPressMask | ButtonPressMask | PointerMotionMask;
  overlay = XCreateWindow(dpy, root, mx, my, (unsigned int)mw,
                          (unsigned int)mh, 0, CopyFromParent, CopyFromParent,
                          CopyFromParent,
                          CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
  backbuf = XCreatePixmap(dpy, overlay, (unsigned int)mw, (unsigned int)mh,
                          (unsigned int)DefaultDepth(dpy, screen));
  backpict = XRenderCreatePicture(
      dpy, backbuf, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)),
      0, NULL);
  gc = XCreateGC(dpy, backbuf, 0, NULL);
  XSetFont(dpy, gc, font->fid);
  readmodel();
  selrow = MAX(0, rowof(curdesk));
  selectrow(selrow);
  if (selthumb < 0 && nthumbs) /* current ws empty: still land somewhere */
    selectrow(0);
  active = None;
  if (XGetWindowProperty(dpy, root, netactivewindow, 0L, 1L, False, XA_WINDOW,
                         &real, &fmt, &n, &extra, &p) == Success &&
      p) {
    if (n)
      active = *(Window *)p;
    XFree(p);
  }
  if ((i = thumbidx(active)) >= 0) { /* start on the focused window */
    selthumb = i;
    selrow = rowof(thumbs[i].desk);
  }
  initialactive = active;
  lastfocused = active;
  XSelectInput(dpy, root, PropertyChangeMask | SubstructureNotifyMask);
  XMapRaised(dpy, overlay);
  grabinput();
}

static void run(void) {
  XEvent ev;
  fd_set fds;
  struct timeval tv;
  long now, lastframe = 0, wait;

  layout();
  while (running) {
    while (XPending(dpy)) {
      XNextEvent(dpy, &ev);
      handle(&ev);
      if (!running)
        return;
    }
    now = msnow();
    if (dirty && now - lastframe >= 1000 / fps) {
      render();
      dirty = 0;
      lastframe = now;
    }
    wait = dirty ? MAX(1, 1000 / fps - (now - lastframe)) : 1000;
    FD_ZERO(&fds);
    FD_SET(xfd, &fds);
    tv.tv_sec = wait / 1000;
    tv.tv_usec = (wait % 1000) * 1000;
    select(xfd + 1, &fds, NULL, NULL, &tv);
  }
}

int main(void) {
  int i;

  setup();
  run();
  for (i = 0; i < nthumbs; i++)
    freethumbpict(&thumbs[i]);
  if (redirected)
    XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectAutomatic);
  XCloseDisplay(dpy);
  return 0;
}
