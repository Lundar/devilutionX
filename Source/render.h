#ifndef __RENDER_H__
#define __RENDER_H__

DEVILUTION_BEGIN_NAMESPACE

#ifdef __cplusplus
extern "C" {
#endif

/**
 * level_cel_block Specifies the current MIN block of the level CEL file, as used during rendering of the level tiles.
 *
 * frameNum  := block & 0x0FFF
 * frameType := block & 0x7000 >> 12
 */
void RenderTile(BYTE *pBuff, DWORD level_cel_block);
void world_draw_black_tile(int sx, int sy);
void trans_rect(int sx, int sy, int width, int height);

#ifdef __cplusplus
}
#endif

DEVILUTION_END_NAMESPACE

#endif /* __RENDER_H__ */
