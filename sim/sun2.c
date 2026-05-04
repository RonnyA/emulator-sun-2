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

#define debug 0

/* Sun-2 modifier scancodes */
#define SUN2_SC_LSHIFT  99
#define SUN2_SC_RSHIFT  111
#define SUN2_SC_CTRL    76
#define SUN2_SC_CAPSLOCK 119

/* Paste buffer for clipboard text injection */
static char *paste_buf = NULL;
static int paste_pos = 0;
static int paste_len = 0;
/* Delay counter - feed one char per N update cycles to avoid FIFO overflow */
static int paste_delay = 0;
#define PASTE_CHAR_DELAY 3

/* ASCII to Sun-2 scancode mapping: {scancode, needs_shift} */
struct ascii_to_sun2 {
  unsigned char sc;
  unsigned char shifted;
};

static struct ascii_to_sun2 ascii_map[128];

static void init_ascii_map(void)
{
  int i;
  for (i = 0; i < 128; i++) {
    ascii_map[i].sc = 0;
    ascii_map[i].shifted = 0;
  }

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
  char *clip;

  if (!SDL_HasClipboardText())
    return;

  clip = SDL_GetClipboardText();
  if (clip == NULL || clip[0] == '\0') {
    SDL_free(clip);
    return;
  }

  /* Free any previous paste in progress */
  if (paste_buf != NULL)
    free(paste_buf);

  paste_len = strlen(clip);
  paste_buf = malloc(paste_len + 1);
  memcpy(paste_buf, clip, paste_len + 1);
  paste_pos = 0;
  paste_delay = 0;
  SDL_free(clip);

  if (0) printf("paste: %d chars queued\n", paste_len);
}

/* Feed one character from paste buffer into SCC FIFO.
 * Called from sdl_poll on each update cycle. */
static void paste_feed(void)
{
  unsigned char ch, sc;

  if (paste_buf == NULL || paste_pos >= paste_len)
    return;

  /* Throttle: wait a few cycles between characters */
  if (paste_delay > 0) {
    paste_delay--;
    return;
  }

  ch = (unsigned char)paste_buf[paste_pos];

  /* Skip characters we can't map */
  if (ch >= 128 || ascii_map[ch].sc == 0) {
    paste_pos++;
    return;
  }

  sc = ascii_map[ch].sc;

  if (ascii_map[ch].shifted) {
    scc_in_push(3, SUN2_SC_LSHIFT);        /* shift make */
    scc_in_push(3, sc);                     /* key make */
    scc_in_push(3, sc | 0x80);             /* key break */
    scc_in_push(3, SUN2_SC_LSHIFT | 0x80); /* shift break */
  } else {
    scc_in_push(3, sc);                     /* key make */
    scc_in_push(3, sc | 0x80);             /* key break */
  }

  paste_pos++;
  paste_delay = PASTE_CHAR_DELAY;

  /* Done? */
  if (paste_pos >= paste_len) {
    free(paste_buf);
    paste_buf = NULL;
    paste_pos = 0;
    paste_len = 0;
  }
}

static unsigned char *fbmem;
//static SDL_Surface *screen;
static SDL_Window* screen;
static SDL_Surface* surface;
static SDL_Color COLOR_BLACK = {0,0,0,0};
static SDL_Color COLOR_WHITE = {255,255,255,255};


static int rows, cols;

static int toggle_trace;
static unsigned int fbctrl;

void sdl_clear(void)
{
  //unsigned char *p = screen->pixels;
  unsigned char *p = surface->pixels;

  int i, j;

  for (i = 0; i < cols; i++)
    for (j = 0; j < rows; j++) {
      *p = ~*p;
      p++;
    }
}

// Helper function to find and set the SDL2 pixel
void set_pixel(int offset,SDL_Color c) {
  // Get plane depth of surface
  int bpp = surface->format->BytesPerPixel;
  // Temp var
  int i=0;
  // Get pixel array
  uint8_t* pixels = (uint8_t*)surface-> pixels;
  // Loop for each of R/G/B/Alpha
  for(i=0;i<4;i++)
    pixels[offset*bpp + i ] = SDL_MapRGB(surface->format, c.r, c.g, c.b);

}

void sdl_write(unsigned int offset, int size, unsigned value)
{
  //  Lock surface before modifying, this is near instant if the surface does not need locking
  SDL_LockSurface(surface);

  int i, h, v;

  if (0) printf("sdl_write offset %x size %d <- %x\n", offset, size, value);

  // Multipy offset with bit plane depth
  offset *= 8;
  v = offset / cols;
  h = offset % cols;

  switch (size) {
  case 1:
    for (i = 0; i < 8; i++) {
      set_pixel(offset+i,(value & 0x80) ? COLOR_BLACK : COLOR_WHITE);
      //ps[offset + i] = (value & 0x80) ? COLOR_BLACK : COLOR_WHITE;
      value <<= 1;
    }
    break;
  case 2:
    //if (value == 0x0) { for (i = 0; i < 16; i++) ps[offset + i] = COLOR_WHITE; } else
    if (value == 0x0) { for (i = 0; i < 16; i++) set_pixel(offset+i,COLOR_WHITE); } else
    //if (value == 0xffff) { for (i = 0; i < 16; i++) ps[offset + i] = COLOR_BLACK; } else
    if (value == 0xffff) { for (i = 0; i < 16; i++) set_pixel(offset+i,COLOR_BLACK); } else
    for (i = 0; i < 16; i++) {
      set_pixel(offset+i,(value & 0x8000) ? COLOR_BLACK : COLOR_WHITE);
      //ps[offset + i] = (value & 0x8000) ? COLOR_BLACK : COLOR_WHITE;
      value <<= 1;
    }
    break;
  case 4:
    //if (value == 0x0) { for (i = 0; i < 32; i++) ps[offset + i] = COLOR_WHITE; } else
    if (value == 0x0) { for (i = 0; i < 32; i++) set_pixel(offset+i,COLOR_WHITE); } else
    //if (value == 0xffffffff) { for (i = 0; i < 32; i++) ps[offset + i] = COLOR_BLACK; } else
    if (value == 0xffffffff) { for (i = 0; i < 32; i++) set_pixel(offset+i,COLOR_BLACK); } else
    for (i = 0; i < 32; i++) {
      set_pixel(offset+i,(value & 0x80000000) ? COLOR_BLACK : COLOR_WHITE);
      //ps[offset + i] = (value & 0x80000000) ? COLOR_BLACK : COLOR_WHITE;
      value <<= 1;
    }
    break;

  // Unlock surface after modifying
  SDL_UnlockSurface(surface);
  }

// This is no longer needed
//#ifdef __linux__
//  accumulate_update(h, v, 32, 1);
  // for some reason this slows down mac os with SDL 1.2
//  SDL_UpdateRect(screen, h, v, size*8, 1);
//#endif
}

void sdl_init(void)
{
    int flags;

#if 0
    cols = 1152;
    rows = 900;
#else
    cols = 1024;
    rows = 1024;
#endif

    if (0) printf("Initialize display %dx%d\n", cols, rows);

    //flags = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE;
    // Init parachute is the default in SDL2 and no longer needed
    flags = SDL_INIT_VIDEO;

    if (SDL_Init(flags)) {
        printf("SDL initialization failed %s\n",SDL_GetError());
        return;
    }

    // SDL2 is now hardware accelerated by default so this isn't needed anymore
    // flags = SDL_HWSURFACE|SDL_ASYNCBLIT|SDL_HWACCEL;
    flags = SDL_WINDOW_SHOWN;

    //screen = SDL_SetVideoMode(cols, rows, 8, flags);
    screen = SDL_CreateWindow("My Game Window",
                          SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED,
                          cols,rows,
                          flags);

    surface = SDL_GetWindowSurface(screen);


    if (!screen) {
        printf("Could not open SDL display\n");
        return;
    }

    if(!surface) {
	printf("Can't init surface\n");
	return;
    }

    SDL_SetWindowTitle(screen,"Sun2");
}

//void sun2_sdl_key(int sdl_code, int modifiers, unsigned int unicode, int down);
void sun2_sdl_key(SDL_Keycode sdl_code, uint16_t modifiers, SDL_Scancode unicode, int down);

void sdl_poll(void)
{
  SDL_Event event;
  //SDL_Event ev1, *ev = &ev1;

  //  send_accumulated_updates();
  SDL_UpdateWindowSurface(screen);

  while (SDL_PollEvent(&event)) {
    switch (event.type) {
	    case SDL_WINDOWEVENT:
	      break;

	    case SDL_KEYDOWN:
	      if (event.key.repeat)
	        break; /* suppress SDL auto-repeat */
	      sun2_sdl_key(event.key.keysym.sym, event.key.keysym.mod, event.key.keysym.scancode, 1);
	      break;

	    case SDL_KEYUP:
	      sun2_sdl_key(event.key.keysym.sym, event.key.keysym.mod, event.key.keysym.scancode, 0);
	      break;

	    case SDL_TEXTINPUT:
	      /* Sun-2 keyboard uses make/break scancodes only, discard */
	      break;

	    case SDL_QUIT:
	      break;
	    case SDL_MOUSEMOTION:
	      break;
	    case SDL_MOUSEBUTTONDOWN:
	      /* Right-click pastes clipboard text */
	      if (event.button.button == SDL_BUTTON_RIGHT)
	        paste_start();
	      break;
	    case SDL_MOUSEBUTTONUP:
	      break;
	    }
  }

  /* Drip-feed paste buffer into SCC FIFO */
  paste_feed();
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
  sdl_write(address & 0xfffff, size, value);
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

unsigned int sun2_video_ctl_read(unsigned int address, int size)
{
  printf("sun2: fb ctrl @ %x -> %x (%d)\n", address, fbctrl, size);
//fbctrl = 0;
  return fbctrl;
}

unsigned int sun2_video_ctl_write(unsigned int address, int size, unsigned int value)
{
  printf("sun2: fb ctrl @ %x <- %x (%d)\n", address, value, size);
  fbctrl = value & 0xe07e;
  return 0;
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

static int is_modifier(unsigned int sc)
{
  return (sc == SUN2_SC_LSHIFT || sc == SUN2_SC_RSHIFT ||
          sc == SUN2_SC_CTRL || sc == SUN2_SC_CAPSLOCK);
}

void sun2_sdl_key(SDL_Keycode sdl_code, uint16_t modifiers, SDL_Scancode scancode, int down)
{
  unsigned int mapped;

  if (0) printf("sdl: %u %u %u %d\n", sdl_code, modifiers, scancode, down);

  /* If the keycode is over 128 use the scancode instead */
  if (sdl_code >= 255)
    sdl_code = scancode;

  mapped = map_sdl_to_sun2kb[sdl_code];
  if (0) printf("sdl: %u %u %u %u %d\n", sdl_code, modifiers, scancode, mapped, down);

  mapped &= 0xff;
  if (mapped == 0)
    return;

  if (is_modifier(mapped)) {
    /* Modifiers: real keydown/keyup tracking for combos */
    if (down == 0)
      mapped |= 0x80;
    scc_in_push(3, mapped);
  } else {
    /* Normal keys: send make+break atomically on keydown, ignore keyup.
     * The emulated CPU runs fast relative to SDL event timing, so a
     * separate break code from KEYUP arrives thousands of instructions
     * too late -- the PROM re-reads stale data. */
    if (down) {
      scc_in_push(3, mapped);        /* make code */
      scc_in_push(3, mapped | 0x80); /* break code */
    }
    /* ignore keyup for normal keys */
  }
}

#define m(f,t) map_sdl_to_sun2kb[(f)] = (t);
#define m_sh(f,t) map_sdl_to_sun2kb[(f)] = (t) | SHIFTED;

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
