#include "pty.h"
#include "screen.h"
#include "scrollbar.h"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#define SCROLL_LINES 3 // rows per mouse wheel notch

Display *main_display = NULL;
Window main_window = 0;
ScrollBar main_scrollbar;
GC main_gc = 0;

// shared with the event handlers, which only get the XEvent
static int pty_fd = -1;
static int needs_redraw = 0;

static void expose(XEvent *);
static void configure(XEvent *);
static void bpress(XEvent *);
static void kpress(XEvent *);

// X event type -> handler; unlisted events are ignored
static void (*handler[LASTEvent])(XEvent *) = {
    [Expose] = expose,
    [ConfigureNotify] = configure,
    [ButtonPress] = bpress,
    [KeyPress] = kpress,
};

void create_window() {
  main_display = XOpenDisplay(NULL);
  if (main_display == NULL) {
    fprintf(stderr, "Error: cannot open display");
  } else {
    /****************
     * window
     ****************/
    // every window needs a root window
    Window root_window = XDefaultRootWindow(main_display);
    // (display, root window, x, y, width, height, border width, border,
    // background)
    main_window =
        XCreateSimpleWindow(main_display, root_window, 0, 0, SCREEN_WIDTH,
                            SCREEN_HEIGHT, 0, 0, screen_default_bg());
    XSizeHints *hints = XAllocSizeHints();
    hints->flags = PMinSize | PMaxSize;
    hints->min_width = 400;
    hints->min_height = 300;
    hints->max_width = 1920;
    hints->max_height = 1080;
    XSetWMNormalHints(main_display, main_window, hints);
    XFree(hints);

    /***************
     * make cursor
     ****************/
    Cursor cursor;
    cursor = XCreateFontCursor(main_display, XC_trek);
    if (cursor == None) {
      fprintf(stderr, "Error: cannot create cursor");
    }
    // add cursor to window
    XDefineCursor(main_display, main_window, cursor);
    TextCursor text_cursor;
    text_cursor.row = 0;
    text_cursor.col = 0;
    text_cursor.visible = 1;

    /**************
     * scroll bar
     **************/
    // TODO: need to dynamically adjust it
    /* main_scrollbar.win = XCreateSimpleWindow(
      main_display, main_window, 780, 0, 20, SCREEN_HEIGHT, 0, 0, 0x00808080);
    */

    // GC fills cell backgrounds, underlines, and the cursor block; Xft draws
    // glyphs
    main_gc = XCreateGC(main_display, main_window, 0, NULL);

    /***************
     * display
     ****************/
    XMapWindow(main_display, main_window);
    XFlush(main_display);
    screen_init(main_display, main_window, text_cursor, main_gc);
  }
}

// cursor keys and the final byte xterm sends for each
static const struct {
  KeySym sym;
  char final;
} cursor_keys[] = {
    {XK_Up, 'A'},    {XK_KP_Up, 'A'},    {XK_Down, 'B'}, {XK_KP_Down, 'B'},
    {XK_Right, 'C'}, {XK_KP_Right, 'C'}, {XK_Left, 'D'}, {XK_KP_Left, 'D'},
    {XK_Home, 'H'},  {XK_KP_Home, 'H'},  {XK_End, 'F'},  {XK_KP_End, 'F'},
};

// encode a cursor key into out; returns the byte count, 0 if not a cursor key
// plain: ESC [ x, or ESC O x in app cursor mode (DECCKM)
// with modifiers: ESC [ 1 ; m x, where m = 1 + shift(1) + alt(2) + ctrl(4)
static int encode_cursor_key(KeySym sym, unsigned int state, char *out,
                             size_t size) {
  for (size_t i = 0; i < sizeof(cursor_keys) / sizeof(cursor_keys[0]); i++) {
    if (cursor_keys[i].sym != sym)
      continue;
    int mod = 1 + ((state & ShiftMask) ? 1 : 0) + ((state & Mod1Mask) ? 2 : 0) +
              ((state & ControlMask) ? 4 : 0);
    char f = cursor_keys[i].final;
    if (mod > 1)
      return snprintf(out, size, "\033[1;%d%c", mod, f);
    return snprintf(out, size, "\033%c%c", screen_app_cursor_keys() ? 'O' : '[',
                    f);
  }
  return 0;
}

static void expose(XEvent *e) { needs_redraw = 1; }

static void configure(XEvent *e) {
  screen_resize_handler(e);
  needs_redraw = 1;
}

// wheel: button 4 scrolls back, button 5 forward
static void bpress(XEvent *e) {
  if (e->xbutton.button == Button4) {
    screen_scroll(SCROLL_LINES);
    needs_redraw = 1;
  } else if (e->xbutton.button == Button5) {
    screen_scroll(-SCROLL_LINES);
    needs_redraw = 1;
  }
}

static void kpress(XEvent *e) {
  char buf[32] = {0};
  KeySym keysym;
  int count = XLookupString(&e->xkey, buf, sizeof(buf), &keysym, NULL);
  // Shift+PgUp/PgDn scroll history by a page, not sent to the pty
  if ((e->xkey.state & ShiftMask) &&
      (keysym == XK_Prior || keysym == XK_Next)) {
    int rows, cols;
    screen_view_size(&rows, &cols);
    screen_scroll(keysym == XK_Prior ? rows : -rows);
    needs_redraw = 1;
    return;
  }
  char seq[16];
  int seq_len = encode_cursor_key(keysym, e->xkey.state, seq, sizeof(seq));
  if (seq_len > 0) {
    // input to the pty returns the view to the live screen
    screen_scroll(-HIST_SIZE);
    needs_redraw = 1;
    write(pty_fd, seq, seq_len);
    return;
  }
  if (keysym == XK_Return) {
    buf[0] = '\r';
    count = 1;
  } else if (keysym == XK_BackSpace) {
    buf[0] = 0x7f;
    count = 1;
  }
  if (count > 0) {
    screen_scroll(-HIST_SIZE);
    needs_redraw = 1;
    if (e->xkey.state & Mod1Mask)
      write(pty_fd, "\033", 1);
    write(pty_fd, buf, count);
  }
}

int main() {
  // enable UTF-8 locale so wcwidth() knows wide/combining codepoints
  setlocale(LC_CTYPE, "");
  create_window();
  XSelectInput(main_display, main_window,
               ExposureMask | KeyPressMask | ButtonPressMask |
                   StructureNotifyMask);

  struct winsize ws;
  int vrows, vcols;
  // use the grid screen_init measured so the pty and the screen agree
  screen_view_size(&vrows, &vcols);
  ws.ws_row = vrows;
  ws.ws_col = vcols;
  ws.ws_xpixel = SCREEN_WIDTH;
  ws.ws_ypixel = SCREEN_HEIGHT;
  if (pty_fork(&pty_fd, &ws) < 0) {
    fprintf(stderr, "Error: failed to start PTY\n");
    return 1;
  }
  update_screen_pty(pty_fd);
  int xfd = ConnectionNumber(main_display);
  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(xfd, &rfds);
    FD_SET(pty_fd, &rfds);
    int maxfd = (xfd > pty_fd) ? xfd : pty_fd;

    int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("select");
      break;
    }

    needs_redraw = 0;
    if (FD_ISSET(pty_fd, &rfds)) {
      char buf[4096];
      int n = (int)read(pty_fd, buf, sizeof(buf));
      if (n > 0) {
        screen_feed(buf, n);
        needs_redraw = 1;
      } else if (n == 0) {
        break;
      } else if (errno != EINTR && errno != EAGAIN) {
        perror("read pty");
        break;
      }
    }

    if (FD_ISSET(xfd, &rfds)) {
      while (XPending(main_display)) {
        XEvent e;
        XNextEvent(main_display, &e);
        if (handler[e.type])
          (handler[e.type])(&e);
      }
    }

    // print_screen_buf_text(1);

    if (needs_redraw) {
      screen_draw_all_text();
      screen_draw_block_cursor();
      XFlush(main_display);
    }
  }
}
