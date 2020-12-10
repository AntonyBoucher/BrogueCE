#ifndef __TILES_H__
#define __TILES_H__

#include <SDL.h>
#include <SDL_image.h>
#include "platform.h"

SDL_Texture *getTexture(int tileWidth, int tileHeight);
void resizeWindow(int width, int height);

#endif
