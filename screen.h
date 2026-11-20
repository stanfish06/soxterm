#ifndef SCREEN_H
#define SCREEN_H

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <stdint.h>

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 600
#define HIST_SIZE 10000 // scrollback lines kept
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define DEFAULT(a, b) (a) = (a) ? (a) : (b);

// color encoding in Cell.fg / Cell.bg:
//   0..255           : indexed (0-15 palette, 16-231 6x6x6 cube, 232-255 gray)
//   COLOR_DEFAULT_*  : config.h defaultfg / defaultbg
//   COLOR_TRUE(r,g,b): 24-bit RGB
#define COLOR_DEFAULT_FG 256
#define COLOR_DEFAULT_BG 257
#define COLOR_COUNT 258
#define COLOR_TRUE_BIT (1u << 24)
#define COLOR_TRUE(r, g, b)                                                    \
  (COLOR_TRUE_BIT | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) |             \
   (uint32_t)(b))

enum CellAttr {
  ATTR_BOLD = 1 << 0,
  ATTR_FAINT = 1 << 1,
  ATTR_ITALIC = 1 << 2,
  ATTR_UNDERLINE = 1 << 3,
  ATTR_REVERSE = 1 << 4,
  ATTR_INVISIBLE = 1 << 5,
  ATTR_STRUCK = 1 << 6,
  ATTR_WIDE = 1 << 7,   // lead cell of a 2-column glyph
  ATTR_WDUMMY = 1 << 8, // trailing placeholder of a 2-column glyph
};

typedef struct {
  uint32_t u;    // Unicode codepoint
  uint32_t fg;   // foreground color
  uint32_t bg;   // background color
  uint16_t mode; // CellAttr bitmask
} Cell;

typedef struct {
  int row;
  int col;
  int visible;
} TextCursor;

typedef struct {
  Display *display;
  Window window;
  TextCursor cursor;
  int pty_fd;
  GC gc;
  XftDraw *draw;
  XftFont *fonts[4]; // [0]=regular, [1]=bold, [2]=italic, [3]=bold-italic
  double font_px;    // pixel size of fonts[0], reused for fallback fonts
  XftColor colors[COLOR_COUNT];
  XftColor cursor_bg;
  XftColor cursor_fg;
  Cell attr; // current SGR pen attributes applied to new/erased cells
  int char_width;
  int char_height;
  Cell **buf;
  char *buf_wrap; // buf_wrap[i]: row i was soft-wrapped onto row i + 1
  int buf_rows;
  int buf_cols;
  int view_rows;
  int view_cols;
  int scroll_top; // scroll region rows, 0-based inclusive (CSI r)
  int scroll_bot;
  int scroll_off; // rows scrolled back into history, 0 = live screen
} TermScreen;

unsigned int screen_default_bg(void);
void update_screen_pty(int pty_fd);
void screen_init(Display *display, Window window, TextCursor cursor, GC gc);
void screen_view_size(int *rows, int *cols);
void screen_draw_all_text(void);
void screen_draw_block_cursor(void);
void screen_feed(const char *buf, int len);
int screen_app_cursor_keys(void);
void screen_resize_handler(XEvent *);
void screen_scroll(int n);

#endif
