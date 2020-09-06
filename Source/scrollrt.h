//HEADER_GOES_HERE
#ifndef __SCROLLRT_H__
#define __SCROLLRT_H__

DEVILUTION_BEGIN_NAMESPACE

#ifdef __cplusplus
extern "C" {
#endif

extern bool sgbControllerActive;
extern int light_table_index;
#ifdef PIXEL_LIGHT
extern int testvar1;
extern int testvar2;
extern int testvar3;
extern int testvar4;
extern int testvar5;
extern std::map<int, std::vector<LightListStruct> > staticLights;
extern int redrawLights;
extern SDL_Surface *pal_surface;
extern SDL_Surface *ui_surface;
extern SDL_Surface *tmp_surface;
extern bool drawRed;
extern std::map<std::string, int> lightColorMap;
#endif
extern BYTE *gpBufStart;
extern BYTE *gpBufEnd;
extern char arch_draw_type;
extern int cel_transparency_active;
extern int cel_foliage_active;
extern int level_piece_id;
extern void (*DrawPlrProc)(int, int, int, int, int, BYTE *, int, int, int, int);

void ClearCursor();
void DrawMissile(int x, int y, int sx, int sy, BOOL pre);
void DrawDeadPlayer(int x, int y, int sx, int sy);
void ShiftGrid(int *x, int *y, int horizontal, int vertical);
int RowsCoveredByPanel();
void CalcTileOffset(int *offsetX, int *offsetY);
void TilesInView(int *columns, int *rows);
void DrawView(int StartX, int StartY);
void ClearScreenBuffer();
#ifdef _DEBUG
void ScrollView();
#endif
void EnableFrameCount();
void scrollrt_draw_game_screen(BOOL draw_cursor);
void DrawAndBlit();

/* rdata */

/* data */

/** used in 1.00 debug */
extern char *szMonModeAssert[18];
extern char *szPlrModeAssert[12];

#ifdef __cplusplus
}
#endif

DEVILUTION_END_NAMESPACE

#endif /* __SCROLLRT_H__ */
