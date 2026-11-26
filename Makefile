# Lane Rush - build with:  make      (.3dsx)
#                          make cia  (.cia, needs makerom)
ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM is not set. Install devkitPro with the 3ds-dev package group")
endif

include $(DEVKITARM)/3ds_rules

TARGET          := LaneRush
APP_TITLE       := Lane Rush
APP_DESCRIPTION := A small lane battle game
APP_AUTHOR      := Homebrew
ICON            := $(CTRULIB)/default_icon.png
MAKEROM         ?= makerom

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) \
           -D__3DS__ -I$(CTRULIB)/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH)
LIBS    := -L$(CTRULIB)/lib -lcitro2d -lcitro3d -lctru -lm

.PHONY: all cia clean

all: $(TARGET).3dsx

# If main.c is missing, look for it under another name or in a folder.
# A copy that already has the color fix is preferred.
main.c:
	@list=$$(mktemp); \
	find . -type f \( -iname '*main*.c' -o -iname '*main*.c.txt' \) ! -path './.git/*' | sort > $$list; \
	f=$$(while IFS= read -r x; do grep -q 'define RGBA(r, g, b, a) ((u32)' "$$x" && echo "$$x"; done < $$list | head -n 1); \
	[ -n "$$f" ] || f=$$(head -n 1 $$list); \
	rm -f $$list; \
	if [ -z "$$f" ]; then \
		echo "No main.c found. Files in this repository:"; \
		find . -type f ! -path './.git/*'; \
		exit 1; \
	fi; \
	echo "Using $$f as main.c"; \
	cp "$$f" main.c

main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: main.o
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(TARGET).smdh: $(ICON)
	smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $< $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	3dsxtool $< $@ --smdh=$(TARGET).smdh

cia: $(TARGET).cia

$(TARGET).cia: $(TARGET).elf $(TARGET).smdh app.rsf
	$(MAKEROM) -f cia -o $@ -elf $(TARGET).elf -rsf app.rsf \
		-icon $(TARGET).smdh -exefslogo -target t

clean:
	rm -f main.o $(TARGET).elf $(TARGET).smdh $(TARGET).3dsx $(TARGET).cia
