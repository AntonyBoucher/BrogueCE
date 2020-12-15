#ifndef __TILES_H__
#define __TILES_H__

#include <SDL.h>
#include <SDL_image.h>
#include "platform.h"

void resizeWindow(int width, int height);
void updateTile(int row, int column, short charIndex,
    short foreRed, short foreGreen, short foreBlue,
    short backRed, short backGreen, short backBlue);
void updateScreen();

#endif
