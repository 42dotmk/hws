/* hws configuration. Edit, then rebuild with `make`. */
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
