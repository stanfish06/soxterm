#ifndef SCREEN_H
#define SCREEN_H

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 600
#define BG_COLOR 0x001e1e1e
#define FG_COLOR 0x00FFFFFF
#define HIST_SIZE 10000 // scrollback lines kept
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#define MAX(a, b)   ((a) < (b) ? (b) : (a))

// Macros
#define DEFAULT(a, b) (a) = (a) ? (a) : (b);
// type for text
typedef unsigned char text_t;

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
  XftColor cursor_fg; // text under the block cursor
  GC gc;
  XftFont *font;
  XftDraw *draw;
  XftColor fg;        // text color
  int char_width;
  int char_height;
  text_t **buf_text;
  char *buf_wrap; // buf_wrap[i]: row i was soft-wrapped onto row i + 1
  int buf_rows;
  int buf_cols;
  int view_rows;
  int view_cols;
  int scroll_top; // scroll region rows, 0-based inclusive (CSI r)
  int scroll_bot;
  int scroll_off; // rows scrolled back into history, 0 = live screen
} TermScreen;

void update_screen_pty(int pty_fd);
void screen_init(Display *display, Window window, TextCursor cursor, GC gc, XftFont *font);
void screen_view_size(int *rows, int *cols);
void screen_greet();
void screen_clear();
void screen_draw_all_text();
void screen_draw_text(const char *text, int row, int col);
void screen_draw_block_cursor();
void move_cursor(int num_cols_shift, int num_rows_shift,
                 int direction, int horizontal);
void remove_cursor();
void delete_at_cursor();
void screen_feed(const char *buf, int len);
int screen_app_cursor_keys();
void screen_refresh();
void print_screen_buf_text(int view);
void screen_resize_handler(XEvent *);
void screen_scroll(int n);

#endif
