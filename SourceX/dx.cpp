#include "diablo.h"
#include "../3rdParty/Storm/Source/storm.h"
#include "miniwin/ddraw.h"
#include <SDL.h>

namespace dvl {

int sgdwLockCount;
BYTE *gpBuffer;
#ifdef _DEBUG
int locktbl[256];
#endif
static CCritSect sgMemCrit;
HMODULE ghDiabMod;

int refreshDelay;
SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *texture;

/** Currently active palette */
SDL_Palette *palette;
unsigned int pal_surface_palette_version = 0;

/** 24-bit renderer texture surface */
SDL_Surface *renderer_texture_surface = nullptr;

/** 8-bit surface wrapper around #gpBuffer */
SDL_Surface *pal_surface;

static void dx_create_back_buffer()
{
	pal_surface = SDL_CreateRGBSurfaceWithFormat(0, BUFFER_WIDTH, BUFFER_HEIGHT, 8, SDL_PIXELFORMAT_INDEX8);
	if (pal_surface == NULL) {
		ErrSdl();
	}

	gpBuffer = (BYTE *)pal_surface->pixels;

	if (SDLC_SetSurfaceColors(pal_surface, palette) <= -1) {
		ErrSdl();
	}

	pal_surface_palette_version = 1;
}

static void dx_create_primary_surface()
{
#ifndef USE_SDL1
	if (renderer) {
		int width, height;
		SDL_RenderGetLogicalSize(renderer, &width, &height);
		Uint32 format;
		if (SDL_QueryTexture(texture, &format, nullptr, nullptr, nullptr) < 0)
			ErrSdl();
		renderer_texture_surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
	}
#endif
	if (GetOutputSurface() == nullptr) {
		ErrSdl();
	}
}

void dx_init(HWND hWnd)
{
	SDL_RaiseWindow(window);
	SDL_ShowWindow(window);

	dx_create_primary_surface();
	palette_init();
	dx_create_back_buffer();
}
static void lock_buf_priv()
{
	sgMemCrit.Enter();
	if (sgdwLockCount != 0) {
		sgdwLockCount++;
		return;
	}

	gpBufEnd += (uintptr_t)(BYTE *)pal_surface->pixels;
	gpBuffer = (BYTE *)pal_surface->pixels;
	sgdwLockCount++;
}

void lock_buf(BYTE idx)
{
#ifdef _DEBUG
	locktbl[idx]++;
#endif
	lock_buf_priv();
}

static void unlock_buf_priv()
{
	if (sgdwLockCount == 0)
		app_fatal("draw main unlock error");
	if (!gpBuffer)
		app_fatal("draw consistency error");

	sgdwLockCount--;
	if (sgdwLockCount == 0) {
		gpBufEnd -= (uintptr_t)gpBuffer;
		//gpBuffer = NULL; unable to return to menu
	}
	sgMemCrit.Leave();
}

void unlock_buf(BYTE idx)
{
#ifdef _DEBUG
	if (!locktbl[idx])
		app_fatal("Draw lock underflow: 0x%x", idx);
	locktbl[idx]--;
#endif
	unlock_buf_priv();
}

void dx_cleanup()
{
	if (ghMainWnd)
		SDL_HideWindow(window);
	sgMemCrit.Enter();
	sgdwLockCount = 0;
	gpBuffer = NULL;
	sgMemCrit.Leave();

	if (pal_surface == nullptr)
		return;
	SDL_FreeSurface(pal_surface);
	pal_surface = nullptr;
	SDL_FreePalette(palette);
	SDL_FreeSurface(renderer_texture_surface);
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
}

void dx_reinit()
{
	int lockCount;

	sgMemCrit.Enter();
	ClearCursor();
	lockCount = sgdwLockCount;

	while (sgdwLockCount != 0)
		unlock_buf_priv();

	dx_cleanup();

	force_redraw = 255;

	dx_init(ghMainWnd);

	while (lockCount != 0) {
		lock_buf_priv();
		lockCount--;
	}

	sgMemCrit.Leave();
}

void CreatePalette()
{
	palette = SDL_AllocPalette(256);
	if (palette == NULL) {
		ErrSdl();
	}
}

void BltFast(DWORD dwX, DWORD dwY, LPRECT lpSrcRect)
{
	auto w = static_cast<decltype(SDL_Rect().w)>(lpSrcRect->right - lpSrcRect->left + 1);
	auto h = static_cast<decltype(SDL_Rect().h)>(lpSrcRect->bottom - lpSrcRect->top + 1);
	SDL_Rect src_rect = {
		static_cast<decltype(SDL_Rect().x)>(lpSrcRect->left),
		static_cast<decltype(SDL_Rect().y)>(lpSrcRect->top),
		w, h
	};
	SDL_Rect dst_rect = {
		static_cast<decltype(SDL_Rect().x)>(dwX),
		static_cast<decltype(SDL_Rect().y)>(dwY),
		w, h
	};
	if (OutputRequiresScaling()) {
		ScaleOutputRect(&dst_rect);
		// Convert from 8-bit to 32-bit
		SDL_Surface *tmp = SDL_ConvertSurface(pal_surface, GetOutputSurface()->format, 0);
		if (SDL_BlitScaled(tmp, &src_rect, GetOutputSurface(), &dst_rect) <= -1) {
			SDL_FreeSurface(tmp);
			ErrSdl();
		}
		SDL_FreeSurface(tmp);
	} else {
		// Convert from 8-bit to 32-bit
		if (SDL_BlitSurface(pal_surface, &src_rect, GetOutputSurface(), &dst_rect) <= -1) {
			ErrSdl();
		}
	}
}

/**
 * @brief Limit FPS to avoid high CPU load, use when v-sync isn't available
 */
void LimitFrameRate()
{
	static uint32_t frameDeadline;
	uint32_t tc = SDL_GetTicks() * 1000;
	uint32_t v = 0;
	if (frameDeadline > tc) {
		v = tc % refreshDelay;
		SDL_Delay(v / 1000 + 1); // ceil
	}
	frameDeadline = tc + v + refreshDelay;
}

SDL_Surface *lightSurf;
int derp = 0;
void PutPixel32_nolock(SDL_Surface *surface, int x, int y, Uint32 color)
{
	Uint8 *pixel = (Uint8 *)surface->pixels;
	pixel += (y * surface->pitch) + (x * sizeof(Uint32));
	*((Uint32 *)pixel) = color;
}

Uint32 GetPixel32(SDL_Surface *surface, int x, int y)
{
	Uint8 *pixel = (Uint8 *)surface->pixels;
	pixel += (y * surface->pitch) + (x * sizeof(Uint32));
	return *((Uint32 *)pixel);
}

POINT gameToScreen(int targetRow, int targetCol)
{
	int playerRow = plr[myplr].WorldX;
	int playerCol = plr[myplr].WorldY;
	int sx = 32 * (targetRow - playerRow) + 32 * (playerCol - targetCol) + SCREEN_WIDTH / 2;
	if (ScrollInfo._sdir == 3) {
		sx -= 32;
	} else if (ScrollInfo._sdir == 7) {
		sx += 32;
	}
	int sy = 32 * (targetCol - playerCol) + sx / 2;
	if (ScrollInfo._sdir == 7) {
		sy -= 32;
	}
	POINT ret;
	ret.x = sx;
	ret.y = sy;
	return ret;
}

float distance(int x1, int y1, int x2, int y2)
{
	// Calculating distance
	return sqrt(pow(x2 - x1, 2) + pow(y2 - y1, 2) * 1.0);
}

float distance2(int x1, int y1, int x2, int y2)
{
	// Calculating distance
	return sqrt(pow(x2 - x1, 2) + pow((y2 - y1)*2, 2) * 1.0);
}

int mergeChannel(int a, int b, float amount)
{
	float result = (a * amount) + (b * (1 - amount));
	return (int)result;
}

Uint32 blendColors(Uint32 c1, Uint32 c2, float howmuch)
{
	int r = mergeChannel(c1 & 0x0000FF, c2 & 0x0000FF, howmuch);
	int g = mergeChannel((c1 & 0x00FF00) >> 8, (c2 & 0x00FF00) >> 8, howmuch);
	int b = mergeChannel((c1 & 0xFF0000) >> 16, (c2 & 0xFF0000) >> 16, howmuch);
	return r + (g << 8) + (b << 16);

	/*
    int r1 = (c1 & 0x0000FF);
    int g1 = (c1 & 0x00FF00);
    int b1 = (c1 & 0xFF0000);
    Uint32 out = r1 + (g1 << 8) + (b1 << 16);
    printf("IN: %08X OUT: %08X  %d %d %d\n", c1, out, r1,b1,g1);
    return out;
    */
}

void drawRadius(int lid, int row, int col, int radius)
{
	POINT pos = gameToScreen(row, col);
	int sx = pos.x;
	int sy = pos.y;

	int xoff = 0;
	int yoff = 0;

	for (int i = 0; i < nummissiles; i++) {
		MissileStruct *mis = &missile[missileactive[i]];
		if (mis->_mlid == lid) {
			xoff = mis->_mixoff;
			yoff = mis->_miyoff;
			break;
		}
	}
	printf("RADIUS %d", radius);
	sx += xoff;
	sy += yoff;
	int hey = radius * 64;
	for (int x = sx - hey; x < sx + hey; x++) {
		for (int y = sy - hey/2; y < sy + hey/2; y++) {
			if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
				float howmuch;
				float diffx = sx - x;
				float diffy = sy - y;
				float sa = diffx / 32;
				float a = sa * sa;
				float sb = diffy / 16;
				float b = sb * sb;
				float c = radius * 32;
				float ab = a + b;
				if (ab <= c) {

				//float dist = distance2(sx, sy, x, y);
				//if (dist <= radius * 32) {

					//howmuch = 0;
					//howmuch = 0.5*a/c + 0.5*b/c;
					switch (testvar1) {
					case 0:
						howmuch = 0;
						break;
					case 1:
						howmuch = (ab) / c;
						break;
					case 2:
						howmuch = cbrt(ab / c);
						break;
					}
					Uint32 pix = GetPixel32(lightSurf, x, y);
					if (pix == 0 || 1 != 0) {
						pix = GetPixel32(GetOutputSurface(), x, y);
					}
					//PutPixel32_nolock(lightSurf, x, y, blendColors(0x000000, pix, howmuch));
					PutPixel32_nolock(lightSurf, x, y, pix);
					//PutPixel32_nolock(GetOutputSurface(), x, y, 0x000000);
				}
			}
		}
	}
}

void turbopotato()
{
	for (int i = 0; i < numlights; i++) {
		int lid = lightactive[i];
		drawRadius(lid, LightList[lid]._lx, LightList[lid]._ly, LightList[lid]._lradius);
	}
}

void RenderPresent()
{
	SDL_Surface *surface = GetOutputSurface();
	assert(!SDL_MUSTLOCK(surface));

	if (!gbActive) {
		LimitFrameRate();
		return;
	}

#ifndef USE_SDL1
	if (renderer) {
		if (testvar3 == 0) {
			SDL_UpdateTexture(texture, NULL, surface->pixels, surface->pitch);
		} else {
			if (derp == 0) {
				derp = 1;
				int width, height;
				SDL_RenderGetLogicalSize(renderer, &width, &height);
				Uint32 format;
				if (SDL_QueryTexture(texture, &format, nullptr, nullptr, nullptr) < 0)
					ErrSdl();
				lightSurf = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
			}
			SDL_FillRect(lightSurf, NULL, SDL_MapRGB(lightSurf->format, 0, 0, 0));
			SDL_BlendMode bm;
			switch (testvar5) {
			case 0:
				bm = SDL_BLENDMODE_NONE;
				break;
			case 1:
				bm = SDL_BLENDMODE_BLEND;
				break;
			case 2:
				bm = SDL_BLENDMODE_ADD;
				break;
			case 3:
				bm = SDL_BLENDMODE_MOD;
				break;
			}
			SDL_SetSurfaceBlendMode(lightSurf, bm);
			turbopotato();
			SDL_BlitSurface(lightSurf, NULL, surface, NULL);
			//SDL_UpdateTexture(texture, NULL, lightSurf->pixels, lightSurf->pitch);
			SDL_UpdateTexture(texture, NULL, surface->pixels, surface->pitch);
		}

		// Clear buffer to avoid artifacts in case the window was resized
		if (SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) <= -1) { // TODO only do this if window was resized
			ErrSdl();
		}

		if (SDL_RenderClear(renderer) <= -1) {
			ErrSdl();
		}
		if (SDL_RenderCopy(renderer, texture, NULL, NULL) <= -1) {
			ErrSdl();
		}
		SDL_RenderPresent(renderer);
	} else {
		if (SDL_UpdateWindowSurface(window) <= -1) {
			ErrSdl();
		}
		LimitFrameRate();
	}
#else
	if (SDL_Flip(surface) <= -1) {
		ErrSdl();
	}
	LimitFrameRate();
#endif
}

void PaletteGetEntries(DWORD dwNumEntries, LPPALETTEENTRY lpEntries)
{
	for (DWORD i = 0; i < dwNumEntries; i++) {
		lpEntries[i].peFlags = 0;
		lpEntries[i].peRed = system_palette[i].peRed;
		lpEntries[i].peGreen = system_palette[i].peGreen;
		lpEntries[i].peBlue = system_palette[i].peBlue;
	}
}
} // namespace dvl
