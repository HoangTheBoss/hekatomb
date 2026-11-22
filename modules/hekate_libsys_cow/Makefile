ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/base_rules

TARGET := libsys_cow
BUILDDIR := ../../build/$(TARGET)
OUTPUTDIR := ../../output
BDKDIR := bdk
BDKINC := -I../../$(BDKDIR)
VPATH = $(dir ./)

# Track compiler flags
TRACK_CFLAGS = $(BUILDDIR)/.cflags
TRACK_LDFLAGS = $(BUILDDIR)/.ldflags

OBJS = $(addprefix $(BUILDDIR)/,\
	sys_cow.o \
)

FFCFG_INC := '"../bootloader/libs/fatfs/ffconf.h"'
CUSTOMDEFINES := -DCOW_BSO_MODULE=1 -DFFCFG_INC=$(FFCFG_INC)

ARCH := -march=armv4t -mtune=arm7tdmi -mthumb-interwork
CFLAGS = $(ARCH) -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -nostdlib -fpie -fvisibility=hidden -ffunction-sections -fdata-sections -fomit-frame-pointer -std=gnu11 -Wall -Wsign-compare $(CUSTOMDEFINES)
LDFLAGS = $(ARCH) -fpie -pie -Wl,-Bsymbolic -nostartfiles -lgcc -Wl,-z,max-page-size=256

.PHONY: clean all

all: $(TARGET).bso
$(BUILDDIR)/%.o: ./%.c $(TRACK_CFLAGS)
	@$(CC) $(CFLAGS) $(BDKINC) -MMD -MP -c $< -o $@

$(TARGET).bso: $(OBJS) $(TRACK_LDFLAGS)
	@$(CC) $(LDFLAGS) -e cow_entry $(OBJS) -o $(OUTPUTDIR)/$(TARGET).bso
	@$(STRIP) -g $(OUTPUTDIR)/$(TARGET).bso
	@echo "Built module: "$(TARGET)".bso"

clean:
	@rm -rf $(BUILDDIR)
	@rm -rf $(OUTPUTDIR)/$(TARGET).bso

$(BUILDDIR):
	@mkdir -p "$(BUILDDIR)"

# Non objects change detectors.
$(TRACK_CFLAGS): $(BUILDDIR)
	@echo '$(CFLAGS)' | cmp -s - $@ || echo '$(CFLAGS)' > $@
$(TRACK_LDFLAGS): $(BUILDDIR)
	@echo '$(LDFLAGS)' | cmp -s - $@ || echo '$(LDFLAGS)' > $@
-include $(OBJS:.o=.d)
