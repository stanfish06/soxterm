#ifndef CONFIG_H
#define CONFIG_H

static const char *font = "monospace:size=12";

// 16 ANSI colors (0-7 normal, 8-15 bright) as 0x00RRGGBB
static const unsigned int colorname[16] = {
    0x1e1e1e, // 0: black
    0xcc241d, // 1: red
    0x98971a, // 2: green
    0xd79921, // 3: yellow
    0x458588, // 4: blue
    0xb16286, // 5: magenta
    0x689d6a, // 6: cyan
    0xa89984, // 7: white
    0x928374, // 8: bright black
    0xfb4934, // 9: bright red
    0xb8bb26, // 10: bright green
    0xfabd2f, // 11: bright yellow
    0x83a598, // 12: bright blue
    0xd3869b, // 13: bright magenta
    0x8ec07c, // 14: bright cyan
    0xebdbb2, // 15: bright white
};

static const unsigned int defaultfg = 0xffffff;
static const unsigned int defaultbg = 0x1e1e1e;
static const unsigned int defaultcs = 0xffffff;  // block cursor bg
static const unsigned int defaultrcs = 0x000000; // text under cursor

#endif
