/* font_loader.h */
#ifndef FONT_LOADER_H
#define FONT_LOADER_H

#include <SDL/SDL_ttf.h>

/* Load a TTF font from memory
 * Returns NULL on error
 */
TTF_Font* load_font_from_memory(const unsigned char* font_data, 
                               unsigned int data_size, 
                               int pt_size);

#endif