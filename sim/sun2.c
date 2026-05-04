/*
 * sun-2 emulator
 * 10/2014  Brad Parker <brad@heeltoe.com>
 *
 * Sun-2 video and keyboard emulation via SDL
 *
 * Copyright (C) 2017-2018 Brad Parker <brad@heeltoe.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#include "sim.h"
#include "icon_data.h"

/* Sun-2 modifier scancodes */
#define SUN2_SC_LSHIFT   99
#define SUN2_SC_RSHIFT   111
#define SUN2_SC_CTRL     76
#define SUN2_SC_CAPSLOCK 119

/* Clipboard paste: maps ASCII to Sun-2 scancodes and drip-feeds keystrokes
   into the SCC FIFO so the PROM/SunOS sees them as if typed. */
static char *paste_buf = NULL;
static int paste_pos = 0;
static int paste_len = 0;
static int paste_delay = 0;
#define PASTE_CHAR_DELAY 3

/* Forward declaration -- defined further down with the other SDL globals. */
static SDL_Window *screen;

/* ---- MouseSystems 5-byte mouse on SCC channel 2 ----
   Left-click captures the mouse; Right Alt (PC) / Right Option (Mac)
   releases.  When captured, motion deltas accumulate per poll and a
   packet is emitted per MOUSE_PACKET_DELAY cycles to keep the 16-byte
   SCC FIFO from overflowing.  Button-only packets flush immediately. */
static int mouse_captured = 0;
static int mouse_dx = 0;
static int mouse_dy = 0;
static unsigned char mouse_buttons = 0x07; /* bits 2,1,0 = L,M,R; 0=pressed */
static int mouse_delay = 0;
#define MOUSE_PACKET_DELAY 2

static void mouse_update_title(void)
{
  char title[128];
#ifdef __APPLE__
  const char *key = "Right Option";
#else
  const char *key = "Right Alt";
#endif
  if (mouse_captured) {
    snprintf(title, sizeof(title),
             "Sun2 - Mouse captured, press %s to release", key);
    SDL_SetWindowTitle(screen, title);
  } else {
    SDL_SetWindowTitle(screen, "Sun2");
  }
}

static void mouse_toggle_capture(void)
{
  mouse_captured = !mouse_captured;
  SDL_SetRelativeMouseMode(mouse_captured ? SDL_TRUE : SDL_FALSE);
  mouse_update_title();
  printf("mouse: capture %s\n", mouse_captured ? "ON" : "OFF");
}

static void mouse_send_packet(void)
{
  if (mouse_delay > 0) { mouse_delay--; return; }
  if (mouse_dx == 0 && mouse_dy == 0) return;

  int dx = mouse_dx, dy = mouse_dy;
  mouse_dx = 0; mouse_dy = 0;

  if (dx >  127) dx =  127;
  if (dx < -127) dx = -127;
  if (dy >  127) dy =  127;
  if (dy < -127) dy = -127;

  int dx1 = dx / 2, dx2 = dx - dx1;
  int dy1 = dy / 2, dy2 = dy - dy1;
  /* Sun: positive dy = up; SDL: positive dy = down. */
  dy1 = -dy1; dy2 = -dy2;

  if (0) printf("mouse: pkt btn=0x%02x dx=%d,%d dy=%d,%d\n",
                0x80 | mouse_buttons, dx1, dx2, dy1, dy2);

  scc_in_push(2, 0x80 | mouse_buttons);
  scc_in_push(2, (unsigned char)dx1);
  scc_in_push(2, (unsigned char)dy1);
  scc_in_push(2, (unsigned char)dx2);
  scc_in_push(2, (unsigned char)dy2);

  mouse_delay = MOUSE_PACKET_DELAY;
}

static void mouse_send_button_packet(void)
{
  scc_in_push(2, 0x80 | mouse_buttons);
  scc_in_push(2, 0);
  scc_in_push(2, 0);
  scc_in_push(2, 0);
  scc_in_push(2, 0);
}

struct ascii_to_sun2 {
  unsigned char sc;
  unsigned char shifted;
};
static struct ascii_to_sun2 ascii_map[128];

static void init_ascii_map(void)
{
  int i;
  for (i = 0; i < 128; i++) { ascii_map[i].sc = 0; ascii_map[i].shifted = 0; }

  /* lowercase letters */
  ascii_map['a'].sc = 77;  ascii_map['b'].sc = 104;
  ascii_map['c'].sc = 102; ascii_map['d'].sc = 79;
  ascii_map['e'].sc = 56;  ascii_map['f'].sc = 80;
  ascii_map['g'].sc = 81;  ascii_map['h'].sc = 82;
  ascii_map['i'].sc = 61;  ascii_map['j'].sc = 83;
  ascii_map['k'].sc = 84;  ascii_map['l'].sc = 85;
  ascii_map['m'].sc = 106; ascii_map['n'].sc = 105;
  ascii_map['o'].sc = 62;  ascii_map['p'].sc = 63;
  ascii_map['q'].sc = 54;  ascii_map['r'].sc = 57;
  ascii_map['s'].sc = 78;  ascii_map['t'].sc = 58;
  ascii_map['u'].sc = 60;  ascii_map['v'].sc = 103;
  ascii_map['w'].sc = 55;  ascii_map['x'].sc = 101;
  ascii_map['y'].sc = 59;  ascii_map['z'].sc = 100;

  /* uppercase = same scancode but shifted */
  for (i = 'A'; i <= 'Z'; i++) {
    ascii_map[i].sc = ascii_map[i + 32].sc;
    ascii_map[i].shifted = 1;
  }

  /* digits */
  ascii_map['1'].sc = 30;  ascii_map['2'].sc = 31;
  ascii_map['3'].sc = 32;  ascii_map['4'].sc = 33;
  ascii_map['5'].sc = 34;  ascii_map['6'].sc = 35;
  ascii_map['7'].sc = 36;  ascii_map['8'].sc = 37;
  ascii_map['9'].sc = 38;  ascii_map['0'].sc = 39;

  /* shifted digits */
  ascii_map['!'].sc = 30;  ascii_map['!'].shifted = 1;
  ascii_map['@'].sc = 31;  ascii_map['@'].shifted = 1;
  ascii_map['#'].sc = 32;  ascii_map['#'].shifted = 1;
  ascii_map['$'].sc = 33;  ascii_map['$'].shifted = 1;
  ascii_map['%'].sc = 34;  ascii_map['%'].shifted = 1;
  ascii_map['^'].sc = 35;  ascii_map['^'].shifted = 1;
  ascii_map['&'].sc = 36;  ascii_map['&'].shifted = 1;
  ascii_map['*'].sc = 37;  ascii_map['*'].shifted = 1;
  ascii_map['('].sc = 38;  ascii_map['('].shifted = 1;
  ascii_map[')'].sc = 39;  ascii_map[')'].shifted = 1;

  /* punctuation - unshifted */
  ascii_map['-'].sc = 40;  ascii_map['='].sc = 41;
  ascii_map['`'].sc = 42;  ascii_map['\t'].sc = 53;
  ascii_map['['].sc = 64;  ascii_map[']'].sc = 65;
  ascii_map[';'].sc = 86;  ascii_map['\''].sc = 87;
  ascii_map['\\'].sc = 88; ascii_map['\r'].sc = 89;
  ascii_map['\n'].sc = 89;
  ascii_map[','].sc = 107; ascii_map['.'].sc = 108;
  ascii_map['/'].sc = 109; ascii_map[' '].sc = 121;
  ascii_map[27].sc = 29;   /* ESC */
  ascii_map[8].sc = 43;    /* backspace */

  /* punctuation - shifted */
  ascii_map['_'].sc = 40;  ascii_map['_'].shifted = 1;
  ascii_map['+'].sc = 41;  ascii_map['+'].shifted = 1;
  ascii_map['~'].sc = 42;  ascii_map['~'].shifted = 1;
  ascii_map['{'].sc = 64;  ascii_map['{'].shifted = 1;
  ascii_map['}'].sc = 65;  ascii_map['}'].shifted = 1;
  ascii_map[':'].sc = 86;  ascii_map[':'].shifted = 1;
  ascii_map['"'].sc = 87;  ascii_map['"'].shifted = 1;
  ascii_map['|'].sc = 88;  ascii_map['|'].shifted = 1;
  ascii_map['<'].sc = 107; ascii_map['<'].shifted = 1;
  ascii_map['>'].sc = 108; ascii_map['>'].shifted = 1;
  ascii_map['?'].sc = 109; ascii_map['?'].shifted = 1;
}

static void paste_start(void)
{
  if (!SDL_HasClipboardText()) return;

  char *clip = SDL_GetClipboardText();
  if (clip == NULL || clip[0] == '\0') { SDL_free(clip); return; }

  if (paste_buf != NULL) free(paste_buf);
  paste_len = strlen(clip);
  paste_buf = malloc(paste_len + 1);
  memcpy(paste_buf, clip, paste_len + 1);
  paste_pos = 0;
  paste_delay = 0;
  SDL_free(clip);
}

/* Feed one character from paste buffer into SCC FIFO; called every poll. */
static void paste_feed(void)
{
  if (paste_buf == NULL || paste_pos >= paste_len) return;
  if (paste_delay > 0) { paste_delay--; return; }

  unsigned char ch = (unsigned char)paste_buf[paste_pos];
  if (ch >= 128 || ascii_map[ch].sc == 0) { paste_pos++; return; }

  unsigned char sc = ascii_map[ch].sc;
  if (ascii_map[ch].shifted) {
    scc_in_push(3, SUN2_SC_LSHIFT);
    scc_in_push(3, sc);
    scc_in_push(3, sc | 0x80);
    scc_in_push(3, SUN2_SC_LSHIFT | 0x80);
  } else {
    scc_in_push(3, sc);
    scc_in_push(3, sc | 0x80);
  }

  paste_pos++;
  paste_delay = PASTE_CHAR_DELAY;

  if (paste_pos >= paste_len) {
    free(paste_buf); paste_buf = NULL;
    paste_pos = paste_len = 0;
  }
}

static unsigned char *fbmem;
static SDL_Window* screen;
static SDL_Renderer* renderer;
static SDL_Texture* texture;

/* Logical framebuffer dimensions (must match what the PROM writes). */
static int rows, cols;
/* Last observed window size — used by the resize-snap handler so we know
   which dimension the user just changed. */
static int last_win_w, last_win_h;

static int toggle_trace;
static unsigned int fbctrl;

/* Sun bwtwo render: 1 bpp, MSB-first within byte, bit 1 = black, bit 0 = white.
   Renders fbmem into the streaming texture. */
static void sdl_render_frame(void)
{
  if (!texture || !fbmem) return;

  void *pixels;
  int pitch;
  if (SDL_LockTexture(texture, NULL, &pixels, &pitch) < 0) return;

  const Uint32 black = 0xFF000000u;
  const Uint32 white = 0xFFFFFFFFu;
  int bytes_per_row = cols / 8;
  int pitch_pix = pitch / (int)sizeof(Uint32);
  Uint32 *base = (Uint32 *)pixels;

  for (int y = 0; y < rows; y++) {
    Uint32 *p = base + y * pitch_pix;
    const unsigned char *src = fbmem + y * bytes_per_row;
    for (int x = 0; x < bytes_per_row; x++) {
      unsigned char b = src[x];
      *p++ = (b & 0x80) ? black : white;
      *p++ = (b & 0x40) ? black : white;
      *p++ = (b & 0x20) ? black : white;
      *p++ = (b & 0x10) ? black : white;
      *p++ = (b & 0x08) ? black : white;
      *p++ = (b & 0x04) ? black : white;
      *p++ = (b & 0x02) ? black : white;
      *p++ = (b & 0x01) ? black : white;
    }
  }

  SDL_UnlockTexture(texture);
}

static int sdl_ready = 0;

void sdl_init(void)
{
    /* Logical FB dimensions come from the active --mode=.  The same mode also
       drives the bwtwo CSR JUMPER_HIRES bit (sampled by PROM) so the PROM and
       the renderer agree on stride. */
    cols = g_mode->width;
    rows = g_mode->height;

    /* Default window size = native FB dimensions (1:1 logical pixel mapping).
       Resize-snap below keeps the FB aspect on user resize. */
    int win_w = cols;
    int win_h = rows;
    last_win_w = win_w;
    last_win_h = win_h;

    printf("SDL: initializing video subsystem...\n");

    if (SDL_Init(SDL_INIT_VIDEO)) {
        printf("ERROR: SDL_Init(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
        printf("ERROR: No display will be available. Check DISPLAY or WAYLAND_DISPLAY.\n");
        return;
    }

    printf("SDL: video driver: %s\n", SDL_GetCurrentVideoDriver());

    screen = SDL_CreateWindow("Sun2",
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              win_w, win_h,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!screen) {
        printf("ERROR: SDL_CreateWindow failed: %s\n", SDL_GetError());
        printf("ERROR: No display will be available.\n");
        return;
    }

    /* Set the SDL window icon from the embedded RGBA pixel array.  The
       array in icon_data.h is 64x64 RGBA top-to-bottom, byte order R,G,B,A.
       The .ico embedded via sim.rc covers the taskbar / Alt-Tab / file
       explorer use; this call covers the in-window decoration. */
    {
        SDL_Surface *icon = SDL_CreateRGBSurfaceFrom(
            (void *)icon_data, ICON_WIDTH, ICON_HEIGHT, 32, ICON_WIDTH * 4,
            0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
        if (icon) {
            SDL_SetWindowIcon(screen, icon);
            SDL_FreeSurface(icon);
        }
    }

    /* Try hardware-accelerated renderer first, fall back to software. */
    renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        printf("SDL: accelerated renderer failed (%s), trying software...\n", SDL_GetError());
        renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) {
            printf("ERROR: SDL_CreateRenderer failed: %s\n", SDL_GetError());
            printf("ERROR: No display will be available.\n");
            return;
        }
    }

    /* Map the logical framebuffer onto whatever window size,
       preserving aspect ratio and using nearest-neighbor for crisp pixels. */
    SDL_RenderSetLogicalSize(renderer, cols, rows);
    SDL_RenderSetIntegerScale(renderer, SDL_FALSE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    texture = SDL_CreateTexture(renderer,
                                SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING,
                                cols, rows);
    if (!texture) {
        printf("ERROR: SDL_CreateTexture failed: %s\n", SDL_GetError());
        printf("ERROR: No display will be available.\n");
        return;
    }

    SDL_RendererInfo info;
    SDL_GetRendererInfo(renderer, &info);
    printf("SDL: display ready - %dx%d framebuffer, %dx%d window, renderer=%s\n",
           cols, rows, win_w, win_h, info.name);
    sdl_ready = 1;
}

//void sun2_sdl_key(int sdl_code, int modifiers, unsigned int unicode, int down);
void sun2_sdl_key(SDL_Keycode sdl_code, uint16_t modifiers, SDL_Scancode unicode, int down);

void sdl_poll(void)
{
  SDL_Event event;

  if (!sdl_ready)
    return;

  sdl_render_frame();
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, NULL, NULL);
  SDL_RenderPresent(renderer);

  while (SDL_PollEvent(&event)) {
    switch (event.type) {
	    case SDL_WINDOWEVENT:
	      /* Snap window size to FB aspect on resize so the picture always
	         fills the window — no letterbox bars from SDL_RenderSetLogicalSize.
	         Strategy: figure out which dimension the user just changed, treat
	         that one as authoritative, derive the other from cols:rows. */
	      if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
	        int w = event.window.data1;
	        int h = event.window.data2;
	        int dw = w - last_win_w; if (dw < 0) dw = -dw;
	        int dh = h - last_win_h; if (dh < 0) dh = -dh;
	        int target_w, target_h;
	        if (dw >= dh) {
	          target_w = w;
	          target_h = (int)((long)w * rows / cols);
	        } else {
	          target_h = h;
	          target_w = (int)((long)h * cols / rows);
	        }
	        if (target_w < 1) target_w = 1;
	        if (target_h < 1) target_h = 1;
	        last_win_w = target_w;
	        last_win_h = target_h;
	        if (target_w != w || target_h != h)
	          SDL_SetWindowSize(screen, target_w, target_h);
	      }
	      break;

	    case SDL_KEYDOWN:
	      if (event.key.repeat)
	        break; /* suppress SDL auto-repeat -- SunOS does software repeat */
	      /* Right Alt (PC) / Right Option (Mac) RELEASES the mouse.
	         Capture happens by clicking inside the window.  Match on the
	         positional scancode -- SDLK_RALT alone misses on macOS layouts
	         where Right Option produces a special character. */
	      if (event.key.keysym.scancode == SDL_SCANCODE_RALT ||
	          event.key.keysym.sym == SDLK_RALT) {
	        if (mouse_captured)
	          mouse_toggle_capture();
	        break;
	      }
	      sun2_sdl_key(event.key.keysym.sym, event.key.keysym.mod, event.key.keysym.scancode, 1);
	      break;

	    case SDL_TEXTINPUT:
	      /* Sun-2 keyboard uses make/break scancodes only, discard SDL text events */
	      break;

	    case SDL_KEYUP:
	      sun2_sdl_key(event.key.keysym.sym, event.key.keysym.mod, event.key.keysym.scancode, 0);
	      break;
	    case SDL_QUIT:
	      SDL_Quit();
	      exit(0);
	    case SDL_MOUSEMOTION:
	      if (mouse_captured) {
	        mouse_dx += event.motion.xrel;
	        mouse_dy += event.motion.yrel;
	      }
	      break;
	    case SDL_MOUSEBUTTONDOWN:
	      if (mouse_captured) {
	        if      (event.button.button == SDL_BUTTON_LEFT)   mouse_buttons &= ~0x04;
	        else if (event.button.button == SDL_BUTTON_MIDDLE) mouse_buttons &= ~0x02;
	        else if (event.button.button == SDL_BUTTON_RIGHT)  mouse_buttons &= ~0x01;
	        mouse_send_button_packet();
	      } else {
	        /* Not captured: left-click captures the mouse, right-click
	           still pastes clipboard text. */
	        if (event.button.button == SDL_BUTTON_LEFT)
	          mouse_toggle_capture();
	        else if (event.button.button == SDL_BUTTON_RIGHT)
	          paste_start();
	      }
	      break;
	    case SDL_MOUSEBUTTONUP:
	      if (mouse_captured) {
	        if      (event.button.button == SDL_BUTTON_LEFT)   mouse_buttons |= 0x04;
	        else if (event.button.button == SDL_BUTTON_MIDDLE) mouse_buttons |= 0x02;
	        else if (event.button.button == SDL_BUTTON_RIGHT)  mouse_buttons |= 0x01;
	        mouse_send_button_packet();
	      }
	      break;
	    }
  }

  /* Drip-feed paste buffer into the SCC keyboard FIFO. */
  paste_feed();

  /* Send accumulated mouse motion to SCC channel 2. */
  if (mouse_captured)
    mouse_send_packet();
}

void sun2_fb_alloc(void)
{
  if (fbmem == NULL) {
    fbmem = malloc(128*1024);
    memset(fbmem, 0, 128*1024);
    sdl_init();
  }
}

unsigned int sun2_video_read(unsigned int address, int size)
{
  unsigned int *p32;
  unsigned short *p16;
  unsigned char *p08;
  unsigned offset;

  sun2_fb_alloc();

  offset = address & 0xfffff;
  if (offset >= 128*1024)
    return 0xffffffff;

  switch (size) {
  case 1:
    p08 = (unsigned char *)(fbmem + offset);
    return *p08;
  case 2:
    p08 = (unsigned char *)(fbmem + offset);
    return (p08[0] << 8) | p08[1];
  case 4:
    p08 = (unsigned char *)(fbmem + offset);
    return (p08[0] << 24) | (p08[1] << 16) | (p08[2] << 8) | p08[3];
  default:
    return 0x0;
  }
}

unsigned int sun2_video_write(unsigned int address, int size, unsigned int value)
{
  unsigned int *p32;
  unsigned short *p16;
  unsigned char *p08;
  unsigned offset;

  sun2_fb_alloc();

  offset = address & 0xfffff;
  if (offset >= 128*1024)
    return -1;

  switch (size) {
  case 1:
    p08 = (unsigned char *)(fbmem + (address & 0xfffff));
    *p08 = value;
    break;
  case 2:
    p08 = (unsigned char *)(fbmem + (address & 0xfffff));
    p08[0] = value >> 8;
    p08[1] = value;
    break;
  case 4:
    p08 = (unsigned char *)(fbmem + (address & 0xfffff));
    p08[0] = value >> 24;
    p08[1] = value >> 16;
    p08[2] = value >> 8;
    p08[3] = value;
    break;
  }
  return 0;
}

unsigned int sun2_kbm_read(unsigned int address, int size)
{
  unsigned int value;
  if (0) printf("sun2: scc read %x (%d)\n", address, size);
  value = scc_read(address, size);
  if (0) printf("sun2: scc read %x -> %x (%d)\n", address, value, size);
  return value;
}

unsigned int sun2_kbm_write(unsigned int address, int size, unsigned int value)
{
  if (0) printf("sun2: scc write %x <- %x(%d)\n", address, value, size);
  scc_write(address, value, size);
  return 0;
}

/* bwtwo Multibus CSR at base+0x81800 (PA 0x781800).  16-bit big-endian register.
   Bit layout (per C# RetroCore SunBwTwo.cs and TME tme-0.8/machine/sun/sun-bwtwo.c):
     15 VIDEO_ENABLE  RW    11 JUMPER_B       RO
     14 COPY_ENABLE   RW    10 JUMPER_A       RO
     13 INT_ENABLE    RW     9 JUMPER_COLOR   RO
     12 INT_ACTIVE    RO     8 JUMPER_HIRES   RO   <-- PROM reads this to pick resolution
      6:1 COPYBASE_MASK RW
   The PROM's resolution-detect (sun2-multi-rev-R.bin PC 0xef3168) reads BYTE at
   PA 0x781800 (high byte of CSR) and tests bit 0 — that's bit 8 of the CSR =
   JUMPER_HIRES.  JUMPER_HIRES=0 -> 1152x900 path; JUMPER_HIRES=1 -> 1024x1024.
   The jumper value comes from the active --mode= entry (g_mode->hires_jumper). */
#define BWTWO_CSR_RW_MASK    0xE07Eu  /* VIDEO_ENABLE, COPY_ENABLE, INT_ENABLE, COPYBASE */
#define BWTWO_CSR_JUMPER_HIRES 0x0100u

static unsigned int bwtwo_csr_ro_bits(void) {
    return g_mode && g_mode->hires_jumper ? BWTWO_CSR_JUMPER_HIRES : 0;
}

unsigned int sun2_video_ctl_read(unsigned int address, int size)
{
  unsigned int value;
  unsigned int reg = (fbctrl & BWTWO_CSR_RW_MASK) | bwtwo_csr_ro_bits();
  unsigned int off = address & 1;

  switch (size) {
  case 1:
    value = off ? (reg & 0xff) : ((reg >> 8) & 0xff);
    break;
  case 2:
    value = reg;
    break;
  default:
    value = reg;
    break;
  }
  if (0) printf("sun2: fb ctrl @ %x -> %x (%d)\n", address, value, size);
  return value;
}

unsigned int sun2_video_ctl_write(unsigned int address, int size, unsigned int value)
{
  if (0) printf("sun2: fb ctrl @ %x <- %x (%d)\n", address, value, size);
  /* Only the RW bits are writable; ignore writes to RO jumper/status bits. */
  if (size == 1) {
    unsigned int off = address & 1;
    if (off == 0)
      fbctrl = (fbctrl & 0x00ff) | ((value & 0xff) << 8);
    else
      fbctrl = (fbctrl & 0xff00) | (value & 0xff);
  } else {
    fbctrl = value & 0xffff;
  }
  fbctrl &= BWTWO_CSR_RW_MASK;
  return 0;
}

/* Sun keyboard L1-A (Stop-A) abort sequence.  Bound to F12 to match
   RetroCore SunKeyboardMapper.IsAbortKey().  Real Sun keyboard scancodes:
     L1 = 1, A = 77, idle = 0x7F.  Bit 7 of a scancode marks a key release. */
#define SUN_KEY_L1   1
#define SUN_KEY_A    77
#define SUN_KEY_IDLE 0x7F

/* Auto-abort: matches RetroCore SunVideoBoard.cs (ABORT_AUTO_BOOT path).
   On the FIRST keyboard "bell off" command from the PROM, synthesise the
   L1-A abort burst — drops the auto-boot to the PROM monitor command
   prompt.  Subsequent bell-offs (e.g. test-acknowledgment beeps) are
   no-op so commands like 'x' run normally. */
static int auto_abort_enabled = 0;     /* default: off; --auto-abort turns it on */
static int auto_abort_done = 0;

void sun2_set_auto_abort(int enabled) { auto_abort_enabled = enabled; }

static void sun2_send_abort(void)
{
  scc_in_push(3, SUN_KEY_L1);
  scc_in_push(3, SUN_KEY_A);
  scc_in_push(3, SUN_KEY_A   | 0x80);
  scc_in_push(3, SUN_KEY_L1  | 0x80);
  scc_in_push(3, SUN_KEY_IDLE);
}

/* ---- auto-typer (drives the PROM monitor non-interactively for trace
   capture).  Uses the same SCC keyboard channel as physical keystrokes:
   pushes the press scancode, waits, then pushes the release scancode.
   Driven from io_update via sun2_autotype_tick().

   Special character `\033` (ESC) in the input string sends the L1-A
   abort burst — the equivalent of pressing F12 in the SDL window —
   so headless test runs can break into the PROM monitor. */

extern unsigned int map_sdl_to_sun2kb[512];

#define SUN_KEY_LSHIFT 99

static unsigned autotype_pos;
static unsigned autotype_delay;        /* tick countdown before next action */
#define AUTOTYPE_BOOT_DELAY  20000000  /* ~20M io_update ticks before first key — let PROM reach prompt */
#define AUTOTYPE_PRESS_HOLD  100000    /* hold each key down */
#define AUTOTYPE_GAP         200000    /* gap between successive keys */

/* ASCII → Sun-2 scancode + shift flag.  Returns 1 if mapped, 0 otherwise.
   Shifted characters require pressing LSHIFT around the key press. */
static int autotype_lookup(unsigned char ch, unsigned int *out_code, int *out_shifted)
{
  static const struct { unsigned char ch; unsigned char code; } shifted[] = {
    {'!', 30}, {'@', 31}, {'#', 32}, {'$', 33}, {'%', 34}, {'^', 35},
    {'&', 36}, {'*', 37}, {'(', 38}, {')', 39}, {'_', 40}, {'+', 41},
    {'~', 42}, {'{', 64}, {'}', 65}, {':', 86}, {'"', 87}, {'|', 88},
    {'<', 107}, {'>', 108}, {'?', 109},
  };
  for (size_t i = 0; i < sizeof(shifted)/sizeof(shifted[0]); i++) {
    if (shifted[i].ch == ch) {
      *out_code = shifted[i].code;
      *out_shifted = 1;
      return 1;
    }
  }
  if (ch >= 'A' && ch <= 'Z') {
    unsigned int code = map_sdl_to_sun2kb[ch - 'A' + 'a'] & 0xff;
    if (code) { *out_code = code; *out_shifted = 1; return 1; }
  }
  unsigned int code = map_sdl_to_sun2kb[ch] & 0xff;
  if (code) { *out_code = code; *out_shifted = 0; return 1; }
  return 0;
}

void sun2_autotype_tick(void)
{
  static int started;
  static unsigned char emit_buf[6];   /* shift-dn, key-dn, key-up, shift-up */
  static unsigned emit_count;
  static unsigned emit_idx;

  if (!g_autotype) return;
  /* Wait for auto-abort to have dropped the PROM to the monitor prompt
     before injecting any keystrokes — otherwise the keys queue up in the
     SCC FIFO ahead of the L1-A abort burst and get consumed by the
     still-running auto-boot.  Then add a short post-abort settle delay
     so the PROM has finished switching context. */
  if (auto_abort_enabled && !auto_abort_done) return;
  if (!started) {
    autotype_delay = AUTOTYPE_GAP * 4;   /* settle after auto-abort */
    started = 1;
  }
  if (autotype_delay) { autotype_delay--; return; }

  /* Drain any pending scancodes for the current char (shift-dn / key-dn /
     key-up / shift-up — up to 4 bytes for a shifted char). */
  if (emit_idx < emit_count) {
    scc_in_push(3, emit_buf[emit_idx++]);
    if (emit_idx < emit_count) {
      autotype_delay = AUTOTYPE_PRESS_HOLD;
    } else {
      autotype_delay = AUTOTYPE_GAP;
      autotype_pos++;
    }
    return;
  }

  if (!g_autotype[autotype_pos]) return;

  unsigned char ch = (unsigned char)g_autotype[autotype_pos];

  /* Special: ESC (0x1B) → L1-A abort burst (matches F12 in SDL window) */
  if (ch == 0x1B) {
    printf("autotype: send L1-A (abort)\n");
    sun2_send_abort();
    autotype_pos++;
    autotype_delay = AUTOTYPE_GAP;
    return;
  }

  if (ch == '\n') ch = '\r';     /* SDLK_RETURN = 0x0D */

  unsigned int code;
  int shifted;
  if (!autotype_lookup(ch, &code, &shifted)) {
    printf("autotype: skipping unmapped char 0x%02x\n", ch);
    autotype_pos++;
    autotype_delay = AUTOTYPE_GAP;
    return;
  }

  emit_count = 0;
  if (shifted) emit_buf[emit_count++] = SUN_KEY_LSHIFT;
  emit_buf[emit_count++] = (unsigned char)code;
  emit_buf[emit_count++] = (unsigned char)(code | 0x80);
  if (shifted) emit_buf[emit_count++] = SUN_KEY_LSHIFT | 0x80;
  emit_idx = 0;

  printf("autotype: %s '%c' (scancode 0x%02x)\n",
         shifted ? "shift+press" : "press", ch, code);

  scc_in_push(3, emit_buf[emit_idx++]);
  if (emit_idx < emit_count) {
    autotype_delay = AUTOTYPE_PRESS_HOLD;
  } else {
    autotype_delay = AUTOTYPE_GAP;
    autotype_pos++;
  }
}

/* ----- */

void sun2_kb_write(int value, int size)
{
    /* Sun keyboard command protocol.  PROM writes a 1-byte command to the
       keyboard SCC data port; real keyboard responds with 0..N bytes via
       the SCC RX FIFO.  Bell on/off produce no SCC response on real hw
       (only drive the beeper); we hijack the FIRST bell-off as the
       auto-abort trigger, then become inert. */

    /* --no-kbd: stay silent on every keyboard command so the PROM's
       reset-and-wait-for-reply times out, declares "no keyboard",
       and switches its console to ttya (SCC channel 0). */
    if (g_no_kbd) return;

    switch (value) {
    case 0x01: /* RESET */
      scc_in_push(3, 0xff);   /* reset done */
      scc_in_push(3, 0x02);   /* layout id 0x02 = US English Type 4 */
      scc_in_push(3, 0x7f);   /* idle */
      break;
    case 0x02: /* BELL ON */
      break;
    case 0x03: /* BELL OFF */
      if (auto_abort_enabled && !auto_abort_done) {
        printf("kb: auto-abort (first bell-off) → L1-A burst\n");
        sun2_send_abort();
        auto_abort_done = 1;
      }
      break;
    }
}

unsigned int map_sdl_to_sun2kb[512];

void sun2_sdl_key(SDL_Keycode sdl_code, uint16_t modifiers, SDL_Scancode scancode, int down)
{
  unsigned int mapped;

  /* --no-kbd: the keyboard is "not attached" — discard SDL keystrokes
     so they don't show up on SCC channel 3.  Use --scc-tcp + a telnet
     client for input instead (lands on channel 0). */
  if (g_no_kbd) return;

  /* Set SUN2_KEY_TRACE=1 in the environment to print what SDL hands
     us on every key event.  Useful for figuring out which scancode +
     keycode a non-US-layout key produces. */
  {
    static int probed = 0, enabled = 0;
    if (!probed) {
      probed = 1;
      const char *e = getenv("SUN2_KEY_TRACE");
      enabled = (e && *e && *e != '0');
    }
    if (enabled)
      fprintf(stderr,
              "sun2-key: keycode=0x%x scancode=%u (%s) mod=0x%x down=%d\n",
              (unsigned)sdl_code, (unsigned)scancode,
              SDL_GetScancodeName(scancode),
              (unsigned)modifiers, down);
  }

  /* F12 → L1-A (Stop-A) abort burst, matching RetroCore
     SunKeyboardMapper.IsAbortKey().  Press once on key-down only. */
  if (scancode == SDL_SCANCODE_F12 || sdl_code == SDLK_F12) {
    if (down) {
      printf("kb: F12 → L1-A abort\n");
      sun2_send_abort();
    }
    return;
  }

  // If the keycode is over 128 use the scancode instead
  if(sdl_code >= 255)
	sdl_code = scancode;

  /* Numeric keypad: SDL keypad Keycodes are all > 255 (e.g.
     SDLK_KP_0 = 0x40000059), so the scancode-fallback path below
     would index a flat 512-int map and collide with SDLK_* values
     for main-keyboard keys (SDL_SCANCODE_KP_3 = 91 = SDLK_LEFTBRACKET).
     Handle keypad keys explicitly by scancode here.

     Sun-2 keyboard keypad layout (from SunOS keytables.c):
        KP digits 7/8/9 -> keypos 45/46/47
        KP digits 4/5/6 -> keypos 68/69/70
        KP digits 1/2/3 -> keypos 91/92/93
        KP digit  0     -> keypos 114
        KP '.'          -> keypos 116
        KP '+'          -> keypos 22
        KP '-'          -> keypos 23
     The Sun-2 keyboard has no separate keypad Enter, '/', or '*',
     so we alias KP_ENTER to the main Enter (pos 89) and KP_DIVIDE
     to the main '/' (pos 109).  KP_MULTIPLY has no clean mapping --
     the only '*' producing key on a Sun-2 is shift+':' (pos 87 with
     shift held); synthesising shift around a single keypress is
     fragile, so leave it unmapped (use shift+; on the main row to
     get '*'). */
  mapped = 0;
  switch (scancode) {
    case SDL_SCANCODE_KP_0:      mapped = 114; break;
    case SDL_SCANCODE_KP_1:      mapped = 91;  break;
    case SDL_SCANCODE_KP_2:      mapped = 92;  break;
    case SDL_SCANCODE_KP_3:      mapped = 93;  break;
    case SDL_SCANCODE_KP_4:      mapped = 68;  break;
    case SDL_SCANCODE_KP_5:      mapped = 69;  break;
    case SDL_SCANCODE_KP_6:      mapped = 70;  break;
    case SDL_SCANCODE_KP_7:      mapped = 45;  break;
    case SDL_SCANCODE_KP_8:      mapped = 46;  break;
    case SDL_SCANCODE_KP_9:      mapped = 47;  break;
    case SDL_SCANCODE_KP_PERIOD: mapped = 116; break;
    case SDL_SCANCODE_KP_PLUS:   mapped = 22;  break;
    case SDL_SCANCODE_KP_MINUS:  mapped = 23;  break;
    case SDL_SCANCODE_KP_ENTER:  mapped = 89;  break;  /* alias main Enter */
    case SDL_SCANCODE_KP_DIVIDE: mapped = 109; break;  /* alias main '/'   */
    /* Layout-independent fallbacks for keys whose Keycode varies by
       OS keyboard layout.  SDL_SCANCODE_* is the physical key position
       (US-keyboard reference), so on a Norwegian/German/... layout the
       physical "next to right-Shift" key still sends Sun '/' (pos 109)
       even though its Keycode might be SDLK_MINUS or whatever.  Add
       more keys here as we test other layouts. */
    case SDL_SCANCODE_SLASH:     mapped = 109; break;
    default: break;
  }

  if (mapped == 0) {
    // If the keycode is over 128 use the scancode instead
    if(sdl_code >= 255)
      sdl_code = scancode;

    // This should still work, just with slightly different values
    mapped = map_sdl_to_sun2kb[sdl_code];
  }
  if (0) printf("sdl: %u %u %u %u %d \n", sdl_code, modifiers, scancode, mapped, down);

  mapped &= 0xff;
  if (mapped == 0)
    return;

  /* Sun-2 modifier scancodes — these need real keydown/keyup tracking
     because SunOS reads the modifier state for combos. */
  int is_modifier = (mapped == 99   /* LSHIFT */ ||
                     mapped == 111  /* RSHIFT */ ||
                     mapped == 76   /* CTRL   */ ||
                     mapped == 119  /* CAPSLOCK */);

  if (is_modifier) {
    if (down == 0)
      mapped |= 0x80;
    scc_in_push(3, mapped);
  } else {
    /* Normal keys: send make+break atomically on keydown, ignore keyup.
       The emulated CPU runs fast relative to SDL event timing, so a
       separate break code from KEYUP arrives thousands of instructions
       too late and the PROM/SunOS re-reads stale data (= duplicate
       characters).  SunOS handles repeat in software. */
    if (down) {
      scc_in_push(3, mapped);          /* make code  */
      scc_in_push(3, mapped | 0x80);   /* break code */
    }
  }
}

#define m(f,t) map_sdl_to_sun2kb[(f)] = (t);

void sun2_init(void)
{
  init_ascii_map();

  m(SDLK_ESCAPE, 29);

  m(SDLK_1, 30);
  m(SDLK_2, 31);
  m(SDLK_3, 32);
  m(SDLK_4, 33);
  m(SDLK_5, 34);
  m(SDLK_6, 35);
  m(SDLK_7, 36);
  m(SDLK_8, 37);
  m(SDLK_9, 38);
  m(SDLK_0, 39);
  m(SDLK_MINUS, 40);
  m(SDLK_EQUALS, 41);
  m(SDLK_BACKQUOTE, 42);
  m(SDLK_BACKSPACE, 43);

  m(SDLK_TAB, 53);
  m(SDLK_q, 54);
  m(SDLK_w, 55);

  m(SDLK_e, 56);
  m(SDLK_r, 57);
  m(SDLK_t, 58);
  m(SDLK_y, 59);
  m(SDLK_u, 60);
  m(SDLK_i, 61);
  m(SDLK_o, 62);
  m(SDLK_p, 63);

  m(SDLK_LEFTBRACKET, 64);
  m(SDLK_RIGHTBRACKET, 65);

  m(SDLK_a, 77);
  m(SDLK_s, 78);
  m(SDLK_d, 79);

  m(SDLK_f, 80);
  m(SDLK_g, 81);
  m(SDLK_h, 82);
  m(SDLK_j, 83);
  m(SDLK_k, 84);
  m(SDLK_l, 85);
  m(SDLK_SEMICOLON, 86);
  m(SDLK_QUOTE, 87);

  m(SDLK_BACKSLASH, 88);
  m(SDLK_RETURN, 89);

  m(SDLK_z, 100);
  m(SDLK_x, 101);
  m(SDLK_c, 102);
  m(SDLK_v, 103);

  m(SDLK_b, 104);
  m(SDLK_n, 105);
  m(SDLK_m, 106);
  m(SDLK_COMMA, 107);
  m(SDLK_PERIOD, 108);
  m(SDLK_SLASH, 109);

  m(SDLK_SPACE, 121);

  m(SDL_SCANCODE_LCTRL, 76);
  m(SDL_SCANCODE_RCTRL, 76);
  m(SDL_SCANCODE_LSHIFT, 99);
  m(SDL_SCANCODE_RSHIFT,111);

//-----
//	SDLK_CLEAR		
//	SDLK_PAUSE		

//
//	SDLK_DELETE		
//
//	SDLK_UP
//	SDLK_DOWN		
//	SDLK_RIGHT		
//	SDLK_LEFT		
//	SDLK_INSERT		
//	SDLK_HOME		
//	SDLK_END		
//	SDLK_PAGEUP		
//	SDLK_PAGEDOWN		
//
//	SDLK_F1
//	SDLK_F2
//	SDLK_F3
//	SDLK_F4
//	SDLK_F5
//	SDLK_F6
//	SDLK_F7
//	SDLK_F8
//	SDLK_F9
//	SDLK_F10		
//	SDLK_F11		
//	SDLK_F12		
//	SDLK_F13		
//	SDLK_F14		
//	SDLK_F15		
//
//	SDLK_NUMLOCK		
//	SDLK_CAPSLOCK		
//	SDLK_SCROLLOCK		
//	SDLK_RSHIFT		
//	SDLK_LSHIFT		
//	SDLK_RCTRL		
//	SDLK_LCTRL		
//	SDLK_RALT		
//	SDLK_LALT		
//	SDLK_RMETA		
//	SDLK_LMETA		
//	SDLK_LSUPER		
//	SDLK_RSUPER		
//	SDLK_MODE		
//	SDLK_COMPOSE		
//
//	SDLK_HELP		
//	SDLK_PRINT		
//	SDLK_SYSREQ		
//	SDLK_BREAK		
//	SDLK_MENU		
//	SDLK_POWER		
//	SDLK_EURO		
//	SDLK_UNDO		

}

/* Local Variables:  */
/* mode: c           */
/* c-basic-offset: 2 */
/* End:              */
