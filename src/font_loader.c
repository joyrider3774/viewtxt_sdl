/* font_loader.c */
#include <SDL.h>
#include <SDL_ttf.h>
#include <string.h>
#include "font_loader.h"

static SDL_RWops* create_rwops_from_memory(const unsigned char* data, 
                                          unsigned int size) {
    return SDL_RWFromMem((void*)data, size);
}

TTF_Font* load_font_from_memory(const unsigned char* font_data, 
                               unsigned int data_size, 
                               int pt_size) {
    SDL_RWops* rw = create_rwops_from_memory(font_data, data_size);
    if (!rw) {
        return NULL;
    }
    
    /* 1 means SDL_ttf will free the RWops */
    TTF_Font* font = TTF_OpenFontRW(rw, 1, pt_size);
    return font;
}