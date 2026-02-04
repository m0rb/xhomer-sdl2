/* term_overlay_sdl2.c: SDL2 overlay frame buffer code

   Copyright (c) 1997-2003, Tarik Isani (xhomer@isani.org)

   This file is part of Xhomer.

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
#include "debug_log.h"
#include "pro_defs.h"
#include "pro_font.h"

int			pro_overlay_on = 0;
unsigned char		*pro_overlay_data;	/* overlay frame buffer */
unsigned char		*pro_overlay_alpha;	/* overlay frame buffer alpha */

static int		pro_overlay_dirty = 1;

LOCAL int		pro_overlay_open = 0;
LOCAL int		clutmode;
LOCAL int		pixsize;
LOCAL int		blackpixel;
LOCAL int		whitepixel;
LOCAL int		overlay_a;
LOCAL int		start_x;
LOCAL int		last_x;
LOCAL int		last_y;
LOCAL int		last_size;

/* SDL overlay texture */
static SDL_Texture *sdl_overlay_texture = NULL;

/* Print a single character into overlay frame buffer */

/* x = 0..79  y= 0..23 */

/* xnor = 0 -> replace mode
   xnor = 1 -> xnor mode */

/* prints 12x10 characters */

LOCAL void pro_overlay_print_char(int x, int y, int xnor, int font, char ch)
{
int	sx, sy, sx0, sy0; /* screen coordinates */
int	i, pix, pnum, opix, vindex;
int	charint;

	charint = ((int)ch) - PRO_FONT_FIRSTCHAR;
	if ((charint < 0) || (charint>(PRO_FONT_NUMCHARS-1)))
	  charint = 32 - PRO_FONT_FIRSTCHAR;

	sx0 = x * 12;
	sy0 = y * 10;

	/* Render character */

	for(y=0; y<10; y++)
	{
	  sy = sy0 + y;

	  for(x=0; x<12; x++)
	  {
	    sx = sx0 + x;

	    /* Set color */

	    if (((pro_overlay_font[font][charint][y] >> (11-x)) & 01) == 0)
	      pix = blackpixel;
	    else
	      pix = whitepixel;

	    /* Plot pixel */

	    pnum = sy*PRO_VID_SCRWIDTH+sx;

	    /* Perform XNOR, if required */

	    if (xnor == 1)
	    {
	      /* Get old pixel value */

	      opix = 0;

	      for(i=0; i<pixsize; i++)
	        opix |= pro_overlay_data[pixsize*pnum+i] << (i*8);

	      if (opix == blackpixel)
	      {
	        if (pix == blackpixel)
	          pix = whitepixel;
	        else
	          pix = blackpixel;
	      }
	    }

	    /* Assign alpha based on color */

	    if (pix == blackpixel)
	      pro_overlay_alpha[sy*PRO_VID_SCRWIDTH+sx] = overlay_a;
	    else
	      /* Make white pixels non-transparent */
	      pro_overlay_alpha[sy*PRO_VID_SCRWIDTH+sx] = PRO_OVERLAY_MAXA;

	    /* Write pixel into frame buffer */

	    for(i=0; i<pixsize; i++)
	      pro_overlay_data[pixsize*pnum+i] = pix >> (i*8);

	    /* Mark display cache entry invalid */

	    vindex = vmem((sy<<6) | ((sx>>4)&077));
	    pro_vid_mvalid[cmem(vindex)] = 0;
	  }
	}
	pro_overlay_dirty = 1;
}

/* Print text string into overlay frame buffer */

void pro_overlay_print(int x, int y, int xnor, int font, char *text)
{
int	i, size;

	if (pro_overlay_open)
	{
	  if (x == -1)
	    x = start_x;
	  else if (x == -2)
	    x = last_x + last_size;
	  else
	    start_x = x;

	  if (y == -1)
	    y = last_y + 1;
	  else if (y == -2)
	    y = last_y;

	  if (y > 23)
	    y = 23;

	  size = strlen(text);

	  for(i=0; i<size; i++)
	    pro_overlay_print_char(x+i, y, xnor, font, text[i]);

	  last_x = x;
	  last_y = y;
	  last_size = size;
	}
}

/* Clear the overlay frame buffer */

void pro_overlay_clear ()
{
int	i, j;

	if (pro_overlay_open)
	{
	  for(i=0; i<PRO_VID_SCRWIDTH*PRO_VID_MEMHEIGHT; i++)
	  {
	    for(j=0; j<pixsize; j++)
	      pro_overlay_data[pixsize*i+j] = blackpixel >> (j*8);

	    pro_overlay_alpha[i] = 0;
	  }
	  pro_overlay_dirty = 1;
	}
}

/* Turn on overlay */

void pro_overlay_enable ()
{
	pro_overlay_clear();
	pro_overlay_on = 1;
}

/* Turn off overlay */

void pro_overlay_disable ()
{
	pro_clear_mvalid();
	pro_overlay_on = 0;
}

/* Initialize the overlay frame buffer */

void pro_overlay_init (int psize, int cmode, int bpixel, int wpixel)
{
	if (pro_overlay_open == 0)
	{
	  start_x = 0;
	  last_x = 0;
	  last_y = 0;
	  last_size = 0;

	  clutmode = cmode;
	  pixsize = psize;
	  blackpixel = bpixel;
	  whitepixel = wpixel;

	  /* No blending is done in 8-bit modes */

	  if (cmode == 1)
	    overlay_a = PRO_OVERLAY_MAXA;
	  else
	    overlay_a = PRO_OVERLAY_A;

	  pro_overlay_data = (char *)malloc(PRO_VID_SCRWIDTH*PRO_VID_MEMHEIGHT*pixsize);
	  pro_overlay_alpha = (char *)malloc(PRO_VID_SCRWIDTH*PRO_VID_MEMHEIGHT);
	  pro_overlay_on = 0;
	  pro_overlay_open = 1;
	  pro_overlay_dirty = 1;

	  xh_debug_log("pro_overlay_init: SDL2 overlay initialized");
	}
}

/* Close the overlay frame buffer */

void pro_overlay_close ()
{
	if (pro_overlay_open)
	{
	  free(pro_overlay_data);
	  free(pro_overlay_alpha);
	  pro_overlay_on = 0;
	  pro_overlay_open = 0;

	  /* Destroy SDL overlay texture if it exists */
	  if (sdl_overlay_texture) {
	    SDL_DestroyTexture(sdl_overlay_texture);
	    sdl_overlay_texture = NULL;
	  }

	  xh_debug_log("pro_overlay_close: SDL2 overlay closed");
	}
}

/* Create SDL overlay texture for rendering */
void pro_overlay_create_texture(SDL_Renderer *renderer)
{
  if (!pro_overlay_open || !renderer) return;

  /* Destroy existing texture if it exists */
  if (sdl_overlay_texture) {
    SDL_DestroyTexture(sdl_overlay_texture);
    sdl_overlay_texture = NULL;
  }

  /* Create new texture for overlay rendering */
  sdl_overlay_texture = SDL_CreateTexture(renderer,
                                         SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         PRO_VID_SCRWIDTH, PRO_VID_MEMHEIGHT);
  if (!sdl_overlay_texture) {
    xh_debug_log("pro_overlay_create_texture: Failed to create overlay texture: %s", SDL_GetError());
  } else {
    xh_debug_log("pro_overlay_create_texture: Created overlay texture %dx%d", PRO_VID_SCRWIDTH, PRO_VID_MEMHEIGHT);
    pro_overlay_dirty = 1; /* Force update */
  }
}

/* Render overlay to SDL renderer */
void pro_overlay_render(SDL_Renderer *renderer)
{
  if (!pro_overlay_on || !pro_overlay_open || !renderer)
    return;

  /* Check if overlay texture is valid */
  if (!sdl_overlay_texture) {
    xh_debug_log("pro_overlay_render: Overlay texture is NULL, recreating");
    pro_overlay_create_texture(renderer);
    if (!sdl_overlay_texture) {
      xh_debug_log("pro_overlay_render: Failed to recreate overlay texture");
      return;
    }
  }

  /* Only update texture if content changed */
  if (pro_overlay_dirty) {
      /* Convert overlay data to RGBA format for SDL texture */
      void *pixels;
      int pitch;
      if (SDL_LockTexture(sdl_overlay_texture, NULL, &pixels, &pitch) != 0) {
        xh_debug_log("pro_overlay_render: Failed to lock overlay texture: %s", SDL_GetError());
        return;
      }

      /* Convert overlay data to RGBA format */
      uint32_t *rgba_pixels = (uint32_t *)pixels;
      for (int y = 0; y < PRO_VID_MEMHEIGHT; y++) {
        for (int x = 0; x < PRO_VID_SCRWIDTH; x++) {
          int pnum = y * PRO_VID_SCRWIDTH + x;
          uint8_t alpha = pro_overlay_alpha[pnum];

          if (alpha > 0) {
            /* Get pixel color from overlay data */
            uint32_t pixel = 0;
            for (int i = 0; i < pixsize; i++) {
              pixel |= ((uint32_t)pro_overlay_data[pixsize * pnum + i]) << (i * 8);
            }

            /* Convert to RGBA format */
            uint8_t r, g, b;
            if (clutmode == 1) {
              /* 8-bit color mode - use pixel as index */
              r = g = b = pixel;
            } else {
              /* Extract RGB components */
              r = (pixel >> 16) & 0xFF;
              g = (pixel >> 8) & 0xFF;
              b = pixel & 0xFF;
            }

            /* Create RGBA pixel with alpha blending */
            rgba_pixels[y * (pitch / 4) + x] = (alpha << 24) | (r << 16) | (g << 8) | b;
          } else {
            /* Transparent pixel */
            rgba_pixels[y * (pitch / 4) + x] = 0x00000000;
          }
        }
      }

      SDL_UnlockTexture(sdl_overlay_texture);
      pro_overlay_dirty = 0;
  }

  /* Render overlay texture */
  int window_w, window_h;
  SDL_GetRendererOutputSize(renderer, &window_w, &window_h);
  
  /* Assume 80x24 character grid for overlay positioning */
  /* Overlay texture is PRO_VID_SCRWIDTH x PRO_VID_MEMHEIGHT (1024x256) */
  /* Characters are 12x10 pixels in overlay space */
  
  float scale_x = (float)window_w / 1024.0f;
  float scale_y = (float)window_h / 240.0f; /* Map 24 lines * 10 pixels = 240 pixels */

  SDL_Rect dst_rect = {0, 0, window_w, (int)(256.0f * scale_y)};
  
  if (SDL_RenderCopy(renderer, sdl_overlay_texture, NULL, &dst_rect) != 0) {
    xh_debug_log("pro_overlay_render: Failed to render overlay texture: %s", SDL_GetError());
    /* If rendering fails, destroy the texture and try to recreate it next time */
    SDL_DestroyTexture(sdl_overlay_texture);
    sdl_overlay_texture = NULL;
  }
}
#endif
