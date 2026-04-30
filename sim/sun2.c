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

#ifdef __APPLE__
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#include "sim.h"

#define debug 0

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

    if (SDL_Init(SDL_INIT_VIDEO)) {
        printf("SDL initialization failed: %s\n", SDL_GetError());
        return;
    }

    screen = SDL_CreateWindow("Sun2",
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              win_w, win_h,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!screen) {
        printf("Could not open SDL window: %s\n", SDL_GetError());
        return;
    }

    renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(screen, -1, 0);
        if (!renderer) {
            printf("Could not create renderer: %s\n", SDL_GetError());
            return;
        }
    }

    /* Map the logical 1024x1024 framebuffer onto whatever window size,
       preserving aspect ratio and using nearest-neighbor for crisp pixels. */
    SDL_RenderSetLogicalSize(renderer, cols, rows);
    SDL_RenderSetIntegerScale(renderer, SDL_FALSE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    texture = SDL_CreateTexture(renderer,
                                SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING,
                                cols, rows);
    if (!texture) {
        printf("Could not create texture: %s\n", SDL_GetError());
        return;
    }

    SDL_RendererInfo info;
    SDL_GetRendererInfo(renderer, &info);
    printf("sdl_init: logical fb %dx%d, window %dx%d (resizable), renderer=%s\n",
           cols, rows, win_w, win_h, info.name);
}

//void sun2_sdl_key(int sdl_code, int modifiers, unsigned int unicode, int down);
void sun2_sdl_key(SDL_Keycode sdl_code, uint16_t modifiers, SDL_Scancode unicode, int down);

void sdl_poll(void)
{
  SDL_Event event;
  //SDL_Event ev1, *ev = &ev1;

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
	      sun2_sdl_key(event.key.keysym.sym, event.key.keysym.mod, event.key.keysym.scancode, 1);
	      break;

	    case SDL_KEYUP:
	      sun2_sdl_key(event.key.keysym.sym, event.key.keysym.mod, event.key.keysym.scancode, 0);
	      break;
	    case SDL_QUIT:
	      SDL_Quit();
	      exit(0);
	    case SDL_MOUSEMOTION:
	//      sdl_send_mouse_event();
	      break;
	    case SDL_MOUSEBUTTONDOWN:
	    case SDL_MOUSEBUTTONUP:
	    {
	      /*SDL_MouseButtonEvent *bev = &ev->button;*/
	//      sdl_send_mouse_event();
	    }
	    break;
	    }
  }
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

/* ---- auto-typer (drives the PROM monitor non-interactively for trace
   capture).  Uses the same SCC keyboard channel as physical keystrokes:
   pushes the press scancode, waits, then pushes the release scancode.
   Driven from io_update via sun2_autotype_tick(). */

extern unsigned int map_sdl_to_sun2kb[512];

static unsigned autotype_pos;
static unsigned autotype_phase;        /* 0 = press, 1 = release */
static unsigned autotype_delay;        /* tick countdown before next action */
#define AUTOTYPE_BOOT_DELAY  20000000  /* ~20M io_update ticks before first key — let PROM reach prompt */
#define AUTOTYPE_PRESS_HOLD  100000    /* hold each key down */
#define AUTOTYPE_GAP         200000    /* gap between successive keys */

void sun2_autotype_tick(void)
{
  static int started;
  if (!g_autotype) return;
  if (!started) { autotype_delay = AUTOTYPE_BOOT_DELAY; started = 1; }
  if (autotype_delay) { autotype_delay--; return; }
  if (!g_autotype[autotype_pos]) return;

  unsigned char ch = (unsigned char)g_autotype[autotype_pos];
  /* Map to SDL keycode the existing keymap expects.  Lower-case
     ASCII letters and digits go through directly (SDLK_a == 'a' etc.).
     Convert '\n' to SDLK_RETURN (0x0D). */
  if (ch == '\n') ch = '\r';     /* SDLK_RETURN = 0x0D */
  unsigned int code = map_sdl_to_sun2kb[ch] & 0xff;
  if (code == 0) {
    printf("autotype: skipping unmapped char 0x%02x\n", ch);
    autotype_pos++;
    autotype_delay = AUTOTYPE_GAP;
    return;
  }

  if (autotype_phase == 0) {
    if (debug || 1) printf("autotype: press '%c' (scancode 0x%02x)\n", ch, code);
    scc_in_push(3, code);
    autotype_phase = 1;
    autotype_delay = AUTOTYPE_PRESS_HOLD;
  } else {
    scc_in_push(3, code | 0x80);   /* release = bit 7 set */
    autotype_phase = 0;
    autotype_pos++;
    autotype_delay = AUTOTYPE_GAP;
  }
}

/* ----- */

void sun2_kb_write(int value, int size)
{
    switch (value) {
    case 0x01: /* reset */
      scc_in_push(3, 0xff);
      scc_in_push(3, 0x02);
      scc_in_push(3, 0x7f);
      break;
    case 0x02: /* bell on */
      break;
    case 0x03: /* bell off */
      /* send abort */
      scc_in_push(3, 0x00+1);
      scc_in_push(3, 0x00+77);
      scc_in_push(3, 0x80+77);
      scc_in_push(3, 0x80+1);

      scc_in_push(3, 0x7f);
      break;
    }
}

unsigned int map_sdl_to_sun2kb[512];

#define SHIFTED 0x10s

//void sun2_sdl_key(int sdl_code, int modifiers, unsigned int unicode, int down)
void sun2_sdl_key(SDL_Keycode sdl_code, uint16_t modifiers, SDL_Scancode scancode, int down)
{
  unsigned int mapped, shifted;

  if (0) printf("sdl: %u %u %u %d\n", sdl_code, modifiers, scancode, down);

  // If the keycode is over 128 use the scancode instead
  if(sdl_code >= 255)
	sdl_code = scancode;

  // This should still work, just with slightly different values
  mapped = map_sdl_to_sun2kb[sdl_code];
  if (0) printf("sdl: %u %u %u %u %d \n", sdl_code, modifiers, scancode, mapped, down);
//  shifted = mapped & SHIFTED;

  mapped &= 0xff;
  if (down == 0)
    mapped |= 0x80;

//  if (shifted) {
//    scc_in_push(3, 0x84);
//    scc_in_push(3, mapped);
//    scc_in_push(3, 0x84);
//    return;
//  }

#if 0
  if (sdl_code == SDLK_SLASH && down) {
    extern unsigned char g_ram[];
    unsigned char *p = g_ram;
    printf("0: %02x%02x%02x%02x\n", p[0], p[1], p[2], p[3]); p += 4;
    printf("4: %02x%02x%02x%02x\n", p[0], p[1], p[2], p[3]); p += 4;
    printf("8: %02x%02x%02x%02x\n", p[0], p[1], p[2], p[3]); p += 4;
    printf("c: %02x%02x%02x%02x\n", p[0], p[1], p[2], p[3]);
  }
#endif

#if 1
  if (sdl_code == SDLK_QUOTE && down) {
    extern int trace_armed;
    if (trace_armed == 0) {
      trace_armed = 1;
      printf("TRACE ARMED!\n");
    } else {
      trace_armed = 0;
      printf("TRACE unarmed!\n");
    }
  }
#endif

#if 0
  if (sdl_code == SDLK_SEMICOLON && down) {
    toggle_trace = !toggle_trace;
    if (toggle_trace) {
      printf("TRACE ENABLED!\n");
      enable_trace(1);
    } else {
      printf("TRACE DISABLED!\n");
      enable_trace(0);
    }
  }
#endif

  scc_in_push(3, mapped);
}

#define m(f,t) map_sdl_to_sun2kb[(f)] = (t);
#define m_sh(f,t) map_sdl_to_sun2kb[(f)] = (t) | SHIFTED;

void sun2_init(void)
{
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

#if 0
  /* shifted */
  m_sh(SDLK_EXCLAIM, 30);
  m_sh(SDLK_AT, 31);

  m_sh(SDLK_HASH, 32);
  m_sh(SDLK_DOLLAR, 33);
  m_sh('%', 34);
  m_sh(SDLK_CARET, 35);
  m_sh(SDLK_AMPERSAND, 36);
  m_sh(SDLK_ASTERISK, 37);
  m_sh(SDLK_LEFTPAREN, 38);
  m_sh(SDLK_RIGHTPAREN, 39);

  m_sh(SDLK_UNDERSCORE, 40);
  m_sh(SDLK_PLUS, 41);
  m_sh('~', 42);

  m_sh('{', 64);
  m_sh('}', 65);

  m_sh(SDLK_COLON, 86);
  m_sh(SDLK_QUOTEDBL, 87);
  m_sh('|', 88);

  m_sh(SDLK_LESS, 107);
  m_sh(SDLK_GREATER, 108);
  m_sh(SDLK_QUESTION, 109);
#endif

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
