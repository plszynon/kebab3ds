ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM is not set - run inside the devkitpro/devkitarm image")
endif

TARGET   := kebab-klikacz
BUILD_NUM ?= 0
REPO     ?= plszynon/kebab3ds
SOURCES  := source
CTRULIB  := $(DEVKITPRO)/libctru
PORTLIBS := $(DEVKITPRO)/portlibs/3ds

CC       := $(DEVKITARM)/bin/arm-none-eabi-gcc
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) -std=gnu11 -D__3DS__ -DBUILD_NUM=$(BUILD_NUM) -DUPDATE_REPO=\"$(REPO)\" \
            -I$(CTRULIB)/include -I$(PORTLIBS)/include
LDFLAGS  := -specs=3dsx.specs -g $(ARCH) -L$(CTRULIB)/lib -L$(PORTLIBS)/lib
LIBS     := -lcitro2d -lcitro3d -lctru -lm

CFILES   := $(wildcard $(SOURCES)/*.c)
OBJS     := $(CFILES:.c=.o)

all: $(TARGET).3dsx

%.o: %.c $(SOURCES)/data.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

icon.png:
	cp $(CTRULIB)/default_icon.png icon.png

$(TARGET).smdh: icon.png
	smdhtool --create "Kebab Klikacz 2" "Klikacz kebabow na 3DS" "plszynon" icon.png $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	3dsxtool $< $@ --smdh=$(TARGET).smdh

clean:
	rm -f $(SOURCES)/*.o $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh

.PHONY: all clean
