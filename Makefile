DEBUG = 1
SRC_DIR = src
OBJ_DIR = ./obj
EXE=viewtxt

SRC=$(wildcard *.c $(foreach fd, $(SRC_DIR), $(fd)/*.c)) 
OBJS=$(addprefix $(OBJ_DIR)/, $(SRC:.c=.o))


CC ?= gcc
SDL_CONFIG ?= sdl-config
CFLAGS ?= -Wall -Wextra
LDFLAGS ?= 

ifeq ($(DEBUG),1)
CFLAGS += -g
else
CFLAGS += -O2
endif

ifdef TARGET
include $(TARGET).mk
endif

CFLAGS += `$(SDL_CONFIG) --cflags`
LDFLAGS += `$(SDL_CONFIG) --libs` -lSDL_ttf

.PHONY: all clean

all: $(EXE)

$(EXE): $(OBJS)
	$(CC) $(CFLAGS) $(TARGET_ARCH) $^ $(LDFLAGS) -o $@ 

$(OBJ_DIR)/%.o: %.c
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $@

clean:
	$(RM) -rv *~ $(OBJS) $(EXE)
