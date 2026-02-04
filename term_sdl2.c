/* term_sdl2.c: SDL2 terminal backend for Xhomer
 
   This is an extension of the original Xhomer by
   Tarik Isani. 

   They are in no way involved with these derivative 
   works.

   -------------------------------------------------

   Xhomer is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2 
   as published by the Free Software Foundation.

   Xhomer is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Xhomer; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#ifdef PRO

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <termios.h>
#include <signal.h>
#include "debug_log.h"

#include "pro_defs.h"
#include "pro_lk201.h"

/* Forward declaration for menu function */
void pro_menu_top();

/* Forward declarations for overlay functions */
void pro_overlay_init(int psize, int cmode, int bpixel, int wpixel);
void pro_overlay_create_texture(SDL_Renderer *renderer);
void pro_overlay_render(SDL_Renderer *renderer);
void pro_overlay_close();

/* Forward declaration for terminal restoration */
void pro_screen_restore_terminal(void);

#define PRO_KEYBOARD_FIFO_DEPTH 1024

/* Globals similar to other backends */
int pro_nine_workaround = 0;
int pro_libc_workaround = 0;
int pro_window_x = 0;
int pro_window_y = 0;
int pro_screen_full = 0;
int pro_screen_window_scale = 2;
int pro_screen_full_scale = 3;
int pro_screen_gamma = 10;
int pro_screen_pcm = 1;
int pro_screen_framebuffers = 0;

int pro_sdl_opengl = 0;

int pro_screen_open = 0;
int pro_screen_winheight;
int pro_screen_bufheight;
int pro_screen_updateheight;
int pro_screen_pixsize;
int pro_screen_pixsize_act;
int pro_screen_clsize;
int pro_screen_clutmode;
int pro_screen_blank = 0;
unsigned char *pro_image_data = NULL; /* kept for compatibility */
uint8_t *pro_image_index = NULL; /* 1 byte per pixel buffer for RGB332 */
static uint8_t *pro_image_index_prev = NULL; /* Previous frame for change detection */
int pro_image_stride = 0;

/* keyboard FIFO */
int pro_keyboard_fifo_h = 0;
int pro_keyboard_fifo_t = 0;
int pro_keyboard_fifo[PRO_KEYBOARD_FIFO_DEPTH];

/* Mouse state (used by other modules) */
int pro_mouse_x = 0;
int pro_mouse_y = 0;
int pro_mouse_l = 0;
int pro_mouse_m = 0;
int pro_mouse_r = 0;
int pro_mouse_in = 0;

/* Keyboard/audio helpers (stubs) */
void pro_keyboard_bell_vol (int vol) { (void)vol; }
void pro_keyboard_bell () {}
void pro_keyboard_auto_off () {}
void pro_keyboard_auto_on () {}
void pro_keyboard_click_off () {}
void pro_keyboard_click_on () {}

/* Video/colormap stubs */
void pro_screen_reset () {}
void pro_mapchange () {}
void pro_colormap_write (int index, int rgb) { (void)index; (void)rgb; }
void pro_scroll () { pro_clear_mvalid(); }

/* SDL objects */
static SDL_Window *sdl_window = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Texture *sdl_texture = NULL;

/* Terminal state preservation */
static struct termios sdl_original_termios;
static int sdl_terminal_saved = 0;

/* Simple 8-color palette */
static const unsigned char palette[8][3] = {
  {0,0,0}, {255,0,0}, {0,255,0}, {255,255,0},
  {0,0,255}, {255,0,255}, {0,255,255}, {255,255,255}
};

/* precomputed mapping from 3-bit color index to packed RGB332 byte */
static uint8_t rgb332_map[8];

/* Current modifier key states */
static int sdl_lshift_down = 0;
static int sdl_rshift_down = 0;
static int sdl_lctrl_down = 0;
static int sdl_rctrl_down = 0;
static int sdl_lalt_down = 0;
static int sdl_ralt_down = 0;
static int sdl_lgui_down = 0;
static int sdl_rgui_down = 0;

/* Keyboard state tracking */
static int sdl_keyboard_state[32] = {0};
static int sdl_has_focus = 0;

/* Background status tracking */
static int sdl_is_background = 0;

/* Last update time for rate limiting */
static uint32_t sdl_last_update_time = 0;
static uint32_t sdl_last_poll_time = 0;

/* Put character in keycode FIFO */
static void pro_keyboard_fifo_put (int key)
{
  /* Filter keycodes for commands */

  /* This is to avoid changing the menu state while screen is closed */

  if (pro_screen_open)
    key = pro_menu(key);

  if (key != PRO_NOCHAR)
  {
    int next = pro_keyboard_fifo_h + 1;
    if (next == PRO_KEYBOARD_FIFO_DEPTH) next = 0;
    if (next != pro_keyboard_fifo_t)
    {
      pro_keyboard_fifo[pro_keyboard_fifo_h] = key;
      pro_keyboard_fifo_h = next;
    }
  }
}

/* Update keyboard state from SDL key event */
static void pro_keyboard_update_state(SDL_Keysym ks, int down)
{
  int keycode = ks.scancode;
  int index = keycode / 8;
  int bit = 1 << (keycode % 8);

  if (down) {
    sdl_keyboard_state[index] |= bit;
  } else {
    sdl_keyboard_state[index] &= ~bit;
  }
}

/* Minimal key mapping (partial) */
static int map_sdl_key(SDL_Keysym ks)
{
  int sym = ks.sym;
  
  switch(sym)
  {
    case SDLK_ESCAPE: return PRO_LK201_HOLD;
    case SDLK_F1:     return PRO_LK201_PRINT;
    case SDLK_F2:     return PRO_LK201_SETUP;
    case SDLK_F3:     return PRO_LK201_FFOUR;
    case SDLK_F4:     return PRO_LK201_BREAK;
    case SDLK_F5:     return PRO_LK201_INT;
    case SDLK_F6:     return PRO_LK201_RESUME;
    case SDLK_F7:     return PRO_LK201_CANCEL;
    case SDLK_F8:     return PRO_LK201_MAIN;
    case SDLK_F9:     return PRO_LK201_EXIT;
    case SDLK_F10:    return PRO_LK201_ESC;
    case SDLK_F11:    return PRO_LK201_BS;
    case SDLK_F12:    return PRO_LK201_LF;
    case SDLK_NUMLOCKCLEAR: return PRO_LK201_ADDOP;
    case SDLK_PAUSE:  return PRO_LK201_DO;
    case SDLK_PRINTSCREEN: return PRO_LK201_HELP;
    case SDLK_SCROLLLOCK: return PRO_LK201_LOCK;
    case SDLK_INSERT: return PRO_LK201_INSERT;
    case SDLK_DELETE: return PRO_LK201_REMOVE;
    case SDLK_HOME: return PRO_LK201_FIND;
    case SDLK_END: return PRO_LK201_PREV;
    case SDLK_PAGEUP: return PRO_LK201_SELECT;
    case SDLK_PAGEDOWN: return PRO_LK201_NEXT;
    
    case SDLK_SPACE:     return PRO_LK201_SPACE;
    case SDLK_BACKSPACE: return PRO_LK201_DEL;
    case SDLK_RETURN:    return PRO_LK201_RETURN;
    case SDLK_TAB:       return PRO_LK201_TAB;
    
    case SDLK_UP:    return PRO_LK201_UP;
    case SDLK_DOWN:  return PRO_LK201_DOWN;
    case SDLK_LEFT:  return PRO_LK201_LEFT;
    case SDLK_RIGHT: return PRO_LK201_RIGHT;
    
    case SDLK_KP_0: return PRO_LK201_0;
    case SDLK_KP_1: return PRO_LK201_1;
    case SDLK_KP_2: return PRO_LK201_2;
    case SDLK_KP_3: return PRO_LK201_3;
    case SDLK_KP_4: return PRO_LK201_4;
    case SDLK_KP_5: return PRO_LK201_5;
    case SDLK_KP_6: return PRO_LK201_6;
    case SDLK_KP_7: return PRO_LK201_7;
    case SDLK_KP_8: return PRO_LK201_8;
    case SDLK_KP_9: return PRO_LK201_9;
    case SDLK_KP_MULTIPLY: return PRO_LK201_PF3; /* Use PF3 as substitute for asterisk */
    case SDLK_KP_PLUS: return PRO_LK201_PF4; /* Use PF4 as substitute for plus */
    case SDLK_KP_MINUS: return PRO_LK201_MINUS;
    case SDLK_KP_PERIOD: return PRO_LK201_PERIOD;
    case SDLK_KP_DIVIDE: return PRO_LK201_SLASH;
    case SDLK_KP_ENTER: return PRO_LK201_RETURN;
    
    case SDLK_CAPSLOCK:     return PRO_LK201_LOCK;
    case SDLK_LSHIFT:       return PRO_LK201_SHIFT;
    case SDLK_RSHIFT:       return PRO_LK201_SHIFT;
    case SDLK_LCTRL:        return PRO_LK201_CTRL;
    case SDLK_RCTRL:        return PRO_LK201_CTRL;
    case SDLK_LALT:         return PRO_LK201_COMPOSE;
    case SDLK_RALT:         return PRO_LK201_COMPOSE;
    case SDLK_LGUI:         return PRO_LK201_COMPOSE;
    case SDLK_RGUI:         return PRO_LK201_COMPOSE;
    
    /* Punctuation */
    case SDLK_MINUS:   return PRO_LK201_MINUS;
    case SDLK_EQUALS:  return PRO_LK201_EQUAL;
    case SDLK_LEFTBRACKET:  return PRO_LK201_LEFTB;
    case SDLK_RIGHTBRACKET: return PRO_LK201_RIGHTB;
    case SDLK_SEMICOLON: return PRO_LK201_SEMI;
    case SDLK_QUOTE:     return PRO_LK201_QUOTE;
    case SDLK_BACKSLASH: return PRO_LK201_BACKSL;
    case SDLK_COMMA:     return PRO_LK201_COMMA;
    case SDLK_PERIOD:    return PRO_LK201_PERIOD;
    case SDLK_SLASH:     return PRO_LK201_SLASH;
    case SDLK_BACKQUOTE: return PRO_LK201_TICK;
    
    /* Letters a-z */
    case SDLK_a: return PRO_LK201_A;
    case SDLK_b: return PRO_LK201_B;
    case SDLK_c: return PRO_LK201_C;
    case SDLK_d: return PRO_LK201_D;
    case SDLK_e: return PRO_LK201_E;
    case SDLK_f: return PRO_LK201_F;
    case SDLK_g: return PRO_LK201_G;
    case SDLK_h: return PRO_LK201_H;
    case SDLK_i: return PRO_LK201_I;
    case SDLK_j: return PRO_LK201_J;
    case SDLK_k: return PRO_LK201_K;
    case SDLK_l: return PRO_LK201_L;
    case SDLK_m: return PRO_LK201_M;
    case SDLK_n: return PRO_LK201_N;
    case SDLK_o: return PRO_LK201_O;
    case SDLK_p: return PRO_LK201_P;
    case SDLK_q: return PRO_LK201_Q;
    case SDLK_r: return PRO_LK201_R;
    case SDLK_s: return PRO_LK201_S;
    case SDLK_t: return PRO_LK201_T;
    case SDLK_u: return PRO_LK201_U;
    case SDLK_v: return PRO_LK201_V;
    case SDLK_w: return PRO_LK201_W;
    case SDLK_x: return PRO_LK201_X;
    case SDLK_y: return PRO_LK201_Y;
    case SDLK_z: return PRO_LK201_Z;
    
    /* Numbers 0-9 */
    case SDLK_0: return PRO_LK201_0;
    case SDLK_1: return PRO_LK201_1;
    case SDLK_2: return PRO_LK201_2;
    case SDLK_3: return PRO_LK201_3;
    case SDLK_4: return PRO_LK201_4;
    case SDLK_5: return PRO_LK201_5;
    case SDLK_6: return PRO_LK201_6;
    case SDLK_7: return PRO_LK201_7;
    case SDLK_8: return PRO_LK201_8;
    case SDLK_9: return PRO_LK201_9;
    
    default: return PRO_NOCHAR;
  }
}

/* Bring emulator state up to date with keyboard state */
static void pro_keyboard_update_keys()
{
  for (int i = 0; i < 32 * 8; i++) {
    int oldkey = (sdl_keyboard_state[i/8] & (1 << (i % 8))) != 0;
    int curkey = SDL_GetKeyboardState(NULL)[i] != 0;

    if (oldkey != curkey) {
      SDL_Keysym ks;
      ks.scancode = i;
      ks.sym = SDL_GetKeyFromScancode(i);
      ks.mod = 0;
      ks.unused = 0;

      int key = map_sdl_key(ks);
      if ((key == PRO_LK201_SHIFT) || (key == PRO_LK201_CTRL) ||
          (key == PRO_LK201_ALLUPS)) {
        pro_keyboard_fifo_put(key);
      }
    }
  }
}

/* Get character from keyboard FIFO */
int pro_keyboard_get ()
{
  int key;
  if (pro_keyboard_fifo_h == pro_keyboard_fifo_t)
    key = PRO_NOCHAR;
  else
  {
    key = pro_keyboard_fifo[pro_keyboard_fifo_t];
    pro_keyboard_fifo_t++;
    if (pro_keyboard_fifo_t == PRO_KEYBOARD_FIFO_DEPTH) pro_keyboard_fifo_t = 0;
  }
  return key;
}

/* Poll SDL events and translate them */
void pro_screen_service_events ()
{
  /* xh_debug_log("pro_screen_service_events enter"); */
  SDL_Event ev;
  static int in_service = 0;
  if (in_service) return;
  in_service = 1;

  /* Drain all events to prevent queue buildup and freezes */
  while (SDL_PollEvent(&ev))
  {
    /* xh_debug_log("SDL Event: type=%d", ev.type); */

    if (ev.type == SDL_QUIT) {
      xh_debug_log("SDL_QUIT");
      exit(0);
    }
    else if (ev.type == SDL_KEYDOWN)
    {
      int ks_mapped = map_sdl_key(ev.key.keysym);

      /* Special handling for ctrl-F1 to trigger menu */
      int ctrl_down = (SDL_GetModState() & KMOD_CTRL) != 0;
      int alt_down = (SDL_GetModState() & KMOD_ALT) != 0;
      int shift_down = (SDL_GetModState() & KMOD_SHIFT) != 0;

      /* Improved ctrl-F1 detection with better modifier handling */
      if (ev.key.keysym.sym == SDLK_F1 && ctrl_down && !alt_down && !shift_down) {
        /* Rate limit menu trigger from key event to avoid rapid toggling */
        static uint32_t last_menu_trigger_ev = 0;
        uint32_t now = SDL_GetTicks();
        if (!pro_menu_on && (now - last_menu_trigger_ev > 500)) {
            xh_debug_log("Ctrl-F1 detected, initializing menu system");
            pro_menu_reset();
            /* Emulate Ctrl-Print to trigger the menu inside pro_menu() filters */
            pro_keyboard_fifo_put(PRO_LK201_CTRL);
            pro_keyboard_fifo_put(PRO_LK201_PRINT);
            pro_keyboard_fifo_put(PRO_LK201_ALLUPS);
            last_menu_trigger_ev = now;
        }
        /* Mark this event as handled to prevent further processing */
        ks_mapped = PRO_NOCHAR;
      }
      
      if (ks_mapped != PRO_NOCHAR) {
        pro_keyboard_fifo_put(ks_mapped);
      }

      /* Update modifier state */
      switch(ev.key.keysym.sym) {
        case SDLK_LSHIFT: sdl_lshift_down = 1; break;
        case SDLK_RSHIFT: sdl_rshift_down = 1; break;
        case SDLK_LCTRL:  sdl_lctrl_down = 1; break;
        case SDLK_RCTRL:  sdl_rctrl_down = 1; break;
        case SDLK_LALT:   sdl_lalt_down = 1; break;
        case SDLK_RALT:   sdl_ralt_down = 1; break;
        case SDLK_LGUI:   sdl_lalt_down = 1; break;
        case SDLK_RGUI:   sdl_ralt_down = 1; break;
      }

      /* Update keyboard state */
      pro_keyboard_update_state(ev.key.keysym, 1);
    }
    else if (ev.type == SDL_KEYUP)
    {
      int k = map_sdl_key(ev.key.keysym);
      (void)k;
      /* xh_debug_log("KEYUP sym=%d scancode=%d mapped=%d", ev.key.keysym.sym, ev.key.keysym.scancode, k); */

      /* Send ALLUPS on modifier key release */
      if (ev.key.keysym.sym == SDLK_LSHIFT || ev.key.keysym.sym == SDLK_RSHIFT ||
          ev.key.keysym.sym == SDLK_LCTRL || ev.key.keysym.sym == SDLK_RCTRL ||
          ev.key.keysym.sym == SDLK_LALT || ev.key.keysym.sym == SDLK_RALT ||
          ev.key.keysym.sym == SDLK_LGUI || ev.key.keysym.sym == SDLK_RGUI) {
        pro_keyboard_fifo_put(PRO_LK201_ALLUPS);
      }

      /* Update modifier state */
      switch(ev.key.keysym.sym) {
        case SDLK_LSHIFT: sdl_lshift_down = 0; break;
        case SDLK_RSHIFT: sdl_rshift_down = 0; break;
        case SDLK_LCTRL:  sdl_lctrl_down = 0; break;
        case SDLK_RCTRL:  sdl_rctrl_down = 0; break;
        case SDLK_LALT:   sdl_lalt_down = 0; break;
        case SDLK_RALT:   sdl_ralt_down = 0; break;
        case SDLK_LGUI:   sdl_lalt_down = 0; break;
        case SDLK_RGUI:   sdl_ralt_down = 0; break;
      }

      /* Update keyboard state */
      pro_keyboard_update_state(ev.key.keysym, 0);
    }
    else if (ev.type == SDL_WINDOWEVENT) {
      xh_debug_log("WINDOWEVENT: %d", ev.window.event);
      if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
        sdl_has_focus = 1;
        sdl_is_background = 0;
        xh_debug_log("Window focus gained");
        pro_keyboard_update_keys();
      } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        sdl_has_focus = 0;
        xh_debug_log("Window focus lost");
        memset(sdl_keyboard_state, 0, sizeof(sdl_keyboard_state));
        pro_keyboard_fifo_put(PRO_LK201_ALLUPS);
      } else if (ev.window.event == SDL_WINDOWEVENT_MINIMIZED || ev.window.event == SDL_WINDOWEVENT_HIDDEN) {
        sdl_is_background = 1;
        xh_debug_log("Window minimized/hidden");
      } else if (ev.window.event == SDL_WINDOWEVENT_RESTORED || ev.window.event == SDL_WINDOWEVENT_SHOWN) {
        sdl_is_background = 0;
        xh_debug_log("Window restored/shown");
      }
    }
    else {
      /* xh_debug_log("Unhandled SDL event type: %d", ev.type); */
    }
  }

  in_service = 0;

  /* Check if window has focus and keyboard input is enabled */
  if (sdl_has_focus) {
    int num_keys;
    const Uint8 *keyboard_state = SDL_GetKeyboardState(&num_keys);
    if (keyboard_state) {
      /* Check for ctrl-F1 combination directly from keyboard state */
      if (keyboard_state[SDL_SCANCODE_LCTRL] || keyboard_state[SDL_SCANCODE_RCTRL]) {
        if (keyboard_state[SDL_SCANCODE_F1]) {
          /* Rate limit menu trigger from keyboard state to avoid rapid toggling */
          static uint32_t last_menu_trigger = 0;
          uint32_t now = SDL_GetTicks();
          if (!pro_menu_on && (now - last_menu_trigger > 500)) {
              xh_debug_log("Direct Ctrl-F1 detection from keyboard state");
              pro_menu_reset();
              pro_menu_top();
              pro_keyboard_fifo_put(PRO_LK201_PRINT);
              last_menu_trigger = now;
          }
        }
      }
    }
  }

  /* xh_debug_log("pro_screen_service_events exit"); */
}

/* Call this frequently to service events without blocking */
void pro_screen_poll_events ()
{
  pro_screen_service_events();
}

static void pro_screen_check_events()
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) exit(0);
        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE) exit(0);
    }
}

/* Texture cache to reduce allocations */
static uint8_t *rgb24_cache = NULL;
static int rgb24_cache_size = 0;

/* Initialize the display */
int pro_screen_init ()
{
  int w = PRO_VID_SCRWIDTH;
  int h = PRO_VID_SCRHEIGHT;

  if (pro_screen_open) return PRO_SUCCESS;

  /* Handle fullscreen mode for SDL2 */
  Uint32 window_flags = 0;
  if (pro_screen_full) {
    pro_screen_winheight = pro_screen_full_scale * pro_screen_bufheight;
    window_flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
  } else {
    pro_screen_winheight = pro_screen_window_scale * pro_screen_bufheight;
  }

  /* Save terminal state and set up clean logging */
  static int terminal_setup_done = 0;
  if (!terminal_setup_done)
  {
    /* Ignore background terminal signals to prevent SIGTTOU stops */
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);

    /* Save original terminal settings */
    struct termios original_termios;
    if (tcgetattr(STDIN_FILENO, &original_termios) == 0) {
      /* Set up atexit handler to restore terminal */
      atexit(pro_screen_restore_terminal);
      memcpy(&sdl_original_termios, &original_termios, sizeof(struct termios));
    }

    /* Set up clean logging that doesn't corrupt terminal */
    const char *logfile = getenv("SDL2_LOGFILE");
    if (logfile) {
      int fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0600);
      if (fd >= 0) {
        /* Only redirect stderr to log file, keep stdout for normal operation */
        fflush(stderr);
        dup2(fd, STDERR_FILENO);
        close(fd);
      }
    }

    terminal_setup_done = 1;
  }

  if (xh_debug_enabled) {
    /* Enable SDL2 debug logging */
    SDL_SetHint(SDL_HINT_EVENT_LOGGING, "1");
    /* SDL_HINT_RENDER_LOG_ERRORS and SDL_HINT_VIDEO_LOG_ERRORS are not available in all SDL2 versions */
    /* These hints are optional and don't affect core functionality */
  } else {
    SDL_SetHint(SDL_HINT_EVENT_LOGGING, "0");
  }
  
  /* Select renderer based on state */
  if (pro_sdl_opengl) {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
  } else {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
  }
  
  xh_debug_log("pro_screen_init: SDL2 renderer set to %s", pro_sdl_opengl ? "opengl" : "software");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
  {
    xh_debug_log("SDL_Init failed: %s", SDL_GetError());
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return PRO_FAIL;
  }

  pro_screen_bufheight = PRO_VID_SCRHEIGHT;
  pro_screen_updateheight = PRO_VID_SCRHEIGHT;

  /* Initialize menu position */
  if (pro_screen_full) {
      /* Center menu in fullscreen (assume 80x24 grid, menu is ~38 wide, 18 high) */
      pro_menu_x = (80 - 38) / 2;
      pro_menu_y = (24 - 18) / 2;
  } else {
      pro_menu_x = 24;
      pro_menu_y = 2;
  }

  /* Initialize window height properly based on screen mode */
  if (pro_screen_full) {
    pro_screen_winheight = pro_screen_full_scale * pro_screen_bufheight;
  } else {
    pro_screen_winheight = pro_screen_window_scale * pro_screen_bufheight;
  }

  xh_debug_log("Screen setup: full=%d, bufheight=%d, winheight=%d, scale=%d",
               pro_screen_full, pro_screen_bufheight, pro_screen_winheight,
               pro_screen_full ? pro_screen_full_scale : pro_screen_window_scale);

  pro_screen_pixsize = 3;
  pro_screen_pixsize_act = 3;
  pro_image_stride = 0;

  /* allocate 1-byte-per-pixel index buffer */
  pro_image_index = malloc(w * pro_screen_bufheight);
  if (!pro_image_index) return PRO_FAIL;
  
  /* allocate previous frame buffer */
  if (pro_image_index_prev) free(pro_image_index_prev);
  pro_image_index_prev = calloc(w * pro_screen_bufheight, 1);
  
  pro_image_data = NULL;
  pro_image_stride = w;

  /* prepare rgb332 map from palette */
  for (int i = 0; i < 8; i++) {
    uint8_t r = palette[i][0];
    uint8_t g = palette[i][1];
    uint8_t b = palette[i][2];
    uint8_t r3 = (r >> 5) & 0x07;
    uint8_t g3 = (g >> 5) & 0x07;
    uint8_t b2 = (b >> 6) & 0x03;
    rgb332_map[i] = (r3 << 5) | (g3 << 2) | b2;
  }

  /* Calculate proper window dimensions with minimum size enforcement */
  int window_width = w;
  int window_height = pro_screen_winheight;

  /* Ensure window has reasonable minimum dimensions */
  if (window_width < 640) window_width = 640;
  if (window_height < 480) window_height = 480;

  xh_debug_log("Creating window: %dx%d (texture: %dx%d)",
               window_width, window_height, w, pro_screen_bufheight);

  sdl_window = SDL_CreateWindow("Pro 350",
                               SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                               window_width, window_height,
                               window_flags | SDL_WINDOW_SHOWN);
  if (!sdl_window) {
    xh_debug_log("SDL_CreateWindow failed: %s", SDL_GetError());
    free(pro_image_index); SDL_Quit(); return PRO_FAIL;
  }

  /* Configure window for better display */
  SDL_SetWindowMinimumSize(sdl_window, window_width, window_height);
  SDL_SetWindowResizable(sdl_window, SDL_FALSE);

  /* Show window immediately */
  SDL_ShowWindow(sdl_window);
  SDL_RaiseWindow(sdl_window);

  /* Try to get a renderer */
  sdl_renderer = SDL_CreateRenderer(sdl_window, -1, 0);
  if (!sdl_renderer) {
    xh_debug_log("SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_DestroyWindow(sdl_window); free(pro_image_index); SDL_Quit(); return PRO_FAIL;
  }

  /* Set window to grab keyboard input and ensure it has focus */
  SDL_SetWindowInputFocus(sdl_window);
  SDL_SetWindowKeyboardGrab(sdl_window, SDL_TRUE);

  /* Check if window has input focus */
  Uint32 current_window_flags = SDL_GetWindowFlags(sdl_window);
  xh_debug_log("Window flags after creation: %08X", current_window_flags);
  xh_debug_log("Window has input focus: %d", (current_window_flags & SDL_WINDOW_INPUT_FOCUS) != 0);
  xh_debug_log("Window has keyboard grab: %d", SDL_GetWindowKeyboardGrab(sdl_window));

  /* Try multiple texture formats for best compatibility */
  Uint32 texture_format = SDL_PIXELFORMAT_RGB24;
  int texture_access = SDL_TEXTUREACCESS_STREAMING;

  /* Try to create texture with different formats */
  sdl_texture = SDL_CreateTexture(sdl_renderer, texture_format, texture_access, w, h);
  if (!sdl_texture) {
    xh_debug_log("SDL_CreateTexture RGB24 failed: %s, trying ARGB8888", SDL_GetError());
    sdl_texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888, texture_access, w, h);
    if (!sdl_texture) {
      xh_debug_log("SDL_CreateTexture ARGB8888 failed: %s, trying RGB888", SDL_GetError());
      sdl_texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_RGB888, texture_access, w, h);
      if (!sdl_texture) {
        xh_debug_log("SDL_CreateTexture RGB888 failed: %s, trying RGB332", SDL_GetError());
        sdl_texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_RGB332, texture_access, w, h);
        if (!sdl_texture) {
          SDL_DestroyRenderer(sdl_renderer); SDL_DestroyWindow(sdl_window); free(pro_image_index); SDL_Quit(); return PRO_FAIL;
        }
      }
    }
  }

  /* Configure texture for best rendering quality */
  if (SDL_SetTextureBlendMode(sdl_texture, SDL_BLENDMODE_NONE) != 0) {
    xh_debug_log("SDL_SetTextureBlendMode failed: %s", SDL_GetError());
  }
  if (SDL_SetTextureScaleMode(sdl_texture, SDL_ScaleModeNearest) != 0) {
    xh_debug_log("SDL_SetTextureScaleMode failed: %s", SDL_GetError());
  }

  /* Initialize overlay system */
  pro_overlay_init(pro_screen_pixsize, pro_screen_clutmode, 0, 0xFFFFFF);
  pro_overlay_create_texture(sdl_renderer);

  xh_debug_log("pro_screen_init success w=%d h=%d", w, h);
  pro_screen_open = 1;
  
  /* Reset update timers to prevent large delta on restart */
  sdl_last_update_time = SDL_GetTicks();
  sdl_last_poll_time = sdl_last_update_time;
  
  return PRO_SUCCESS;
}

/* Close display */
void pro_screen_close ()
{
  if (!pro_screen_open) return;
  if (sdl_texture) SDL_DestroyTexture(sdl_texture);
  if (sdl_renderer) SDL_DestroyRenderer(sdl_renderer);
  if (sdl_window) SDL_DestroyWindow(sdl_window);
  if (pro_image_data) free(pro_image_data);
  if (pro_image_index) free(pro_image_index);
  if (pro_image_index_prev) free(pro_image_index_prev);
  if (rgb24_cache) free(rgb24_cache);
  sdl_texture = NULL;
  pro_image_index_prev = NULL;
  sdl_renderer = NULL;
  sdl_window = NULL;
  pro_image_index = NULL;
  rgb24_cache = NULL;
  rgb24_cache_size = 0;
  xh_debug_log("pro_screen_close");
  SDL_Quit();
  pro_screen_open = 0;
}

/* Clear the screen (fill black) */
void pro_screen_clear ()
{
  if (pro_image_index) {
    memset(pro_image_index, 0, PRO_VID_SCRWIDTH * pro_screen_bufheight);
    xh_debug_log("pro_screen_clear");
  }
  if (sdl_renderer) { SDL_SetRenderDrawColor(sdl_renderer,0,0,0,255); SDL_RenderClear(sdl_renderer); SDL_RenderPresent(sdl_renderer); }
}

/* Set window title */
void pro_screen_title (char *title)
{
  if (sdl_window) SDL_SetWindowTitle(sdl_window, title);
}

/* Update display: blit VRAM to SDL texture */
void pro_screen_update ()
{
  int w = PRO_VID_SCRWIDTH;
  int h = pro_screen_updateheight;
  if (!pro_image_index || !pro_screen_open) return;

  /* Limit framerate to ~60 FPS to prevent CPU churn and potential freezes */
  uint32_t current_time = SDL_GetTicks();

  /* Ensure events are polled at least every 16ms (60Hz), independent of rendering throttling */
  if (current_time - sdl_last_poll_time >= 16) {
      pro_screen_poll_events();
      sdl_last_poll_time = current_time;
  }
  
  /* If running in background/minimized, slow down significantly */
  if (sdl_is_background || !sdl_has_focus) {
      if (current_time - sdl_last_update_time < 100) {
          SDL_Delay(50);
          /* Ensure we poll if we sleep for a long time */
          if (current_time - sdl_last_poll_time >= 16) {
             pro_screen_poll_events();
             sdl_last_poll_time = current_time;
          }
          return;
      }
  } else {
      if (current_time - sdl_last_update_time < 16) {
          /* Small delay to prevent CPU spinning when simulator is running fast */
          SDL_Delay(1);
          return;
      }
  }
  sdl_last_update_time = current_time;

  /* If polling events closed the screen, stop here */
  if (!pro_screen_open || !sdl_renderer || !sdl_window) return;

  /* Convert VRAM to 1-byte-per-pixel packed RGB332 buffer.
     Process 16 pixels at a time using the VRAM word to reduce overhead. */
  for (int y = 0; y < h; y++) {
    int base = y * w;
    for (int vcol = 0; vcol < (w >> 4); vcol++) {
      int vindex = vmem((y << 6) | (vcol & 077));
      unsigned int w0 = PRO_VRAM[0][vindex];
      unsigned int w1 = PRO_VRAM[1][vindex];
      unsigned int w2 = PRO_VRAM[2][vindex];
      int out = base + (vcol << 4);
      for (int b = 0; b < 16; b++) {
        int bit0 = (w0 >> b) & 1;
        int bit1 = (w1 >> b) & 1; 
        int bit2 = (w2 >> b) & 1; 
        int idx = bit0 | (bit1 << 1) | (bit2 << 2);
        pro_image_index[out + b] = rgb332_map[idx];
      }
    }
  }

  /* Check if screen changed to avoid unnecessary texture uploads */
  int screen_changed = 1;
  if (pro_image_index_prev) {
      if (memcmp(pro_image_index, pro_image_index_prev, w * h) == 0) {
          screen_changed = 0;
      } else {
          memcpy(pro_image_index_prev, pro_image_index, w * h);
      }
  }

  /* upload texture (3 bytes per pixel RGB24) only if changed */
  if (sdl_texture && screen_changed) {
    int size = w * h * 3;
    if (size > rgb24_cache_size) {
        if (rgb24_cache) free(rgb24_cache);
        rgb24_cache = malloc(size);
        rgb24_cache_size = size;
    }
    
    if (rgb24_cache) {
        /* Use a lookup table for faster conversion if needed, but for now just optimize the loop */
        uint8_t *dst = rgb24_cache;
        uint8_t *src = pro_image_index;
        int count = w * h;
        while (count--) {
          uint8_t rgb332 = *src++;
          *dst++ = (uint8_t)(((rgb332 >> 5) & 0x07) * 255 / 7);
          *dst++ = (uint8_t)(((rgb332 >> 2) & 0x07) * 255 / 7);
          *dst++ = (uint8_t)((rgb332 & 0x03) * 255 / 3);
        }
        SDL_UpdateTexture(sdl_texture, NULL, rgb24_cache, w * 3);
    } else {
        /* Fallback if malloc fails */
        SDL_UpdateTexture(sdl_texture, NULL, pro_image_index, w * 1);
    }
  } else if (sdl_texture && screen_changed) {
    /* Fallback to RGB332 if RGB24 conversion fails or no texture */
    SDL_UpdateTexture(sdl_texture, NULL, pro_image_index, w * 1);
  }

  /* Clear renderer and render texture */
  /* SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255); */
  /* SDL_RenderClear(sdl_renderer); */

  /* Get window dimensions for proper scaling */
  int window_w, window_h;
  if (!sdl_window) return;
  SDL_GetWindowSize(sdl_window, &window_w, &window_h);

  /* Set texture scaling for proper display - scale to fill window */
  SDL_Rect dst = {0, 0, window_w, window_h};

  /* Debug window and texture dimensions */
  /* xh_debug_log("Window: %dx%d, Texture: %dx%d", window_w, window_h, w, h); */

  /* Clear renderer with black background */
  if (sdl_renderer) SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);

  /* Clear the entire window */
  if (sdl_renderer) SDL_RenderClear(sdl_renderer);

  /* Copy texture to renderer, scaling to window size */
  if (sdl_renderer && sdl_texture) {
      SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, &dst);
  }

  /* Update window title with debug info */
  /* char title[128]; */
  /* snprintf(title, sizeof(title), "Pro 350 - SDL2 (%dx%d)", window_w, window_h); */
  /* SDL_SetWindowTitle(sdl_window, title); */

  /* Render overlay if enabled */
  if (sdl_renderer) pro_overlay_render(sdl_renderer);

  if (sdl_renderer) SDL_RenderPresent(sdl_renderer);

  /* xh_debug_log("pro_screen_update rendered"); */
  /* xh_debug_log("pro_screen_update exit"); */
}

/* Keyboard / terminal stubs for compatibility */
void pro_screen_save_old_keyboard () {}
void pro_screen_restore_old_keyboard () {}
void pro_screen_restore_keyboard () {}

/* Terminal restoration function */
void pro_screen_restore_terminal(void)
{
  if (sdl_terminal_saved) {
    tcsetattr(STDIN_FILENO, TCSANOW, &sdl_original_termios);
    sdl_terminal_saved = 0;
  }
}

#endif /* PRO */
