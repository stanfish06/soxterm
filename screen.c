#define _XOPEN_SOURCE 600

#include "screen.h"
#include "config.h"
#include "pty.h"
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define UTF_SIZ 4
#define ESC_BUF_SIZ (128 * UTF_SIZ)
#define ESC_ARG_SIZ 16
#define ESC_ARG_MAX 65535 // cap for a single CSI parameter
#define TAB_WIDTH 8       // fixed tab stops every N columns
#define FB_FONT_MAX 32    // cached fallback fonts
#define MISS_CP_MAX 64    // codepoints known to have no font on the system

static TermScreen screen;

// scrollback: ring of lines that scrolled off the top of the screen
static Cell *hist[HIST_SIZE];
static char hist_wrap[HIST_SIZE]; // soft-wrap flag per line, as buf_wrap
static int hist_head = 0;         // slot the next line goes into
static int hist_len = 0;          // lines stored, at most HIST_SIZE

// fallback font cache for glyphs missing from screen.fonts[]
static struct {
  XftFont *font;
  int style;
} fb_fonts[FB_FONT_MAX];
static int n_fb_fonts = 0;

static uint32_t missing_cp[MISS_CP_MAX];
static int missing_head = 0;

static Cell default_cell(void) {
  return (Cell){
      .u = ' ',
      .fg = COLOR_DEFAULT_FG,
      .bg = COLOR_DEFAULT_BG,
      .mode = 0,
  };
}

// blank cell using the current SGR background (BCE)
static Cell blank_cell(void) {
  return (Cell){
      .u = ' ',
      .fg = screen.attr.fg,
      .bg = screen.attr.bg,
      .mode = 0,
  };
}

static int is_default_blank(Cell g) {
  return g.u == ' ' && g.bg == COLOR_DEFAULT_BG && g.mode == 0;
}

static void fill_cells(Cell *dst, int n, Cell g) {
  for (int i = 0; i < n; i++)
    dst[i] = g;
}

// store line as the newest history entry; returns a spare line for reuse
static Cell *hist_push(Cell *line, char wrap) {
  Cell *spare = hist[hist_head];
  hist[hist_head] = line;
  hist_wrap[hist_head] = wrap;
  hist_head = (hist_head + 1) % HIST_SIZE;
  if (hist_len < HIST_SIZE)
    hist_len++;
  return spare ? spare : malloc(screen.buf_cols * sizeof(Cell));
}

// line shown at view row; rows above the live screen come from history
static Cell *view_line(int row) {
  if (row < screen.scroll_off)
    return hist[(hist_head - screen.scroll_off + row + HIST_SIZE) % HIST_SIZE];
  return screen.buf[row - screen.scroll_off];
}

// scroll the view n rows back into history (negative goes forward)
void screen_scroll(int n) {
  screen.scroll_off = MIN(MAX(screen.scroll_off + n, 0), hist_len);
}

unsigned int screen_default_bg(void) { return defaultbg; }

void update_screen_pty(int pty_fd) {
  if (pty_fd > 0)
    screen.pty_fd = pty_fd;
}

// convert an indexed, default, or 24-bit color value to 0x00RRGGBB
static uint32_t color_to_rgb(uint32_t c) {
  if (c & COLOR_TRUE_BIT)
    return c & 0xffffff;
  if (c < 16)
    return colorname[c];
  if (c < 232) {
    // 6x6x6 cube
    static const uint8_t v[6] = {0, 95, 135, 175, 215, 255};
    uint32_t idx = c - 16;
    return ((uint32_t)v[idx / 36] << 16) | ((uint32_t)v[(idx / 6) % 6] << 8) |
           v[idx % 6];
  }
  if (c < 256) {
    // 24-step grayscale ramp
    uint32_t g = 8 + (c - 232) * 10;
    return (g << 16) | (g << 8) | g;
  }
  return (c == COLOR_DEFAULT_FG) ? defaultfg : defaultbg;
}

static uint32_t dim_rgb(uint32_t rgb) {
  uint32_t r = ((rgb >> 16) & 0xff) * 2 / 3;
  uint32_t g = ((rgb >> 8) & 0xff) * 2 / 3;
  uint32_t b = (rgb & 0xff) * 2 / 3;
  return (r << 16) | (g << 8) | b;
}

// allocate an XftColor from a 0x00RRGGBB value
static void alloc_color(uint32_t rgb, XftColor *out) {
  int scr = DefaultScreen(screen.display);
  // XRenderColor channels are 16-bit; 0xab -> 0xabab
  XRenderColor c = {
      .red = ((rgb >> 16) & 0xff) * 0x101,
      .green = ((rgb >> 8) & 0xff) * 0x101,
      .blue = (rgb & 0xff) * 0x101,
      .alpha = 0xffff,
  };
  if (!XftColorAllocValue(screen.display, DefaultVisual(screen.display, scr),
                          DefaultColormap(screen.display, scr), &c, out)) {
    fprintf(stderr, "Error: cannot allocate color 0x%06x\n", rgb);
    exit(1);
  }
}

static void free_color(XftColor *c) {
  int scr = DefaultScreen(screen.display);
  XftColorFree(screen.display, DefaultVisual(screen.display, scr),
               DefaultColormap(screen.display, scr), c);
}

static int font_style(uint16_t mode) {
  return ((mode & ATTR_BOLD) ? 1 : 0) | ((mode & ATTR_ITALIC) ? 2 : 0);
}

// open font with optional weight/slant overrides (-1 keeps pattern default)
static XftFont *open_font_variant(Display *dpy, int scr, const char *name,
                                  int weight, int slant) {
  FcPattern *pat = FcNameParse((const FcChar8 *)name);
  if (!pat)
    return NULL;
  if (weight >= 0) {
    FcPatternDel(pat, FC_WEIGHT);
    FcPatternAddInteger(pat, FC_WEIGHT, weight);
  }
  if (slant >= 0) {
    FcPatternDel(pat, FC_SLANT);
    FcPatternAddInteger(pat, FC_SLANT, slant);
  }
  FcResult res;
  FcPattern *match = XftFontMatch(dpy, scr, pat, &res);
  FcPatternDestroy(pat);
  if (!match)
    return NULL;
  XftFont *f = XftFontOpenPattern(dpy, match);
  if (!f)
    FcPatternDestroy(match);
  return f;
}

// return a font covering cp: primary -> cached fallback -> fontconfig match
static XftFont *find_font(uint32_t cp, int style) {
  Display *dpy = screen.display;
  XftFont *primary = screen.fonts[style];
  if (XftCharExists(dpy, primary, cp))
    return primary;

  for (int i = 0; i < n_fb_fonts; i++) {
    if (fb_fonts[i].style == style && XftCharExists(dpy, fb_fonts[i].font, cp))
      return fb_fonts[i].font;
  }
  for (int i = 0; i < MISS_CP_MAX; i++) {
    if (missing_cp[i] == cp)
      return primary;
  }

  // query fontconfig for a font at the same pixel size that has cp
  int scr = DefaultScreen(dpy);
  FcPattern *pat = FcPatternCreate();
  FcCharSet *cs = FcCharSetCreate();
  FcCharSetAddChar(cs, cp);
  FcPatternAddCharSet(pat, FC_CHARSET, cs);
  FcPatternAddDouble(pat, FC_PIXEL_SIZE, screen.font_px);
  if (style & 1)
    FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
  if (style & 2)
    FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);

  FcResult res;
  FcPattern *match = XftFontMatch(dpy, scr, pat, &res);
  FcCharSetDestroy(cs);
  FcPatternDestroy(pat);

  XftFont *f = match ? XftFontOpenPattern(dpy, match) : NULL;
  if (match && !f)
    FcPatternDestroy(match);

  if (f && XftCharExists(dpy, f, cp)) {
    if (n_fb_fonts < FB_FONT_MAX) {
      fb_fonts[n_fb_fonts].font = f;
      fb_fonts[n_fb_fonts].style = style;
      n_fb_fonts++;
      return f;
    }
    XftFontClose(dpy, f);
    return primary;
  }
  if (f)
    XftFontClose(dpy, f);
  // remember misses so repeated draws don't re-query fontconfig
  missing_cp[missing_head] = cp;
  missing_head = (missing_head + 1) % MISS_CP_MAX;
  return primary;
}

void screen_init(Display *display, Window window, TextCursor cursor, GC gc) {
  screen.display = display;
  screen.window = window;
  screen.gc = gc;

  int scr = DefaultScreen(display);
  screen.draw = XftDrawCreate(display, window, DefaultVisual(display, scr),
                              DefaultColormap(display, scr));

  screen.fonts[0] = open_font_variant(display, scr, font, -1, -1);
  if (!screen.fonts[0]) {
    fprintf(stderr, "Error: cannot load font '%s'\n", font);
    exit(1);
  }
  screen.fonts[1] = open_font_variant(display, scr, font, FC_WEIGHT_BOLD, -1);
  screen.fonts[2] = open_font_variant(display, scr, font, -1, FC_SLANT_ITALIC);
  screen.fonts[3] =
      open_font_variant(display, scr, font, FC_WEIGHT_BOLD, FC_SLANT_ITALIC);
  for (int i = 1; i < 4; i++) {
    if (!screen.fonts[i])
      screen.fonts[i] = screen.fonts[0];
  }
  if (FcPatternGetDouble(screen.fonts[0]->pattern, FC_PIXEL_SIZE, 0,
                         &screen.font_px) != FcResultMatch)
    screen.font_px = screen.fonts[0]->height;

  // preallocate indexed + default colors so common draws do zero allocs
  for (int i = 0; i < COLOR_COUNT; i++)
    alloc_color(color_to_rgb(i), &screen.colors[i]);
  alloc_color(defaultcs, &screen.cursor_bg);
  alloc_color(defaultrcs, &screen.cursor_fg);
  screen.attr = default_cell();

  // cell width from one glyph's advance; max_advance_width can be wider than
  // the real cell for fonts with a few oversized glyphs
  XGlyphInfo ext;
  XftTextExtents8(display, screen.fonts[0], (const FcChar8 *)"M", 1, &ext);
  screen.char_width = ext.xOff;
  screen.char_height = screen.fonts[0]->ascent + screen.fonts[0]->descent;

  screen.cursor = cursor;
  screen.view_rows = SCREEN_HEIGHT / screen.char_height;
  screen.view_cols = SCREEN_WIDTH / screen.char_width;
  screen.buf_rows = screen.view_rows;
  screen.buf_cols = screen.view_cols;
  screen.scroll_top = 0;
  screen.scroll_bot = screen.view_rows - 1;

  screen.buf = malloc(screen.buf_rows * sizeof(Cell *));
  for (int i = 0; i < screen.buf_rows; ++i) {
    screen.buf[i] = malloc(screen.buf_cols * sizeof(Cell));
    fill_cells(screen.buf[i], screen.buf_cols, default_cell());
  }
  screen.buf_wrap = calloc(screen.buf_rows, sizeof(char));
}

// draw a run of glyphs sharing attributes and font, covering ncells columns
static void draw_run(int row, int col, int ncells, const FcChar32 *run,
                     int run_len, XftFont *f, Cell g) {
  uint32_t fg_id = g.fg;
  uint32_t bg_id = g.bg;
  // bold brightens standard ANSI colors 0-7
  if ((g.mode & ATTR_BOLD) && fg_id < 8)
    fg_id += 8;
  if (g.mode & ATTR_REVERSE) {
    uint32_t tmp = fg_id;
    fg_id = bg_id;
    bg_id = tmp;
  }

  XftColor tmp_bg, tmp_fg;
  int alloc_bg = 0, alloc_fg = 0;
  unsigned long bg_pixel;
  if (bg_id < COLOR_COUNT) {
    bg_pixel = screen.colors[bg_id].pixel;
  } else {
    alloc_color(color_to_rgb(bg_id), &tmp_bg);
    bg_pixel = tmp_bg.pixel;
    alloc_bg = 1;
  }

  XftColor *fg_col;
  if (fg_id < COLOR_COUNT && !(g.mode & ATTR_FAINT)) {
    fg_col = &screen.colors[fg_id];
  } else {
    uint32_t rgb = color_to_rgb(fg_id);
    if (g.mode & ATTR_FAINT)
      rgb = dim_rgb(rgb);
    alloc_color(rgb, &tmp_fg);
    fg_col = &tmp_fg;
    alloc_fg = 1;
  }

  int x = col * screen.char_width;
  int y_top = row * screen.char_height;
  int w = ncells * screen.char_width;
  int baseline = y_top + screen.fonts[0]->ascent;

  XSetForeground(screen.display, screen.gc, bg_pixel);
  XFillRectangle(screen.display, screen.window, screen.gc, x, y_top, w,
                 screen.char_height);

  if (!(g.mode & ATTR_INVISIBLE)) {
    XftDrawString32(screen.draw, fg_col, f, x, baseline, run, run_len);
    if (g.mode & ATTR_UNDERLINE) {
      int uy = MIN(baseline + 1, y_top + screen.char_height - 1);
      XSetForeground(screen.display, screen.gc, fg_col->pixel);
      XFillRectangle(screen.display, screen.window, screen.gc, x, uy, w, 1);
    }
    if (g.mode & ATTR_STRUCK) {
      int sy = y_top + screen.char_height / 2;
      XSetForeground(screen.display, screen.gc, fg_col->pixel);
      XFillRectangle(screen.display, screen.window, screen.gc, x, sy, w, 1);
    }
  }

  if (alloc_fg)
    free_color(&tmp_fg);
  if (alloc_bg)
    free_color(&tmp_bg);
}

static void screen_draw_row(int row) {
  Cell *line = view_line(row);
  int cols = screen.view_cols;
  FcChar32 run[cols];

  for (int col = 0; col < cols;) {
    Cell g = line[col];
    if (g.mode & ATTR_WDUMMY) {
      col++;
      continue;
    }
    int style = font_style(g.mode);
    XftFont *primary = screen.fonts[style];
    XftFont *f = find_font(g.u, style);
    int ncells = (g.mode & ATTR_WIDE) ? 2 : 1;
    int run_len = 0;
    run[run_len++] = g.u;

    // batch adjacent single-cell primary-font glyphs with identical style
    int next = col + ncells;
    if (f == primary && ncells == 1) {
      while (next < cols) {
        Cell ng = line[next];
        if (ng.fg != g.fg || ng.bg != g.bg || ng.mode != g.mode)
          break;
        if (!XftCharExists(screen.display, primary, ng.u))
          break;
        run[run_len++] = ng.u;
        ncells++;
        next++;
      }
    }
    draw_run(row, col, ncells, run, run_len, f, g);
    col = next;
  }
}

void screen_draw_all_text(void) {
  for (int r = 0; r < screen.view_rows; r++)
    screen_draw_row(r);
}

void screen_draw_block_cursor(void) {
  int row = screen.cursor.row;
  int col = screen.cursor.col;
  // hide the cursor once scrolling pushes it below the view
  if (!screen.cursor.visible || row + screen.scroll_off >= screen.view_rows)
    return;
  Cell g = screen.buf[row][col];
  if ((g.mode & ATTR_WDUMMY) && col > 0) {
    col--;
    g = screen.buf[row][col];
  }
  int ncells = (g.mode & ATTR_WIDE) ? 2 : 1;
  int x = col * screen.char_width;
  int y_top = (row + screen.scroll_off) * screen.char_height;
  int baseline = y_top + screen.fonts[0]->ascent;

  XSetForeground(screen.display, screen.gc, screen.cursor_bg.pixel);
  XFillRectangle(screen.display, screen.window, screen.gc, x, y_top,
                 ncells * screen.char_width, screen.char_height);
  if (g.u && !(g.mode & ATTR_INVISIBLE)) {
    FcChar32 cp = g.u;
    XftFont *f = find_font(g.u, font_style(g.mode));
    XftDrawString32(screen.draw, &screen.cursor_fg, f, x, baseline, &cp, 1);
  }
}

typedef struct {
  char buf[ESC_BUF_SIZ]; // own storage so a sequence can span pty reads
  size_t len;
  char priv; // private marker ('?', '>', '<', '=') or 0
  int arg[ESC_ARG_SIZ];
  char sep[ESC_ARG_SIZ]; // separator after arg[i]: ';', ':', or 0
  int narg;
  char mode[2];
} CSIEscape;
static CSIEscape csiescape;

typedef enum {
  ESC_NONE,
  ESC_START,
  ESC_INTER, // ESC + intermediate (e.g. ESC ( B), waiting for the final byte
  ESC_CSI,
  ESC_OSC,
  ESC_OSC_ESC,
} EscState;
static EscState esc_state = ESC_NONE;

// incremental UTF-8 decoder state across pty reads
static uint32_t utf8_cp = 0;
static int utf8_rem = 0;
static uint32_t utf8_min = 0;

// set after printing in the last column; the next printable char wraps first
static int wrap_next = 0;
// cursor and pen saved by ESC 7, restored by ESC 8
static TextCursor saved_cursor = {0, 0, 1};
static Cell saved_attr = {' ', COLOR_DEFAULT_FG, COLOR_DEFAULT_BG, 0};
// DECCKM (CSI ?1h/l): cursor keys send SS3 (ESC O x) instead of CSI (ESC [ x)
static int app_cursor_keys = 0;

int screen_app_cursor_keys(void) { return app_cursor_keys; }

// if (row, col) is half of a wide glyph, blank its other half too
static void clear_wide_cell(int row, int col) {
  Cell *line = screen.buf[row];
  if ((line[col].mode & ATTR_WDUMMY) && col > 0)
    line[col - 1] = blank_cell();
  else if ((line[col].mode & ATTR_WIDE) && col + 1 < screen.view_cols)
    line[col + 1] = blank_cell();
}

// blank columns [from, to) on a row
static void erase_cells(int row, int from, int to) {
  if (from < 0)
    from = 0;
  if (to > screen.view_cols)
    to = screen.view_cols;
  if (from < to) {
    clear_wide_cell(row, from);
    clear_wide_cell(row, to - 1);
    fill_cells(&screen.buf[row][from], to - from, blank_cell());
  }
  // erasing to the end of the row ends the line there
  if (to == screen.view_cols)
    screen.buf_wrap[row] = 0;
}

// move the cursor, clamped to the visible screen; cancels a pending wrap
static void move_to(TextCursor *cursor, int row, int col) {
  if (row < 0)
    row = 0;
  else if (row > screen.view_rows - 1)
    row = screen.view_rows - 1;
  if (col < 0)
    col = 0;
  else if (col > screen.view_cols - 1)
    col = screen.view_cols - 1;
  cursor->row = row;
  cursor->col = col;
  wrap_next = 0;
}

// rotate rows [top, bot] up by n; the n rows entering at bot are blanked
static void scroll_up(int top, int bot, int n) {
  if (n > bot - top + 1)
    n = bot - top + 1;
  for (int k = 0; k < n; k++) {
    Cell *line = screen.buf[top];
    memmove(&screen.buf[top], &screen.buf[top + 1],
            (bot - top) * sizeof(Cell *));
    memmove(&screen.buf_wrap[top], &screen.buf_wrap[top + 1], bot - top);
    screen.buf[bot] = line;
    screen.buf_wrap[bot] = 0;
    fill_cells(line, screen.buf_cols, blank_cell());
  }
}

// rotate rows [top, bot] down by n; the n rows entering at top are blanked
static void scroll_down(int top, int bot, int n) {
  if (n > bot - top + 1)
    n = bot - top + 1;
  for (int k = 0; k < n; k++) {
    Cell *line = screen.buf[bot];
    memmove(&screen.buf[top + 1], &screen.buf[top],
            (bot - top) * sizeof(Cell *));
    memmove(&screen.buf_wrap[top + 1], &screen.buf_wrap[top], bot - top);
    screen.buf[top] = line;
    screen.buf_wrap[top] = 0;
    fill_cells(line, screen.buf_cols, blank_cell());
  }
}

// LF/IND: down one row, scrolling the region when on its bottom margin
static void linefeed(TextCursor *cursor) {
  if (cursor->row == screen.scroll_bot) {
    // the top line leaves the screen only when the region starts at row 0;
    // keep it in history and rotate a spare line in to be blanked
    if (screen.scroll_top == 0)
      screen.buf[0] = hist_push(screen.buf[0], screen.buf_wrap[0]);
    scroll_up(screen.scroll_top, screen.scroll_bot, 1);
  } else
    move_to(cursor, cursor->row + 1, cursor->col);
  wrap_next = 0;
}

// RI: up one row, scrolling the region down when on its top margin
static void reverse_index(TextCursor *cursor) {
  if (cursor->row == screen.scroll_top)
    scroll_down(screen.scroll_top, screen.scroll_bot, 1);
  else
    move_to(cursor, cursor->row - 1, cursor->col);
  wrap_next = 0;
}

// ICH: shift the rest of the line right by n and blank the gap
static void insert_char_at_cursor(TextCursor *cursor, int n) {
  int cols = screen.view_cols;
  if (n > cols - cursor->col)
    n = cols - cursor->col;
  int src = cursor->col;
  int dst = src + n;
  clear_wide_cell(cursor->row, src);
  Cell *line = screen.buf[cursor->row];
  memmove(&line[dst], &line[src], (cols - dst) * sizeof(Cell));
  // a wide glyph pushed past the right edge loses its trailing cell
  if (line[cols - 1].mode & ATTR_WIDE)
    line[cols - 1] = blank_cell();
  erase_cells(cursor->row, src, dst);
}

// DCH: shift the rest of the line left by n and blank the end
static void delete_char_at_cursor(TextCursor *cursor, int n) {
  int cols = screen.view_cols;
  if (n > cols - cursor->col)
    n = cols - cursor->col;
  int dst = cursor->col;
  int src = dst + n;
  clear_wide_cell(cursor->row, dst);
  if (src > 0)
    clear_wide_cell(cursor->row, src - 1);
  Cell *line = screen.buf[cursor->row];
  memmove(&line[dst], &line[src], (cols - src) * sizeof(Cell));
  erase_cells(cursor->row, cols - n, cols);
}

static void parse_csi_escape(void) {
  char *p = csiescape.buf, *np;
  long int v;
  csiescape.narg = 0;
  // 0x3c-0x3f as the first byte marks a private sequence
  if (*p >= '<' && *p <= '?') {
    csiescape.priv = *p;
    p++;
  }
  csiescape.buf[csiescape.len] = '\0';
  while (p < csiescape.buf + csiescape.len) {
    np = NULL;
    v = strtol(p, &np, 10);
    if (np == p)
      v = 0;
    // clamp so handlers can add args to row/col without overflow
    if (v < 0 || v > ESC_ARG_MAX)
      v = ESC_ARG_MAX;
    csiescape.arg[csiescape.narg] = v;
    p = np;
    if ((*p != ';' && *p != ':') || csiescape.narg == ESC_ARG_SIZ - 1) {
      csiescape.sep[csiescape.narg++] = '\0';
      break;
    }
    csiescape.sep[csiescape.narg++] = *p++;
  }
  csiescape.mode[0] = *p++;
  csiescape.mode[1] = (p < csiescape.buf + csiescape.len) ? *p : '\0';
}

// parse extended color after 38/48: 5;n (256-color) or 2;r;g;b / 2::r:g:b (RGB)
// returns the extra args consumed, or 0 on invalid sequence
static int parse_ext_color(const int *arg, const char *sep, int narg, int i,
                           uint32_t *out) {
  if (i + 1 >= narg)
    return 0;
  int kind = arg[i + 1];
  if (kind == 5 && i + 2 < narg) {
    *out = MIN(arg[i + 2], 255);
    return 2;
  }
  if (kind == 2) {
    // ISO 8613-6 colon form may include an extra colorspace arg: 38:2::r:g:b
    int off = (sep[i] == ':' && i + 5 < narg && sep[i + 4] == ':') ? 3 : 2;
    if (i + off + 2 < narg) {
      int r = MIN(arg[i + off], 255);
      int g = MIN(arg[i + off + 1], 255);
      int b = MIN(arg[i + off + 2], 255);
      *out = COLOR_TRUE(r, g, b);
      return off + 2;
    }
  }
  return 0;
}

// SGR (CSI ... m): update screen.attr
static void handle_sgr(const int *arg, const char *sep, int narg) {
  if (narg == 0) {
    screen.attr = default_cell();
    return;
  }
  for (int i = 0; i < narg; i++) {
    int a = arg[i];
    switch (a) {
    case 0:
      screen.attr = default_cell();
      break;
    case 1:
      screen.attr.mode |= ATTR_BOLD;
      break;
    case 2:
      screen.attr.mode |= ATTR_FAINT;
      break;
    case 3:
      screen.attr.mode |= ATTR_ITALIC;
      break;
    case 4:
      screen.attr.mode |= ATTR_UNDERLINE;
      break;
    case 7:
      screen.attr.mode |= ATTR_REVERSE;
      break;
    case 8:
      screen.attr.mode |= ATTR_INVISIBLE;
      break;
    case 9:
      screen.attr.mode |= ATTR_STRUCK;
      break;
    case 22:
      screen.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
      break;
    case 23:
      screen.attr.mode &= ~ATTR_ITALIC;
      break;
    case 24:
      screen.attr.mode &= ~ATTR_UNDERLINE;
      break;
    case 27:
      screen.attr.mode &= ~ATTR_REVERSE;
      break;
    case 28:
      screen.attr.mode &= ~ATTR_INVISIBLE;
      break;
    case 29:
      screen.attr.mode &= ~ATTR_STRUCK;
      break;
    case 38:
      i += parse_ext_color(arg, sep, narg, i, &screen.attr.fg);
      break;
    case 39:
      screen.attr.fg = COLOR_DEFAULT_FG;
      break;
    case 48:
      i += parse_ext_color(arg, sep, narg, i, &screen.attr.bg);
      break;
    case 49:
      screen.attr.bg = COLOR_DEFAULT_BG;
      break;
    default:
      if (a >= 30 && a <= 37)
        screen.attr.fg = a - 30;
      else if (a >= 40 && a <= 47)
        screen.attr.bg = a - 40;
      else if (a >= 90 && a <= 97)
        screen.attr.fg = a - 90 + 8;
      else if (a >= 100 && a <= 107)
        screen.attr.bg = a - 100 + 8;
      break;
    }
  }
}

static void handle_csi_escape(TextCursor *cursor) {
  int *arg = csiescape.arg;
  int rows = screen.view_rows;
  int cols = screen.view_cols;
  int row = cursor->row;
  int col = cursor->col;

  // private sequences: only DEC modes via ESC[?Nh / ESC[?Nl are handled
  if (csiescape.priv) {
    char m = csiescape.mode[0];
    if (csiescape.priv == '?' && (m == 'h' || m == 'l')) {
      for (int i = 0; i < csiescape.narg; i++) {
        if (arg[i] == 1) // DECCKM
          app_cursor_keys = (m == 'h');
        // other DEC modes (?25 cursor visibility, ?1049 alt screen, ...) are
        // ignored for now
      }
    }
    return;
  }
  // every command except SGR cancels a pending wrap
  if (csiescape.mode[0] != 'm')
    wrap_next = 0;

  switch (csiescape.mode[0]) {
  case '@': // ICH: insert n blanks
    DEFAULT(arg[0], 1);
    insert_char_at_cursor(cursor, arg[0]);
    break;
  case 'A': // CUU: up n
    DEFAULT(arg[0], 1);
    move_to(cursor, row - arg[0], col);
    break;
  case 'B': // CUD: down n
  case 'e': // VPR
    DEFAULT(arg[0], 1);
    move_to(cursor, row + arg[0], col);
    break;
  case 'C': // CUF: right n
  case 'a': // HPR
    DEFAULT(arg[0], 1);
    move_to(cursor, row, col + arg[0]);
    break;
  case 'D': // CUB: left n
    DEFAULT(arg[0], 1);
    move_to(cursor, row, col - arg[0]);
    break;
  case 'E': // CNL: down n, column 1
    DEFAULT(arg[0], 1);
    move_to(cursor, row + arg[0], 0);
    break;
  case 'F': // CPL: up n, column 1
    DEFAULT(arg[0], 1);
    move_to(cursor, row - arg[0], 0);
    break;
  case 'G': // CHA: column n
  case '`': // HPA
    DEFAULT(arg[0], 1);
    move_to(cursor, row, arg[0] - 1);
    break;
  case 'd': // VPA: row n
    DEFAULT(arg[0], 1);
    move_to(cursor, arg[0] - 1, col);
    break;
  case 'H': // CUP: row;col
  case 'f': // HVP
    DEFAULT(arg[0], 1);
    DEFAULT(arg[1], 1);
    move_to(cursor, arg[0] - 1, arg[1] - 1);
    break;
  case 'J': // ED: 0 cursor to end, 1 start to cursor, 2/3 whole screen
    switch (arg[0]) {
    case 0:
      erase_cells(row, col, cols);
      for (int r = row + 1; r < rows; r++)
        erase_cells(r, 0, cols);
      break;
    case 1:
      for (int r = 0; r < row; r++)
        erase_cells(r, 0, cols);
      erase_cells(row, 0, col + 1);
      break;
    case 3: // 3 also clears scrollback
      hist_len = 0;
      screen.scroll_off = 0;
      // fall through
    case 2:
      for (int r = 0; r < rows; r++)
        erase_cells(r, 0, cols);
      break;
    }
    break;
  case 'K': // EL: 0 cursor to end, 1 start to cursor, 2 whole line
    switch (arg[0]) {
    case 0:
      erase_cells(row, col, cols);
      break;
    case 1:
      erase_cells(row, 0, col + 1);
      break;
    case 2:
      erase_cells(row, 0, cols);
      break;
    }
    break;
  case 'X': // ECH: blank n chars from the cursor, no shift
    DEFAULT(arg[0], 1);
    erase_cells(row, col, col + arg[0]);
    break;
  case 'P': // DCH: delete n chars
    DEFAULT(arg[0], 1);
    delete_char_at_cursor(cursor, arg[0]);
    break;
  case 'L': // IL: insert n lines at the cursor, only inside the scroll region
    DEFAULT(arg[0], 1);
    if (row >= screen.scroll_top && row <= screen.scroll_bot) {
      scroll_down(row, screen.scroll_bot, arg[0]);
      move_to(cursor, row, 0);
    }
    break;
  case 'M': // DL: delete n lines at the cursor, only inside the scroll region
    DEFAULT(arg[0], 1);
    if (row >= screen.scroll_top && row <= screen.scroll_bot) {
      scroll_up(row, screen.scroll_bot, arg[0]);
      move_to(cursor, row, 0);
    }
    break;
  case 'S': // SU: scroll region up n
    DEFAULT(arg[0], 1);
    scroll_up(screen.scroll_top, screen.scroll_bot, arg[0]);
    break;
  case 'T': // SD: scroll region down n
    DEFAULT(arg[0], 1);
    scroll_down(screen.scroll_top, screen.scroll_bot, arg[0]);
    break;
  case 'm': // SGR: colors and text attributes
    handle_sgr(arg, csiescape.sep, csiescape.narg);
    break;
  case 'r': { // DECSTBM: scroll region top;bottom, then home the cursor
    DEFAULT(arg[0], 1);
    DEFAULT(arg[1], rows);
    int top = arg[0] - 1;
    int bot = (arg[1] > rows ? rows : arg[1]) - 1;
    // a region needs at least two rows; otherwise ignore the request
    if (top < bot) {
      screen.scroll_top = top;
      screen.scroll_bot = bot;
      move_to(cursor, 0, 0);
    }
    break;
  }
  default:
    break;
  }
}

// two-byte escapes: ESC <final>
static void handle_esc(TextCursor *cursor, unsigned char c) {
  switch (c) {
  case 'D': // IND: line feed
    linefeed(cursor);
    break;
  case 'E': // NEL: carriage return + line feed
    move_to(cursor, cursor->row, 0);
    linefeed(cursor);
    break;
  case 'M': // RI: reverse line feed
    reverse_index(cursor);
    break;
  case '7': // DECSC: save cursor and pen
    saved_cursor = *cursor;
    saved_attr = screen.attr;
    break;
  case '8': // DECRC: restore cursor and pen
    move_to(cursor, saved_cursor.row, saved_cursor.col);
    screen.attr = saved_attr;
    break;
  default: // ESC =, ESC >, ... ignored
    break;
  }
}

// C0 controls; these run immediately, even inside an escape sequence
static void handle_control(TextCursor *cursor, unsigned char c) {
  utf8_rem = 0;
  switch (c) {
  case 0x1b: // ESC (re)starts a sequence, dropping any partial one
    esc_state = ESC_START;
    break;
  case 0x18: // CAN
  case 0x1a: // SUB
    esc_state = ESC_NONE;
    break;
  case '\r':
    move_to(cursor, cursor->row, 0);
    break;
  case '\n':
  case '\v':
  case '\f':
    linefeed(cursor);
    break;
  case '\b': // non-destructive: move left only
    move_to(cursor, cursor->row, cursor->col - 1);
    break;
  case '\t':
    move_to(cursor, cursor->row, (cursor->col / TAB_WIDTH + 1) * TAB_WIDTH);
    break;
  default: // BEL, SO/SI, DEL, ... ignored
    break;
  }
}

// write a Unicode codepoint at the cursor, handling wrapping and 2-cell glyphs
static void put_codepoint(TextCursor *cursor, uint32_t cp) {
  int w = wcwidth((wchar_t)cp);
  if (w <= 0)
    return;
  if (w == 2 && screen.view_cols < 2)
    w = 1;

  // wrap when previous write hit the edge or a wide char doesn't fit at end
  if (wrap_next || (w == 2 && cursor->col == screen.view_cols - 1)) {
    if (!wrap_next && w == 2)
      erase_cells(cursor->row, cursor->col, cursor->col + 1);
    screen.buf_wrap[cursor->row] = 1;
    move_to(cursor, cursor->row, 0);
    linefeed(cursor);
  }

  int row = cursor->row;
  int col = cursor->col;
  clear_wide_cell(row, col);
  Cell g = screen.attr;
  g.u = cp;
  if (w == 2) {
    clear_wide_cell(row, col + 1);
    g.mode |= ATTR_WIDE;
    screen.buf[row][col] = g;
    screen.buf[row][col + 1] = (Cell){
        .u = 0,
        .fg = g.fg,
        .bg = g.bg,
        .mode = ATTR_WDUMMY,
    };
  } else {
    screen.buf[row][col] = g;
  }

  if (col + w < screen.view_cols)
    cursor->col = col + w;
  else {
    cursor->col = screen.view_cols - 1;
    wrap_next = 1;
  }
}

// feed one non-control byte into the UTF-8 decoder and emit complete codepoints
static void feed_utf8_byte(TextCursor *cursor, unsigned char c) {
  if (utf8_rem > 0) {
    if ((c & 0xc0) == 0x80) {
      utf8_cp = (utf8_cp << 6) | (c & 0x3f);
      if (--utf8_rem == 0) {
        if (utf8_cp < utf8_min || utf8_cp > 0x10ffff ||
            (utf8_cp >= 0xd800 && utf8_cp <= 0xdfff))
          utf8_cp = 0xfffd;
        put_codepoint(cursor, utf8_cp);
      }
      return;
    }
    // interrupted multi-byte sequence: emit U+FFFD and re-handle c below
    utf8_rem = 0;
    put_codepoint(cursor, 0xfffd);
  }

  if (c < 0x80) {
    put_codepoint(cursor, c);
  } else if ((c & 0xe0) == 0xc0) {
    utf8_cp = c & 0x1f;
    utf8_rem = 1;
    utf8_min = 0x80;
  } else if ((c & 0xf0) == 0xe0) {
    utf8_cp = c & 0x0f;
    utf8_rem = 2;
    utf8_min = 0x800;
  } else if ((c & 0xf8) == 0xf0) {
    utf8_cp = c & 0x07;
    utf8_rem = 3;
    utf8_min = 0x10000;
  } else {
    put_codepoint(cursor, 0xfffd);
  }
}

void screen_feed(const char *buf, int len) {
  TextCursor *cursor = &screen.cursor;
  if (cursor == NULL || buf == NULL || len <= 0)
    return;
  // new output returns the view to the live screen
  screen.scroll_off = 0;
  for (int i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)buf[i];
    // OSC strings swallow everything up to BEL or ESC '\'
    if (esc_state == ESC_OSC) {
      if (c == 0x07) {
        esc_state = ESC_NONE;
      } else if (c == 0x1b) {
        esc_state = ESC_OSC_ESC;
      }
      continue;
    }
    if (esc_state == ESC_OSC_ESC) {
      esc_state = ESC_NONE;
      continue;
    }
    if (c < 0x20 || c == 0x7f) {
      handle_control(cursor, c);
      continue;
    }
    switch (esc_state) {
    case ESC_START:
      if (c == '[') {
        // reset args/priv left over from the previous sequence
        memset(&csiescape, 0, sizeof(csiescape));
        esc_state = ESC_CSI;
      } else if (c == ']') {
        esc_state = ESC_OSC;
      } else if (c >= 0x20 && c <= 0x2f) {
        esc_state = ESC_INTER;
      } else {
        handle_esc(cursor, c);
        esc_state = ESC_NONE;
      }
      break;
    case ESC_INTER:
      // final byte ends it; charset selection (ESC ( B) etc. is ignored
      if (c >= 0x30 && c <= 0x7e)
        esc_state = ESC_NONE;
      break;
    case ESC_CSI:
      // accumulate params; keep one byte for the '\0' parse_csi_escape adds
      if (csiescape.len < ESC_BUF_SIZ - 1)
        csiescape.buf[csiescape.len++] = c;
      // final byte: parse and dispatch once per sequence
      if (c >= 0x40 && c <= 0x7e) {
        parse_csi_escape();
        handle_csi_escape(cursor);
        esc_state = ESC_NONE;
      }
      break;
    default:
      feed_utf8_byte(cursor, c);
      break;
    }
  }
}

void screen_view_size(int *rows, int *cols) {
  *rows = screen.view_rows;
  *cols = screen.view_cols;
}

// row i of history followed by the screen, oldest first
static Cell *src_row(int i, char *wrap) {
  if (i < hist_len) {
    int k = (hist_head - hist_len + i + HIST_SIZE) % HIST_SIZE;
    *wrap = hist_wrap[k];
    return hist[k];
  }
  *wrap = screen.buf_wrap[i - hist_len];
  return screen.buf[i - hist_len];
}

// screen row holds only default blanks and doesn't continue on the next row
static int row_blank(int r) {
  if (screen.buf_wrap[r])
    return 0;
  for (int i = 0; i < screen.view_cols; i++)
    if (!is_default_blank(screen.buf[r][i]))
      return 0;
  return 1;
}

static Cell *alloc_blank_row(int cols) {
  Cell *line = malloc(cols * sizeof(Cell));
  fill_cells(line, cols, default_cell());
  return line;
}

// reflow: join soft-wrapped rows of history + screen into logical lines and
// cut them again at the new width; the cursor stays on the same char, rows
// above the new screen go to history
static void tresize(int row, int col) {
  TextCursor *cursor = &screen.cursor;
  int old_cols = screen.view_cols;
  // skip blank rows below the cursor so shrinking doesn't push them to history
  int last = screen.view_rows - 1;
  while (last > cursor->row && row_blank(last))
    last--;
  int nsrc = hist_len + last + 1;
  int cur_src = hist_len + cursor->row;

  Cell **out = NULL; // rewrapped rows, oldest first
  char *out_wrap = NULL;
  int nout = 0, cap = 0;
  Cell *lbuf = NULL; // one logical line (without ATTR_WDUMMY placeholders)
  int lcap = 0;
  int cur_row = 0, cur_col = 0;

  for (int i = 0; i < nsrc;) {
    int len = 0, cur_off = -1;
    char wrap;
    do {
      Cell *src = src_row(i, &wrap);
      if (len + old_cols > lcap) {
        lcap = (len + old_cols) * 2;
        lbuf = realloc(lbuf, lcap * sizeof(Cell));
      }
      // skip padding cell left at the edge when a wide char wrapped early
      char next_wrap;
      int end = old_cols;
      if (wrap && i + 1 < nsrc &&
          (src_row(i + 1, &next_wrap)[0].mode & ATTR_WIDE) &&
          is_default_blank(src[old_cols - 1]) &&
          !(i == cur_src && cursor->col == old_cols - 1))
        end--;
      for (int j = 0; j < end; j++) {
        if (i == cur_src && j == cursor->col)
          cur_off = (src[j].mode & ATTR_WDUMMY) ? MAX(len - 1, 0) : len;
        if (src[j].mode & ATTR_WDUMMY)
          continue;
        lbuf[len++] = src[j];
      }
      i++;
    } while (wrap && i < nsrc);

    // drop trailing blanks, but keep up to the cursor's cell
    while (len > 0 && len - 1 > cur_off && is_default_blank(lbuf[len - 1]))
      len--;

    // emit at least one row for this logical line, never splitting wide glyphs
    if (nout == cap) {
      cap = cap ? cap * 2 : 64;
      out = realloc(out, cap * sizeof(Cell *));
      out_wrap = realloc(out_wrap, cap * sizeof(char));
    }
    out[nout] = alloc_blank_row(col);
    out_wrap[nout++] = 0;
    if (len == 0 && cur_off == 0) {
      cur_row = nout - 1;
      cur_col = 0;
    }
    int c = 0;
    for (int p = 0; p < len; p++) {
      Cell g = lbuf[p];
      int w = (g.mode & ATTR_WIDE) ? 2 : 1;
      if (w == 2 && col < 2) {
        g.mode &= ~ATTR_WIDE;
        w = 1;
      }
      if (c + w > col) {
        out_wrap[nout - 1] = 1;
        if (nout == cap) {
          cap *= 2;
          out = realloc(out, cap * sizeof(Cell *));
          out_wrap = realloc(out_wrap, cap * sizeof(char));
        }
        out[nout] = alloc_blank_row(col);
        out_wrap[nout++] = 0;
        c = 0;
      }
      if (p == cur_off) {
        cur_row = nout - 1;
        cur_col = c;
      }
      out[nout - 1][c] = g;
      if (w == 2) {
        out[nout - 1][c + 1] = (Cell){
            .u = 0,
            .fg = g.fg,
            .bg = g.bg,
            .mode = ATTR_WDUMMY,
        };
      }
      c += w;
    }
  }
  free(lbuf);

  // old rows are copied out; free them (spare history slots included)
  for (int i = 0; i < HIST_SIZE; i++) {
    free(hist[i]);
    hist[i] = NULL;
  }
  for (int i = 0; i < screen.buf_rows; i++)
    free(screen.buf[i]);

  // screen shows the last rows, moved up if that would hide the cursor
  int start = MAX(nout - row, 0);
  if (cur_row < start)
    start = cur_row;
  // rows above the screen become history, oldest dropped past HIST_SIZE
  int skip = MAX(start - HIST_SIZE, 0);
  for (int i = 0; i < skip; i++)
    free(out[i]);
  hist_len = start - skip;
  hist_head = hist_len % HIST_SIZE;
  for (int i = 0; i < hist_len; i++) {
    hist[i] = out[skip + i];
    hist_wrap[i] = out_wrap[skip + i];
  }
  screen.buf = realloc(screen.buf, row * sizeof(Cell *));
  screen.buf_wrap = realloc(screen.buf_wrap, row * sizeof(char));
  for (int i = 0; i < row; i++) {
    if (start + i < nout) {
      screen.buf[i] = out[start + i];
      screen.buf_wrap[i] = out_wrap[start + i];
    } else {
      screen.buf[i] = alloc_blank_row(col);
      screen.buf_wrap[i] = 0;
    }
  }
  // rows below the screen only exist when the cursor forced start up
  for (int i = start + row; i < nout; i++)
    free(out[i]);
  free(out);
  free(out_wrap);

  screen.buf_rows = row;
  screen.buf_cols = col;
  screen.view_rows = row;
  screen.view_cols = col;
  screen.scroll_top = 0;
  screen.scroll_bot = row - 1;
  screen.scroll_off = 0;
  wrap_next = 0;
  cursor->row = cur_row - start;
  cursor->col = cur_col;
}

static void cresize(int row, int col) {
  tresize(row, col);
  pty_set_winsize(screen.pty_fd, screen.view_rows, screen.view_cols);
}

void screen_resize_handler(XEvent *e) {
  int x_row = e->xconfigure.height / screen.char_height;
  int x_col = e->xconfigure.width / screen.char_width;
  x_row = MAX(x_row, 1);
  x_col = MAX(x_col, 1);
  if (x_col == screen.view_cols && x_row == screen.view_rows)
    return;
  cresize(x_row, x_col);
}
