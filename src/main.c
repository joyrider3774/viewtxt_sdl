#ifdef _WIN32
#include <windows.h>
#endif
#include <errno.h>
#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include "font_loader.h"
#include "font_data.h"

#define DEFAULT_BLOCKSIZE 50
#define MARGINS 4
#define SETTINGS_FILE_VERSION 4
#define DEFAULT_FONT_SIZE 12
#define DEFAULT_WIDTH 240
#define DEFAULT_HEIGHT 240
#define LINE_SPACING 1.2f
#define DEFAULT_FONT "./fonts/DejaVuSansMono.ttf"
#define SETTINGS_DIR ".txtview"
#define SETTINGS_FILE "positions_v2.bin"

#ifndef MAX_PATH
    #define MAX_PATH 1024
#endif

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))


// Make block size configurable
typedef struct {
    size_t block_size;
    struct timeval start_time;
    struct timeval end_time;
    long memory_used;
    int total_blocks;
} LayoutStats;

typedef struct {
    int y_position;          // Vertical position of this line
    int height;             // Height of this line
    int line_start_offset;  // Offset in the source text where this line starts
    int line_length;        // Length of this line in characters
    int is_wrapped;         // Whether this line was wrapped (split due to width)
} LineInfo;

typedef struct LineBlock {
    struct LineBlock* next;
    LineInfo lines[];  // Flexible array member
} LineBlock;

typedef struct {
    LineBlock* first_block;
    LineBlock* current_block;
    size_t current_block_used;
    int total_lines;
    int last_calculated_width;
    int calculated_total_height;
    size_t block_size;           // Configurable block size
    LayoutStats stats;           // Stats for this layout
} TextLayout;

// Struct to store file scroll position
typedef struct {
    int version;              // File format version
    char filename[MAX_PATH];  // Full path of the file
    char font_path[MAX_PATH]; // Path to the font used
    int font_size;            // Font size when position was saved
    int scroll_position;      // Saved scroll position
    int scroll_position_adjusted;
    int ignore_linebreaks;
    int inverted_colors;
} FileScrollPosition;

typedef struct {
    char* text;              // Dynamically allocated text
    char* adjustested_text;
    size_t length;           // Length of text
    int scroll_position;
    int scroll_position_adjusted;
    TTF_Font* font;
    int font_size;
    SDL_Color text_color;
    SDL_Color bg_color;
    int window_width;
    int window_height;
    char current_file[MAX_PATH];
    char settings_path[MAX_PATH];
    char font_path[MAX_PATH];
    int ignore_linebreaks;
    int inverted_colors;
    TextLayout normal_layout;    // Layout info for normal text
    TextLayout adjusted_layout;  // Layout info for text with ignored linebreaks
} TextViewer;

// Configuration structure
typedef struct {
    char font_path[MAX_PATH];
    int font_size;
    SDL_Color bg_color;
    SDL_Color text_color;
    char encoding[32];
    int ignore_linebreaks;
    int inverted_colors;
} ViewerConfig;

typedef struct {
    unsigned long text_hash;
    size_t text_length;
    int window_width;
    int font_size;
    int cached_height;
    int in_use;  // Flag to indicate if this cache slot is occupied
} HeightCache;

typedef struct {
    char message[1024];
    Uint32 stop_display_time;
    int padding;
    int x;
    int y;
    SDL_Color fg;
    SDL_Color bg;
} DisplayMessageData;

TTF_Font *InteralFont;
DisplayMessageData message_data;

// Function prototypes
char* resolve_path(const char* path);
int utf8_char_length(const char* str);
void print_usage(const char* program_name);
void save_scroll_position(TextViewer* viewer);
int load_scroll_position(TextViewer* viewer);
void ensure_settings_dir(const char* settings_path);
void destroy_viewer(TextViewer* viewer);
void change_font_size(TextViewer* viewer, int new_size);
int load_text_file(TextViewer* viewer, const char* filename, const char* encoding);
void render_text(TextViewer* viewer, SDL_Surface* screen);
char* convert_to_utf8(const char* input, size_t input_len, const char* from_encoding);
int is_ttf_file(const char* filename);
TextViewer* create_viewer(const char* settings_path, const char* font_path, int font_size, int width, int height, 
    SDL_Color text_color, SDL_Color bg_color, int ignore_linebreaks, int inverted_colors);
void enforce_scroll_boundaries(TextViewer* viewer);
void calculate_text_layout(TextViewer* viewer, TextLayout* layout, const char* text);
void free_text_layout(TextLayout* layout);
void init_text_layout(TextLayout* layout, size_t block_size);
int ensure_layout_capacity(TextLayout* layout);
void display_message(char* message, Uint32 display_time, int x, int y, int padding, SDL_Color fg, SDL_Color bg);
void draw_display_message(SDL_Surface *destSurface);
void stop_display_message();

void stop_display_message()
{
    message_data.stop_display_time = 0;
}

void display_message(char* message, Uint32 display_time, int x, int y, int padding, SDL_Color fg, SDL_Color bg)
{
    memset(message_data.message, 0, 1024);
    snprintf(message_data.message, sizeof(message_data.message), "%s", message);
    message_data.stop_display_time = SDL_GetTicks() + display_time;
    message_data.x = x;
    message_data.y = y;
    message_data.padding = padding;
    message_data.fg = fg;
    message_data.bg = bg;
}

void draw_display_message(SDL_Surface *destSurface)
{
    if(message_data.stop_display_time < SDL_GetTicks())
        return;
    int padding = message_data.padding < 4 ? 4 : message_data.padding;
    int w, h;
    TTF_SizeText(InteralFont, message_data.message, &w, &h);
    SDL_Rect dst = {message_data.x - (w >> 1) - padding, message_data.y - (h >> 1) - padding, w + 2 * padding, h + 2 * padding };
    SDL_FillRect(destSurface, &dst, SDL_MapRGB(destSurface->format, message_data.bg.r, message_data.bg.g, message_data.bg.b));
    SDL_Rect dst2 = {message_data.x - (w >> 1) - padding + 2, message_data.y - (h >> 1) - padding + 2, w + 2 * padding - 4, h + 2 * padding -4};
    SDL_FillRect(destSurface, &dst2, SDL_MapRGB(destSurface->format, message_data.fg.r, message_data.fg.g, message_data.fg.b));
    SDL_Rect dst3 = {message_data.x - (w >> 1) - padding + 3, message_data.y - (h >> 1) - padding + 3, w + 2 * padding - 6, h + 2 * padding -6};
    SDL_FillRect(destSurface, &dst3, SDL_MapRGB(destSurface->format, message_data.bg.r, message_data.bg.g, message_data.bg.b));
    SDL_Surface *tmp = TTF_RenderText_Blended(InteralFont, message_data.message, message_data.fg);
    if(tmp)
    {
        SDL_Rect dst4 = {message_data.x - (w >> 1), message_data.y - (h >> 1), w, h};
        SDL_BlitSurface(tmp, NULL, destSurface, &dst4);
        SDL_FreeSurface(tmp);
    }
}

// Function to start timing
void start_timing(LayoutStats* stats) {
    gettimeofday(&stats->start_time, NULL);
    stats->memory_used = 0;
    stats->total_blocks = 0;
}

// Function to end timing
void end_timing(LayoutStats* stats) {
    gettimeofday(&stats->end_time, NULL);
}

// Function to get elapsed milliseconds
long get_elapsed_ms(LayoutStats* stats) {
    long seconds = stats->end_time.tv_sec - stats->start_time.tv_sec;
    long microseconds = stats->end_time.tv_usec - stats->start_time.tv_usec;
    return seconds * 1000 + microseconds / 1000;
}

// Corrected initialization
void init_text_layout(TextLayout* layout, size_t block_size) {
    layout->block_size = block_size;
    size_t alloc_size = sizeof(LineBlock) + (block_size * sizeof(LineInfo));
    layout->first_block = malloc(alloc_size);
    
    if (layout->first_block) {
        layout->first_block->next = NULL;
        layout->current_block = layout->first_block;
        layout->current_block_used = 0;
        layout->total_lines = 0;
        layout->last_calculated_width = 0;
        layout->calculated_total_height = 0;
        layout->stats.block_size = block_size;
        layout->stats.memory_used = alloc_size;
        layout->stats.total_blocks = 1;
    } else {
        memset(layout, 0, sizeof(TextLayout));
    }
}

// Corrected block allocation
int ensure_layout_capacity(TextLayout* layout) {
    if (layout->current_block == NULL || 
        layout->current_block_used >= layout->block_size) {
        // Need new block
        size_t alloc_size = sizeof(LineBlock) + (layout->block_size * sizeof(LineInfo));
        LineBlock* new_block = malloc(alloc_size);
        if (!new_block) {
            return 0;  // Allocation failed
        }
        new_block->next = NULL;
        
        if (layout->current_block) {
            layout->current_block->next = new_block;
        } else {
            layout->first_block = new_block;
        }
        
        layout->current_block = new_block;
        layout->current_block_used = 0;
        layout->stats.memory_used += alloc_size;
        layout->stats.total_blocks++;
    }
    return 1;
}

// Helper function to get a line from the layout
LineInfo* get_line_from_layout(TextLayout* layout, int index) {
    if (index < 0 || index >= layout->total_lines) return NULL;
    
    int block_index = index / layout->block_size;
    int line_index = index % layout->block_size;
    
    LineBlock* block = layout->first_block;
    for (int i = 0; i < block_index && block; i++) {
        block = block->next;
    }
    
    if (!block) return NULL;
    return &block->lines[line_index];
}

// Clean up function
void free_text_layout(TextLayout* layout) {
    LineBlock* block = layout->first_block;
    while (block) {
        LineBlock* next = block->next;
        free(block);
        block = next;
    }
    memset(layout, 0, sizeof(TextLayout));
}
size_t find_fitting_text_length(TTF_Font* font, const char* text, size_t max_length, int max_width) {
    if (!font || !text || max_length == 0) return 0;
    
    char measure_buffer[max_length+1];
    size_t left = 0;
    size_t right = max_length;
    size_t best_fit = 0;
    
    while (left <= right && right <= max_length) {
        size_t mid = left + (right - left) / 2;
        
        // Safely copy text portion for measurement
        if (mid >= sizeof(measure_buffer)) {
            // If midpoint would exceed our buffer, use maximum safe length
            mid = sizeof(measure_buffer) - 1;
        }
        
        memset(measure_buffer, 0, sizeof(measure_buffer));
        memcpy(measure_buffer, text, mid);
        measure_buffer[mid] = '\0';
        
        int width = 0;
        if (TTF_SizeUTF8(font, measure_buffer, &width, NULL) == 0) {
            if (width <= max_width) {
                best_fit = mid;
                // If we're at max safe buffer size, no point trying larger
                if (mid == sizeof(measure_buffer) - 1) break;
                left = mid + 1;
            } else {
                if (mid == 0) break;
                right = mid - 1;
            }
        } else {
            // If TTF_SizeUTF8 fails, try with a smaller chunk
            right = mid - 1;
        }
    }
    
    // If we found a fit, try to break at the last space for word wrapping
    if (best_fit > 0 && best_fit < max_length) {
        size_t space_pos = best_fit;
        while (space_pos > 0) {
            if (text[space_pos] == ' ') {
                // Verify this shorter length still fits
                memset(measure_buffer, 0, sizeof(measure_buffer));
                memcpy(measure_buffer, text, space_pos);
                measure_buffer[space_pos] = '\0';
                
                int width = 0;
                if (TTF_SizeUTF8(font, measure_buffer, &width, NULL) == 0 && 
                    width <= max_width) {
                    best_fit = space_pos;
                    break;
                }
            }
            space_pos--;
        }
    }
    
    return best_fit;
}

// Add line to current block, allocate new block if needed
LineInfo* add_line_to_layout(TextLayout* layout) {
    if (layout->current_block_used >= layout->block_size) {
        // Allocate block with space for LineInfo array
        size_t alloc_size = sizeof(LineBlock) + (layout->block_size * sizeof(LineInfo));
        LineBlock* new_block = malloc(alloc_size);
        if (!new_block) return NULL;
        
        new_block->next = NULL;
        
        if (layout->current_block) {
            layout->current_block->next = new_block;
        } else {
            layout->first_block = new_block;
        }
        
        layout->current_block = new_block;
        layout->current_block_used = 0;
        layout->stats.memory_used += alloc_size;
        layout->stats.total_blocks++;
    }
    
    LineInfo* line = &layout->current_block->lines[layout->current_block_used++];
    layout->total_lines++;
    return line;
}

void ensure_settings_dir(const char* settings_path) {
    // Try to create directory
    #ifdef _WIN32
    if (mkdir(settings_path) == -1) {
    #else
    if (mkdir(settings_path, 0700) == -1) {
    #endif
        if (errno != EEXIST) {
            printf("Failed to create directory: %s (errno: %d)\n", settings_path, errno);
        }
    }
}

void calculate_text_layout(TextViewer* viewer, TextLayout* layout, const char* text) {
    if (!text || !layout->first_block) return;
    
    // Reset layout
    layout->current_block = layout->first_block;
    layout->current_block_used = 0;
    layout->total_lines = 0;
    layout->last_calculated_width = viewer->window_width;
    layout->calculated_total_height = 0;

    const char* text_ptr = text;
    int current_y = 0;
    int line_height = (int)(viewer->font_size * LINE_SPACING);
    int max_width = viewer->window_width - 2*MARGINS;

    while (text_ptr && *text_ptr) {
        const char* line_end = text_ptr;
        size_t line_length = 0;
        
        // Find end of current line
        while (*line_end && *line_end != '\n' && *line_end != '\r') {
            line_end++;
            line_length++;
        }

        if (line_length > 0) {
            size_t start = 0;
            while (start < line_length) {
                // Find how much text fits using binary search
                size_t chars_that_fit = find_fitting_text_length(viewer->font, 
                    text_ptr + start, line_length - start, max_width);
                
                if (chars_that_fit == 0) {
                    // Force at least one character if nothing fits
                    chars_that_fit = utf8_char_length(text_ptr + start);
                }

                // Add line to layout
                LineInfo* line = add_line_to_layout(layout);
                if (!line) {
                    printf("Failed to add line to layout\n");
                    return;
                }

                line->y_position = current_y;
                line->height = line_height;
                line->line_start_offset = text_ptr + start - text;
                line->line_length = chars_that_fit;
                line->is_wrapped = (start + chars_that_fit < line_length);
                
                current_y += line_height;

                start += chars_that_fit;
                // Skip whitespace at start of next line
                while (start < line_length && (text_ptr[start] == ' ' || text_ptr[start] == '\t')) {
                    start++;
                }
            }
        } else {
            // Empty line
            LineInfo* line = add_line_to_layout(layout);
            if (!line) {
                printf("Failed to add empty line to layout\n");
                return;
            }

            line->y_position = current_y;
            line->height = line_height;
            line->line_start_offset = text_ptr - text;
            line->line_length = 0;
            line->is_wrapped = 0;
            
            current_y += line_height;
        }

        // Move to next line
        text_ptr = line_end;
        if (*text_ptr == '\r' && *(text_ptr + 1) == '\n') text_ptr += 2;
        else if (*text_ptr == '\n' || *text_ptr == '\r') text_ptr++;
    }

    layout->calculated_total_height = current_y;
}

TextViewer* create_viewer(const char* settings_path, const char* font_path, int font_size, int width, int height, 
    SDL_Color text_color, SDL_Color bg_color, int ignore_linebreaks, int inverted_colors) {
    TextViewer* viewer = (TextViewer*)malloc(sizeof(TextViewer));
    if (!viewer) return NULL;

    viewer->text = NULL;  // Initialize text pointer to NULL
    viewer->adjustested_text = NULL;
    viewer->length = 0;
    viewer->scroll_position = 0;
    viewer->scroll_position_adjusted = 0;
    viewer->inverted_colors = 0;
    viewer->font_size = font_size;
    viewer->window_width = width;
    viewer->window_height = height;
    viewer->text_color = text_color;
    viewer->bg_color = bg_color;
    viewer->current_file[0] = '\0';
    viewer->ignore_linebreaks = ignore_linebreaks;
    viewer->inverted_colors = inverted_colors;

    
    // Initialize layouts
    init_text_layout(&viewer->normal_layout, DEFAULT_BLOCKSIZE);
    init_text_layout(&viewer->adjusted_layout, DEFAULT_BLOCKSIZE);

    char* tmp = resolve_path(settings_path);
    memset(viewer->settings_path, 0, MAX_PATH);
    strncpy(viewer->settings_path, tmp, MAX_PATH-1);
    free(tmp);

    tmp = resolve_path(font_path);
    memset(viewer->font_path, 0, MAX_PATH);
    strncpy(viewer->font_path, resolve_path(font_path), MAX_PATH-1);
    free(tmp);

    viewer->font = TTF_OpenFont(viewer->font_path, font_size);
    if (!viewer->font) 
    {
        printf("Failed to load font \"%s\": %s\n", viewer->font_path, TTF_GetError());
        free(viewer->text);
        free(viewer->adjustested_text);
        free_text_layout(&viewer->normal_layout);
        free_text_layout(&viewer->adjusted_layout);
        free(viewer);
        return NULL;
    }

    return viewer;
}

// Basic character encoding conversion function
char* convert_to_utf8(const char* input, size_t input_len, const char* from_encoding) {
    // If already UTF-8 or encoding is unspecified, return a copy
    if (!from_encoding || 
        strcasecmp(from_encoding, "UTF-8") == 0 || 
        strcasecmp(from_encoding, "UTF8") == 0) {
        char* output = malloc(input_len + 1);
        if (!output) return NULL;
        memcpy(output, input, input_len);
        output[input_len] = '\0';
        return output;
    }

    // Handle some common single-byte encodings manually
    if (strcasecmp(from_encoding, "ISO-8859-1") == 0) {
        char* output = malloc(input_len + 1);
        if (!output) return NULL;
        
        for (size_t i = 0; i < input_len; i++) {
            unsigned char c = (unsigned char)input[i];
            
            if (c < 0x80) {
                // ASCII characters remain the same
                output[i] = c;
            } else {
                // Convert ISO-8859-1 to UTF-8
                output[i] = (char)(0xC0 | (c >> 6));
                output[++i] = (char)(0x80 | (c & 0x3F));
            }
        }
        
        output[input_len] = '\0';
        return output;
    }

    // Fallback: return a copy of the original text
    char* output = malloc(input_len + 1);
    if (!output) return NULL;
    memcpy(output, input, input_len);
    output[input_len] = '\0';
    return output;
}


void save_scroll_position(TextViewer* viewer) {
    FILE* settings_file = fopen(viewer->settings_path, "r+b");
    if (!settings_file) {
        // If file doesn't exist, create it and write the new entry
        settings_file = fopen(viewer->settings_path, "wb");
        if (!settings_file) return;
        
        // Create and write new entry
        FileScrollPosition new_entry;
        memset(&new_entry, 0, sizeof(FileScrollPosition));
        
        new_entry.version = SETTINGS_FILE_VERSION;
        
        // Safely copy filename and font path
        memset(new_entry.filename, 0, MAX_PATH);
        memcpy(new_entry.filename, viewer->current_file, MAX_PATH - 1);
        
        memset(new_entry.font_path, 0, MAX_PATH);
        memcpy(new_entry.font_path, viewer->font_path, MAX_PATH - 1);
        
        new_entry.scroll_position = viewer->scroll_position;
        new_entry.scroll_position_adjusted = viewer->scroll_position_adjusted;
        new_entry.inverted_colors = viewer->inverted_colors;
        new_entry.ignore_linebreaks = viewer->ignore_linebreaks;
        new_entry.font_size = viewer->font_size;
        
        fwrite(&new_entry, sizeof(FileScrollPosition), 1, settings_file);
        fclose(settings_file);
        return;
    }
    
    // Track if we found and updated an existing entry
    int found = 0;
    FileScrollPosition current_entry;
    
    // Seek to beginning of file
    fseek(settings_file, 0, SEEK_SET);
    
    // Track current file position
    long current_pos = 0;
    
    // Read through existing entries
    while (fread(&current_entry, sizeof(FileScrollPosition), 1, settings_file) == 1) {
        // Check for matching filename and font path
        if (strcmp(current_entry.filename, viewer->current_file) == 0 &&
            strcmp(current_entry.font_path, viewer->font_path) == 0) {
            // Found matching entry, update its scroll position
            
            // Use absolute positioning 
            fseek(settings_file, current_pos, SEEK_SET);
            
            // Update entry with current information
            current_entry.version = SETTINGS_FILE_VERSION;
            
            // Safely copy filename and font path
            memset(current_entry.filename, 0, MAX_PATH);
            memcpy(current_entry.filename, viewer->current_file, MAX_PATH - 1);
            
            memset(current_entry.font_path, 0, MAX_PATH);
            memcpy(current_entry.font_path, viewer->font_path, MAX_PATH - 1);
            
            current_entry.scroll_position = viewer->scroll_position;
            current_entry.scroll_position_adjusted = viewer->scroll_position_adjusted;
            current_entry.font_size = viewer->font_size;
            current_entry.ignore_linebreaks = viewer->ignore_linebreaks;
            current_entry.inverted_colors = viewer->inverted_colors;
            
            fwrite(&current_entry, sizeof(FileScrollPosition), 1, settings_file);
            found = 1;
            break;
        }
        
        // Track current position
        current_pos = ftell(settings_file);
    }
    
    // If no matching entry found, append new entry
    if (!found) {
        FileScrollPosition new_entry;
        memset(&new_entry, 0, sizeof(FileScrollPosition));
        
        new_entry.version = SETTINGS_FILE_VERSION;
        
        // Safely copy filename and font path
        memset(new_entry.filename, 0, MAX_PATH);
        memcpy(new_entry.filename, viewer->current_file, MAX_PATH - 1);
        
        memset(new_entry.font_path, 0, MAX_PATH);
        memcpy(new_entry.font_path, viewer->font_path, MAX_PATH - 1);
        
        new_entry.scroll_position = viewer->scroll_position;
        new_entry.scroll_position_adjusted = viewer->scroll_position_adjusted;
        new_entry.font_size = viewer->font_size;
        new_entry.ignore_linebreaks = viewer->ignore_linebreaks;
        new_entry.inverted_colors = viewer->inverted_colors;
        
        fseek(settings_file, 0, SEEK_END);
        fwrite(&new_entry, sizeof(FileScrollPosition), 1, settings_file);
    }
    
    fclose(settings_file);
}

int load_scroll_position(TextViewer* viewer) {
    viewer->scroll_position = 0;
    viewer->scroll_position_adjusted = 0;

    FILE* settings_file = fopen(viewer->settings_path, "rb");

    // First, validate existing file
    if (settings_file) {
        FileScrollPosition test_entry;
        size_t read_count = fread(&test_entry, sizeof(FileScrollPosition), 1, settings_file);
        
        // Check if file is valid
        if (read_count != 1 || 
            test_entry.version != SETTINGS_FILE_VERSION || 
            strnlen(test_entry.filename, MAX_PATH) >= MAX_PATH ||
            strnlen(test_entry.font_path, MAX_PATH) >= MAX_PATH) {
            fclose(settings_file);
            
            // Remove invalid file
            remove(viewer->settings_path);
            return 0;
        }
        fclose(settings_file);
    }
   
    // Open file for writing if it's valid or doesn't exist
    settings_file = fopen(viewer->settings_path, "rb");
    if (!settings_file) {
        return 0;
    }
    
    FileScrollPosition current_entry;
    while (fread(&current_entry, sizeof(FileScrollPosition), 1, settings_file) == 1) {
        
        // Check for matching filename and font path
        if (strcmp(current_entry.filename, viewer->current_file) == 0 &&
            strcmp(current_entry.font_path, viewer->font_path) == 0) {
                // Found matching file and font configuration
                //if (current_entry.font_size > 0 && current_entry.font_size != viewer->font_size) {
                    
                    //reuse previously used font size for this document                  
                    if (viewer->font)
                        TTF_CloseFont(viewer->font);                   
                    viewer->font = TTF_OpenFont(current_entry.font_path, current_entry.font_size);
                    viewer->font_size = current_entry.font_size;
                    //set scroll
                    viewer->scroll_position = current_entry.scroll_position;
                    viewer->scroll_position_adjusted = current_entry.scroll_position_adjusted;
                    viewer->ignore_linebreaks = current_entry.ignore_linebreaks;
                    viewer->inverted_colors = current_entry.inverted_colors;
                                
                //} else {
                    // Direct assignment if font sizes match
                //    viewer->scroll_position = current_entry.scroll_position;
                //    viewer->scroll_position_adjusted = current_entry.scroll_position_adjusted;
                //}
            fclose(settings_file);
            return 1;
        }
    }
    fclose(settings_file);
    return 0;
}

char* resolve_path(const char* path) {
    char* resolved_path = malloc(MAX_PATH);
    if (!resolved_path) return NULL;
    memset(resolved_path, 0, MAX_PATH);

    #ifdef _WIN32
    // Use GetFullPathNameA to resolve the path
    DWORD result = GetFullPathNameA(path, MAX_PATH, resolved_path, NULL);
    if (result == 0 || result >= MAX_PATH) {
        // Fallback to original path if resolution fails
        strncpy(resolved_path, path, MAX_PATH - 1);
    }
    #else
    // Use realpath for Unix-like systems
    char* temp_path = realpath(path, NULL);
    if (temp_path) {
        strncpy(resolved_path, temp_path, MAX_PATH - 1);
        free(temp_path);
    } else {
        // Fallback to original path if resolution fails
        strncpy(resolved_path, path, MAX_PATH - 1);
    }
    #endif

    resolved_path[MAX_PATH - 1] = '\0';
    return resolved_path;
}


// Update destroy_viewer
void destroy_viewer(TextViewer* viewer) {
    if (viewer) {
        if (viewer->font) TTF_CloseFont(viewer->font);
        if (viewer->text) free(viewer->text);
        if (viewer->adjustested_text) free(viewer->adjustested_text);
        free_text_layout(&viewer->normal_layout);
        free_text_layout(&viewer->adjusted_layout);
        free(viewer);
    }
}

void change_font_size(TextViewer* viewer, int new_size) {
    if (new_size <= 0) return;
    
    // Open new font
    TTF_Font* new_font = TTF_OpenFont(viewer->font_path, new_size);
    if (!new_font) return;

    // Save old font position ratios
    float scroll_ratio = 0.0f;
    float scroll_ratio_adjusted = 0.0f;
    
    if (viewer->normal_layout.calculated_total_height > 0) {
        scroll_ratio = (float)viewer->scroll_position / viewer->normal_layout.calculated_total_height;
    }
    if (viewer->adjusted_layout.calculated_total_height > 0) {
        scroll_ratio_adjusted = (float)viewer->scroll_position_adjusted / viewer->adjusted_layout.calculated_total_height;
    }

    // Close old font and set new font
    if (viewer->font) TTF_CloseFont(viewer->font);
    viewer->font = new_font;
    viewer->font_size = new_size;

    // Force recalculation of both layouts
    calculate_text_layout(viewer, &viewer->normal_layout, viewer->text);
    calculate_text_layout(viewer, &viewer->adjusted_layout, viewer->adjustested_text);

    // Restore scroll positions based on ratios
    viewer->scroll_position = (int)(scroll_ratio * viewer->normal_layout.calculated_total_height);
    viewer->scroll_position_adjusted = (int)(scroll_ratio_adjusted * viewer->adjusted_layout.calculated_total_height);

    // Enforce scroll boundaries
    enforce_scroll_boundaries(viewer);
}

// Modified load_text_file to handle different encodings
int load_text_file(TextViewer* viewer, const char* filename, const char* encoding) {
    FILE* file = fopen(filename, "rb");
    if (!file) return 0;

    // Determine file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    // Allocate buffer for original file contents dynamically
    char* original_text = malloc(file_size + 1);
    if (!original_text) {
        fclose(file);
        return 0;
    }

    // Read file contents
    size_t read_len = fread(original_text, 1, file_size, file);
    original_text[read_len] = '\0';

    // Free existing text if any
    if (viewer->text) {
        free(viewer->text);
    }

    // Convert to UTF-8
    char* utf8_text = convert_to_utf8(original_text, read_len, encoding);
    free(original_text);

    if (!utf8_text) {
        fclose(file);
        return 0;
    }

    // Set viewer text to converted UTF-8
    viewer->text = utf8_text;
    viewer->length = strlen(utf8_text);

    if(viewer->adjustested_text)
        free(viewer->adjustested_text);
    
    viewer->adjustested_text = (char*)malloc(viewer->length + 1);

    const char *text_ptr =  viewer->text;
    char *adju_ptr = viewer->adjustested_text;

    //create adjusted text without linebreaks
    char last = '\0';
    while(text_ptr && *text_ptr)
    {
        if((*text_ptr == '\n' || *text_ptr == '\r') && (*(text_ptr+1) == '\n' || *(text_ptr+1) == '\r'))
            *adju_ptr = '\n';
        else if (*text_ptr == '\n' || *text_ptr == '\r')
            if(last != '\n')
                *adju_ptr = ' ';
            else
                *adju_ptr = '\n';
        else
            *adju_ptr = *text_ptr;
        last = *adju_ptr;
        adju_ptr++;
        text_ptr++;
    }
    fclose(file);

    memset(viewer->current_file, 0, MAX_PATH);
    strncpy(viewer->current_file, filename, MAX_PATH - 1);

    // Try to load previous scroll position
    load_scroll_position(viewer);
    return 1;
}

// UTF-8 character length detection
int utf8_char_length(const char* str) {
    unsigned char first_byte = (unsigned char)str[0];
    
    if (first_byte < 0x80) return 1;  // ASCII character
    if (first_byte >= 0xC0 && first_byte <= 0xDF) return 2;  // 2-byte sequence
    if (first_byte >= 0xE0 && first_byte <= 0xEF) return 3;  // 3-byte sequence
    if (first_byte >= 0xF0 && first_byte <= 0xF7) return 4;  // 4-byte sequence
    
    return 1;  // Fallback
}


int find_first_visible_line(TextLayout* layout, int scroll_pos) {
    int left = 0;
    int right = layout->total_lines - 1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        LineInfo* line = get_line_from_layout(layout, mid);
        
        if (!line) break;

        // Check if this line is the first visible line
        if (line->y_position + line->height > scroll_pos) {
            // Check if previous line (if exists) is not visible
            if (mid == 0 || 
                get_line_from_layout(layout, mid-1)->y_position + 
                get_line_from_layout(layout, mid-1)->height <= scroll_pos) {
                return mid;
            }
            // Search in lower half
            right = mid - 1;
        } else {
            // Search in upper half
            left = mid + 1;
        }
    }

    return 0;  // Default to first line if not found
}

void render_text(TextViewer* viewer, SDL_Surface* screen) {
    SDL_Color fg = viewer->text_color;
    SDL_Color bg = viewer->bg_color;

    if(viewer->inverted_colors) {
        fg = viewer->bg_color;
        bg = viewer->text_color;
    }

    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, bg.r, bg.g, bg.b));
   
    TextLayout* layout = viewer->ignore_linebreaks ? 
        &viewer->adjusted_layout : &viewer->normal_layout;
    const char* text = viewer->ignore_linebreaks ? 
        viewer->adjustested_text : viewer->text;
    int scroll_pos = viewer->ignore_linebreaks ? 
        viewer->scroll_position_adjusted : viewer->scroll_position;

    // Find first visible line
    int first_line = find_first_visible_line(layout, scroll_pos);
    
    // Render visible lines
    for (int i = first_line; i < layout->total_lines; i++) {
        LineInfo* line = get_line_from_layout(layout, i);
        if (!line) break;

        int screen_y = line->y_position - scroll_pos;
        
        // Stop if we're past visible area
        if (screen_y >= viewer->window_height) break;

        // Render line if it has content
        if (line->line_length > 0 && screen_y >= -line->height) {
            char line_buffer[1024];
            size_t buf_size = sizeof(line_buffer) - 1;
            size_t copy_len = line->line_length;
            if (copy_len > buf_size) {
                copy_len = buf_size;
            }
            memcpy(line_buffer, text + line->line_start_offset, copy_len);
            line_buffer[copy_len] = '\0';

            SDL_Surface* text_surface = TTF_RenderUTF8_Blended(viewer->font, line_buffer, fg);
            if (text_surface) {
                SDL_Rect dest = {MARGINS, screen_y, 0, 0};
                SDL_BlitSurface(text_surface, NULL, screen, &dest);
                SDL_FreeSurface(text_surface);
            }
        }
    }
    SDL_Flip(screen);
}

// Function to trim whitespace from both ends of a string
void trim(char* str) {
    char* start = str;
    char* end = str + strlen(str) - 1;

    // Trim leading whitespace
    while (isspace(*start)) start++;

    // Trim trailing whitespace
    while (end > start && isspace(*end)) end--;

    // Shift the trimmed string to the beginning
    memmove(str, start, end - start + 1);
    str[end - start + 1] = '\0';
}

// Parse a color string in format "r,g,b"
int parse_color(const char* color_str, SDL_Color* color) {
    char copy[64];
    strncpy(copy, color_str, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    trim(copy);

    char* r_str = strtok(copy, ",");
    char* g_str = strtok(NULL, ",");
    char* b_str = strtok(NULL, ",");

    if (!r_str || !g_str || !b_str) return 0;

    color->r = atoi(r_str);
    color->g = atoi(g_str);
    color->b = atoi(b_str);
    color->unused = 0;

    return 1;
}

// Read configuration from file
int read_config(const char* config_path, ViewerConfig* config) {
    FILE* conf_file = fopen(config_path, "r");
    if (!conf_file) return 0;

    char line[256];
    while (fgets(line, sizeof(line), conf_file)) {
        // Trim line
        trim(line);

        // Skip empty or comment lines
        if (line[0] == '\0' || line[0] == '#') continue;

        // Split key and value
        char* separator = strchr(line, '=');
        if (!separator) continue;

        *separator = '\0';
        char* key = line;
        char* value = separator + 1;

        // Trim key and value
        trim(key);
        trim(value);

        // Parse configuration options
        if (strcmp(key, "font") == 0) {
            if(is_ttf_file(value)) {
                snprintf(config->font_path, MAX_PATH, "%s", value);
            }
        }
        else if (strcmp(key, "ignore_linebreaks") == 0) {
            config->ignore_linebreaks = atoi(value);
        }
        else if (strcmp(key, "inverted_colors") == 0) {
            config->inverted_colors = atoi(value);
        } 
        else if (strcmp(key, "font_size") == 0) {
            config->font_size = atoi(value);
        }
        else if (strcmp(key, "bg_color") == 0) {
            parse_color(value, &config->bg_color);
        }
        else if (strcmp(key, "text_color") == 0) {
            parse_color(value, &config->text_color);
        }
        else if (strcmp(key, "encoding") == 0) {
            strncpy(config->encoding, value, sizeof(config->encoding) - 1);
            config->encoding[sizeof(config->encoding) - 1] = '\0';
        }
    }

    fclose(conf_file);
    return 1;
}

void print_usage(const char* program_name) {
    printf("Usage: %s <text_file> [-conf=path/to/config] [font_path] [font_size] [bg_r,g,b] [text_r,g,b] [encoding] [-ignore_linebreaks] [-inverted_colors]\n", program_name);
    printf("  text_file: Path to the text file to display (required)\n");
    printf("  -conf=path: Optional configuration file path\n");
    printf("  font_path: Path to TTF font file\n");
    printf("  font_size: Default value for font size\n");
    printf("  bg_r,g,b: Background color RGB values 0-255\n");
    printf("  text_r,g,b: Text color RGB values 0-255\n");
    printf("  encoding: Text file encoding (e.g., UTF-8, ISO-8859-1)\n");
    printf("  -ignore_linebreaks: Default value for Ignore original line breaks and fill window width\n");
    printf("  -inverted_colors: Default value for inverted (switched bg & text color)\n");
}

// Add a helper function to check if a file is likely a TTF font
int is_ttf_file(const char* filename) {
    // Find the last dot in the filename
    const char* dot = strrchr(filename, '.');
    
    // If no dot found or filename ends with dot, it's not a valid font file
    if (!dot || dot == filename) return 0;
    
    // Compare extension case-insensitively
    return (strcasecmp(dot, ".ttf") == 0);
}

void enforce_scroll_boundaries(TextViewer* viewer) {
    int max_scroll = MAX(0, viewer->normal_layout.calculated_total_height - viewer->window_height);
    int max_scroll_adjusted = MAX(0, viewer->adjusted_layout.calculated_total_height - viewer->window_height);
    
    viewer->scroll_position = MAX(0, MIN(viewer->scroll_position, max_scroll));
    viewer->scroll_position_adjusted = MAX(0, MIN(viewer->scroll_position_adjusted, max_scroll_adjusted));
}

int main(int argc, char* argv[]) {
    // Default configuration
    ViewerConfig config = {
        .font_path = DEFAULT_FONT,
        .font_size = DEFAULT_FONT_SIZE,
        .bg_color = {255, 255, 255, 0},  // White
        .text_color = {0, 0, 0, 0},      // Black
        .encoding = "UTF-8",
        .ignore_linebreaks = 0,
        .inverted_colors = 0
    };
    SDL_Color* current_color = NULL;
    char* config_file = NULL;
    char* text_file = NULL;

    int width = DEFAULT_WIDTH;
    int height = DEFAULT_HEIGHT;
    int fullscreen = 0;

    // First pass: identify files
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-conf=", 6) == 0) {
            config_file = resolve_path(argv[i] + 6);
        }
        else if (strncmp(argv[i], "-w=", 3) == 0) {
            width = atoi(argv[i] + 3);
        }
        else if (strncmp(argv[i], "-h=", 3) == 0) {
            height = atoi(argv[i] + 3);
        }
        else if (strcmp(argv[i], "-fullscreen") == 0) {
            fullscreen = 1;
        }
        else if (!is_ttf_file(argv[i]) && !text_file) {
            text_file = resolve_path(argv[i]);
        }
    }

    // Validate minimum arguments
    if (!text_file) {
        print_usage(argv[0]);
        return 1;
    }

    // Read configuration file if specified
    if (config_file) {
        read_config(config_file, &config);
    }

    // Override config with command-line parameters
    for (int i = 1; i < argc; i++) {
        if (is_ttf_file(argv[i])) {
            // Use provided TTF font
            char* tmp = resolve_path(argv[i]);
            snprintf(config.font_path, MAX_PATH, "%s", tmp);
            free(tmp);
        }
        else if (strcmp(argv[i], "-ignore_linebreaks") == 0) {
            config.ignore_linebreaks = 1;
        }        
        else if (strcmp(argv[i], "-inverted_colors") == 0) {
            config.inverted_colors = 1;
        }        
        else if (!text_file) {
            text_file = resolve_path(argv[i]);
        }
        else if (strchr(argv[i], ',')) {
            // Alternate between background and text color
            if (current_color == NULL || current_color == &config.text_color) {
                // Try to parse as background color
                if (parse_color(argv[i], &config.bg_color)) {
                    config.bg_color.unused = 0;
                    current_color = &config.bg_color;
                }
            }
            else if (current_color == &config.bg_color) {
                // Try to parse as text color
                if (parse_color(argv[i], &config.text_color)) {
                    config.text_color.unused = 0;
                    current_color = &config.text_color;
                }
            }
        }
        else {
            // Try to parse as font size
            int value = atoi(argv[i]);
            if (value > 0) {
                config.font_size = value;
            }
        }
    }

    // Validate minimum arguments
    if (!text_file) {
        print_usage(argv[0]);
        return 1;
    }

    // SDL and TTF initialization
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }

    if (TTF_Init() < 0) {
        printf("TTF initialization failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    InteralFont = load_font_from_memory(font_data, font_data_size, 14);
    if (!InteralFont) {
        printf("Failed to load font: %s\n", TTF_GetError());
        TTF_Quit();
        TTF_Quit();
        return 1;
    }

    char settings_path[MAX_PATH];
    char* home = getenv("HOME");
   
    if (home) {
        snprintf(settings_path, MAX_PATH, "%s/%s", home, SETTINGS_DIR);
        ensure_settings_dir(settings_path);
        // Create full path to settings file
        snprintf(settings_path, MAX_PATH, "%s/%s/%s", home, SETTINGS_DIR, SETTINGS_FILE);
    } else {
        snprintf(settings_path, MAX_PATH, "%s", SETTINGS_DIR);
        ensure_settings_dir(settings_path);
        // Create full path to settings file
        snprintf(settings_path, MAX_PATH, "./%s/%s", SETTINGS_DIR, SETTINGS_FILE);
    }
    
    printf("Using Settings PAth: %s\n", settings_path);

    
    // Set video mode
    Uint32 flags = SDL_SWSURFACE;
    if (fullscreen)
        flags |= SDL_FULLSCREEN;

    SDL_Surface* screen = SDL_SetVideoMode(width, height, 32, flags);
    if (!screen) {
        printf("Failed to set video mode: %s\n", SDL_GetError());
        printf("Window width: %d, height: %d\n", width, height);
        TTF_CloseFont(InteralFont);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, config.bg_color.r, config.bg_color.b, config.bg_color.b));
    display_message("Creating Viewer", 1000, width >> 1, height >> 1, 5, config.bg_color, config.text_color);
    draw_display_message(screen);
    SDL_Flip(screen);

    // Create viewer with configuration
    TextViewer* viewer = create_viewer(settings_path, config.font_path, config.font_size, 
        width,height, config.text_color, config.bg_color, config.ignore_linebreaks, config.inverted_colors);

    if (!viewer) {
        printf("Failed to create viewer\n");
        SDL_FreeSurface(screen);
        TTF_CloseFont(InteralFont);
        TTF_Quit();
        SDL_Quit();       
        return 1;
    }
    
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, config.bg_color.r, config.bg_color.b, config.bg_color.b));
    display_message("Loading TXT File", 1000, width >> 1, height >> 1, 5, config.bg_color, config.text_color);
    draw_display_message(screen);
    SDL_Flip(screen);

    // Load text file with specified encoding
    if (!load_text_file(viewer, text_file, config.encoding)) {
        printf("Failed to load text file: %s\n", text_file);
        printf("Current file path: %s\n", viewer->current_file);
        printf("Text length: %zu\n", viewer->length);
        destroy_viewer(viewer);
        SDL_FreeSurface(screen);
        TTF_CloseFont(InteralFont);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }                        
    
    // Set window title to filename
    SDL_WM_SetCaption(text_file, NULL);

    // Disable mouse cursor
    SDL_ShowCursor(SDL_DISABLE);

    // Enable key repeat and Unicode
    SDL_EnableUNICODE(1);
    SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);

    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, config.bg_color.r, config.bg_color.b, config.bg_color.b));
    display_message("Calculating Layouts", 1000, width >> 1, height >> 1, 5, config.bg_color, config.text_color);
    draw_display_message(screen);
    SDL_Flip(screen);

    // Calculate the layouts
    calculate_text_layout(viewer, &viewer->normal_layout, viewer->text);
    calculate_text_layout(viewer, &viewer->adjusted_layout, viewer->adjustested_text);
    
    stop_display_message();

    // Render once
    render_text(viewer, screen);

    
    // Main event loop
    int running = 1;
    char msg[1024];
    SDL_Event event;
    while (running) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {                
                case SDL_QUIT:
                    running = 0;
                    break;
                case SDL_KEYDOWN:
                    switch (event.key.keysym.sym) {
                        case SDLK_a:
                            sprintf(msg, "Reloading (font size %d)", viewer->font_size);
                            display_message(msg, 1000, viewer->window_width >> 1, viewer->window_height >> 1, 5, config.bg_color, config.text_color);
                            draw_display_message(screen);                            
                            SDL_Flip(screen);
                        
                            change_font_size(viewer, viewer->font_size + 1);
                            render_text(viewer, screen);
                            break;
                        case SDLK_b:
                            sprintf(msg, "Reloading (font size %d)", viewer->font_size);
                            display_message(msg, 1000, viewer->window_width >> 1, viewer->window_height >> 1, 5, config.bg_color, config.text_color);
                            draw_display_message(screen);                            
                            SDL_Flip(screen);

                            change_font_size(viewer, viewer->font_size - 1);
                            render_text(viewer, screen);                                                                
                            break;
                        case SDLK_x:  // swap background / foreground
                            viewer->inverted_colors = !viewer->inverted_colors;
                            render_text(viewer, screen);
                            break;
                        case SDLK_y:  // Toggle ignore linebreaks mode
                            viewer->ignore_linebreaks = !viewer->ignore_linebreaks;
                            render_text(viewer, screen);
                            break;                         
                        case SDLK_k:
                        case SDLK_HOME:
                            // Jump to the beginning of the text
                            if(viewer->ignore_linebreaks)
                                viewer->scroll_position_adjusted = 0;
                            else
                                viewer->scroll_position = 0;
                            render_text(viewer, screen);
                            break;
                        case SDLK_s:
                        case SDLK_END:
                            // Jump to end of text
                            if(viewer->ignore_linebreaks)
                                viewer->scroll_position_adjusted = MAX(0, viewer->adjusted_layout.calculated_total_height - viewer->window_height);
                            else
                                viewer->scroll_position = MAX(0, viewer->normal_layout.calculated_total_height - viewer->window_height);
                            render_text(viewer, screen);
                            break;
                        case SDLK_u:
                        case SDLK_UP:
                            if(viewer->ignore_linebreaks)
                                viewer->scroll_position_adjusted = MAX(0, viewer->scroll_position_adjusted - viewer->font_size);
                            else
                                viewer->scroll_position = MAX(0, viewer->scroll_position - viewer->font_size);
                            render_text(viewer, screen);
                            break;
                        case SDLK_d:
                        case SDLK_DOWN:
                            if(viewer->ignore_linebreaks)
                                viewer->scroll_position_adjusted = MIN(viewer->adjusted_layout.calculated_total_height - viewer->window_height, viewer->scroll_position_adjusted + viewer->font_size);
                            else
                                viewer->scroll_position = MIN(viewer->normal_layout.calculated_total_height - viewer->window_height, viewer->scroll_position + viewer->font_size);
                            render_text(viewer, screen);
                            break;
                        case SDLK_m:
                        case SDLK_PAGEUP:
                            if(viewer->ignore_linebreaks)
                                viewer->scroll_position_adjusted = MAX(0, viewer->scroll_position_adjusted - viewer->window_height);
                            else
                                viewer->scroll_position = MAX(0, viewer->scroll_position - viewer->window_height);
                            render_text(viewer, screen);
                            break;
                        case SDLK_n:
                        case SDLK_PAGEDOWN:
                            if(viewer->ignore_linebreaks)
                                viewer->scroll_position_adjusted = MIN(viewer->adjusted_layout.calculated_total_height - viewer->window_height, viewer->scroll_position_adjusted + viewer->window_height);
                            else
                                viewer->scroll_position = MIN(viewer->normal_layout.calculated_total_height - viewer->window_height, viewer->scroll_position + viewer->window_height);
                            render_text(viewer, screen);
                            break;
                        case SDLK_q:
                        case SDLK_ESCAPE:
                            running = 0;
                            break;
                        default:
                            break;
                    }
                    break;
            }
        }
        SDL_Flip(screen);
        SDL_Delay(16);
    }

    // Save scroll position before exiting
    save_scroll_position(viewer);

    
    // Cleanup
    destroy_viewer(viewer);
    if(config_file)
        free(config_file);
    if(text_file)
        free(text_file);
    TTF_Quit();
    SDL_Quit();
    return 0;
}