# ALVR PS4 client — build with OpenOrbis PS4 Toolchain (run inside WSL/Linux).
#   make            -> build .pkg
#   make clean

TITLE       := ALVR PS4
VERSION     := 01.28
TITLE_ID    := ALVR00001
CONTENT_ID  := IV0000-ALVR00001_00-ALVRPS4CLIENT000

# param.sfo ATTRIBUTE flags, deduced from installed PSVR games' param.sfo:
#   0x00004000 = supports PlayStation VR, 0x04000000 = PlayStation VR required
#   (the system asks for the headset at launch).
ATTRIBUTE   := 0x04004000

# SDK version reported to system libraries (sceKernelGetCompiledSdkVersion). libSceHmd
# zeroes the 2D VR screen size/distance for SDK < 4.50; use Beat Saber's (5.50).
SDK_VERSION := 0x05508001

LIBS        := -lc -lkernel -lc++ -lSceVideoOut -lSceNetCtl -lSceSysmodule -lSceUserService -lSceSystemService -lSceGnmDriver -lScePad

TOOLCHAIN   ?= $(OO_PS4_TOOLCHAIN)
BIN         := $(TOOLCHAIN)/bin/linux
BUILD       := build

SRCS        := $(wildcard src/*.cpp)
OBJS        := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))
ASSETS      := $(shell find assets -type f 2>/dev/null)
LIBMODULES  := $(wildcard sce_module/*)

CC          := clang
CXX         := clang++
LD          := ld.lld

CXXFLAGS    := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall \
               -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -isystem $(TOOLCHAIN)/include/c++/v1
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(BIN)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png $(LIBMODULES) $(ASSETS)
	$(BIN)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

sce_sys/param.sfo: Makefile
	$(BIN)/PkgTool.Core sfo_new $@
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(BIN)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value $(ATTRIBUTE)
	$(BIN)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(BIN)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o $(BUILD)/alvr_ps4.elf $(LDFLAGS)
	$(BIN)/create-fself -in=$(BUILD)/alvr_ps4.elf -out=$(BUILD)/alvr_ps4.oelf --eboot "eboot.bin" --paid 0x3800000000000011 --sdkver $(SDK_VERSION)

$(BUILD)/%.o: src/%.cpp $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD) eboot.bin pkg.gp4 sce_sys/param.sfo *.pkg

.PHONY: all clean
