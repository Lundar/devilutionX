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

#ifdef PIXEL_LIGHT
SDL_Surface *lightSurf;
SDL_Texture *lightTex;

SDL_Surface *fpsVision;
SDL_Texture *fpsTex;

SDL_Surface *predrawnEllipses[20];
SDL_Texture *ellipsesTextures[20];

int width, height;
int lightReady = 0;
Uint32 format;

void PutPixel32_nolock(SDL_Surface *surface, int x, int y, Uint32 color)
{
	Uint8 *pixel = (Uint8 *)surface->pixels;
	pixel += (y * surface->pitch) + (x * sizeof(Uint32));
	*((Uint32 *)pixel) = color;
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
}

void drawRadius(int lid, int row, int col, int radius, int color)
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
	sx += xoff;
	sy += yoff;
	SDL_Rect rect;

	int srcx = width / 2;
	int srcy = height / 2;
	int targetx = sx;
	int targety = sy;
	int offsetx = targetx - srcx;
	int offsety = targety - srcy;

	rect.x = offsetx;
	rect.y = offsety;
	rect.w = width;
	rect.h = height;

	Uint8 r = (color & 0x0000FF);
	Uint8 g = (color & 0x00FF00) >> 8;
	Uint8 b = (color & 0xFF0000) >> 16;
	if(SDL_SetTextureColorMod(ellipsesTextures[radius], r, g, b) < 0)
		ErrSdl();
	if(SDL_RenderCopy(renderer, ellipsesTextures[radius], NULL, &rect) < 0)
		ErrSdl();
}

void lightLoop()
{
	for (int i = 0; i < numlights; i++) {
		int lid = lightactive[i];
		drawRadius(lid, LightList[lid]._lx, LightList[lid]._ly, LightList[lid]._lradius + 1, LightList[lid]._color);
	}
}

void predrawEllipse(int radius)
{
	int sx = width / 2;
	int sy = height / 2;
	int hey = radius * 16;
	for (int x = 0; x < width; x++) {
		for (int y = 0; y < height; y++) {
			if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
				float howmuch;
				float diffx = sx - x;
				float diffy = sy - y;
				float sa = diffx / 32;
				float a = sa * sa;
				float sb = diffy / 16;
				float b = sb * sb;
				float c = hey;
				float ab = a + b;
				if (ab <= c) {
					howmuch = cbrt(ab / c);
					PutPixel32_nolock(predrawnEllipses[radius], x, y, blendColors(0x000000, 0xFFFFFF, howmuch));
				}
			}
		}
	}
}

void prepareFPS(){
	fpsVision = SDL_CreateRGBSurfaceWithFormat(0, 50, 50, SDL_BITSPERPIXEL(format), format);
	if (fpsVision == NULL)
		ErrSdl();
	if(SDL_SetSurfaceBlendMode(fpsVision, SDL_BLENDMODE_ADD) < 0)
		ErrSdl();
	if(SDL_FillRect(fpsVision, NULL, SDL_MapRGB(fpsVision->format, 255, 255, 255)) < 0)
		ErrSdl();
	fpsTex = SDL_CreateTextureFromSurface(renderer, fpsVision);
	if (fpsTex == NULL)
		ErrSdl();
	if(SDL_SetTextureBlendMode(fpsTex, SDL_BLENDMODE_ADD) < 0)
		ErrSdl();
}

void showFPS(){
	SDL_Rect rect;
	rect.x = 0;
	rect.y = 35;
	rect.w = 50;
	rect.h = 50;
	if(SDL_RenderCopy(renderer, fpsTex, NULL, &rect)< 0)
		ErrSdl();
}

void prepareLight()
{
	SDL_RenderGetLogicalSize(renderer, &width, &height);
	if (SDL_QueryTexture(texture, &format, nullptr, nullptr, nullptr) < 0)
		ErrSdl();
	lightSurf = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
	if (lightSurf == NULL)
		ErrSdl();
	for (int i = 1; i <= 15; i++) {
		predrawnEllipses[i] = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
		if (predrawnEllipses[i] == NULL)
			ErrSdl();
		if (SDL_SetSurfaceBlendMode(predrawnEllipses[i], SDL_BLENDMODE_ADD) < 0)
			ErrSdl();
		if (SDL_FillRect(predrawnEllipses[i], NULL, SDL_MapRGB(predrawnEllipses[i]->format, 0, 0, 0)) < 0)
			ErrSdl();
		predrawEllipse(i);
		ellipsesTextures[i] = SDL_CreateTextureFromSurface(renderer, predrawnEllipses[i]);
		if (ellipsesTextures[i] == NULL)
			ErrSdl();
		if (SDL_SetTextureBlendMode(ellipsesTextures[i], SDL_BLENDMODE_ADD) < 0)
			ErrSdl();
	}
}

#endif
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
#ifdef PIXEL_LIGHT
		if (testvar3 != 0) {
			if (lightReady != 1) {
				lightReady = 1;
				prepareLight();
				prepareFPS();
			}
			if(SDL_FillRect(lightSurf, NULL, SDL_MapRGB(lightSurf->format, 0, 0, 0)) < 0)
				ErrSdl();
			lightTex = SDL_CreateTextureFromSurface(renderer, lightSurf);
			if (lightTex == NULL)
				ErrSdl();
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
			if(SDL_SetTextureBlendMode(lightTex, bm) < 0)
				ErrSdl();
			if(SDL_SetTextureBlendMode(texture, bm) < 0)
				ErrSdl();
		} else {
			if(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) < 0)
				ErrSdl();
		}
#endif

		if (SDL_UpdateTexture(texture, NULL, surface->pixels, surface->pitch) <= -1) { //pitch is 2560
			ErrSdl();
		}

		// Clear buffer to avoid artifacts in case the window was resized
		if (SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) <= -1) { // TODO only do this if window was resized
			ErrSdl();
		}

		if (SDL_RenderClear(renderer) <= -1) {
			ErrSdl();
		}

#ifdef PIXEL_LIGHT
		if (testvar3 != 0) {
			if (SDL_RenderCopy(renderer, lightTex, NULL, NULL) < 0)
				ErrSdl();
			lightLoop();
			showFPS();
			SDL_DestroyTexture(lightTex);
		}

#endif
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
