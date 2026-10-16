# Make 'all' the default goal no matter where other targets appear.
.DEFAULT_GOAL := all
.PHONY: all clean convert-music convert-images

EE_BIN  = PrismELF.elf
IRX     = iomanX fileXio sio2man padman usbd bdm bdmfs_fatfs usbmass_bd freesd audsrv
ASSETS  = logo bg splashbg file folder elf audio image video text archive
SRC     = main gfx pad fs audio ui launch log ftype imgdec imgextra flacdec sevenz compress viewer settings

EE_OBJS = $(SRC:%=src/%.o) $(IRX:%=gen/irx_%.o) $(ASSETS:%=gen/img_%.o)
# gsKit lives outside the ps2sdk include dirs (the ps2dev image sets $GSKIT).
GSKIT ?= $(PS2DEV)/gsKit
EE_CFLAGS += -I$(GSKIT)/include -I$(GSKIT)/ee/dma/include -I$(GSKIT)/ee/gs/include -I$(GSKIT)/ee/toolkit/include
# Use the loader variant without the rainbow border colours when this SDK ships it.
ELFLOADER := $(if $(wildcard $(PS2SDK)/ee/lib/libelf-loader-nocolour.a),-lelf-loader-nocolour,-lelf-loader)
EE_LIBS = -L$(GSKIT)/lib -L$(PS2SDK)/ports/lib -lgskit_toolkit -lgskit -ldmakit -lpad -lmc -lfileXio -lpatches -laudsrv $(ELFLOADER) -ldebug
EE_CFLAGS += -O2 -Wall -Isrc
# CD/DVD browsing: the SDK's cdfs.irx (ISO9660 as "cdfs:") plus libcdvd for the disc-type check. Both optional, so the build never breaks without them.
ifneq ($(wildcard $(PS2SDK)/iop/irx/cdfs.irx),)
IRX       += cdfs
EE_CFLAGS += -DHAVE_CDFS_IRX
endif
ifneq ($(wildcard $(PS2SDK)/ee/lib/libcdvd.a),)
EE_CFLAGS += -DHAVE_CDVD
EE_LIBS   += -lcdvd
endif
# Newer ps2sdk blocks direct fileXio use unless the code declares it knows about the newlib port.
# We use fileXio directly on purpose (no POSIX file calls).
EE_CFLAGS += -DNEWLIB_PORT_AWARE

# Optional codecs from ps2sdk-ports: used only if the headers exist, so the build never breaks without them.
EE_CFLAGS += -I$(PS2SDK)/ports/include
ifneq ($(wildcard $(PS2SDK)/ports/include/png.h),)
EE_CFLAGS += -DHAVE_LIBPNG
EE_LIBS   += -lpng -lz
endif
# zip extraction needs zlib (it ships with the PNG port)
ifneq ($(wildcard $(PS2SDK)/ports/include/zlib.h),)
EE_CFLAGS += -DHAVE_ZLIB
EE_LIBS   += -lz
endif
ifneq ($(wildcard $(PS2SDK)/ports/include/jpeglib.h),)
EE_CFLAGS += -DHAVE_LIBJPEG
EE_LIBS   += -ljpeg
endif
ifneq ($(wildcard $(PS2SDK)/ports/include/mad.h),)
EE_CFLAGS += -DHAVE_LIBMAD
EE_LIBS   += -lmad
endif
EE_LIBS   += -lm -lc -lkernel   # system libs last so codec libs can resolve against them
EE_CFLAGS += -Wno-format-truncation -Wno-stringop-truncation
# Uncomment to use SifLoadElf+ExecPS2 instead of libelf-loader:
# EE_CFLAGS += -DUSE_SIFLOADELF
BIN2C = $(PS2SDK)/bin/bin2c

# Optional music baked into the ELF. Drop an mp3/wav/ogg/flac/... into music/ and run
#   python3 tools/covert-bgm.py     (CI does this) -> gen_music/bgm.wav (16-bit PCM, PS2-ready)
# The Makefile embeds gen_music/*.wav, or else a ready-made music/*.wav.
# Nothing there = nothing embedded; the app then looks on USB / mc0 / mc1 at runtime.
BGM_WAV := $(firstword $(wildcard gen_music/*.wav))
ifeq ($(BGM_WAV),)
BGM_WAV := $(firstword $(wildcard music/*.wav))
endif
ifneq ($(BGM_WAV),)
EE_OBJS   += gen/bgm_embedded.o
EE_CFLAGS += -DHAVE_EMBEDDED_BGM
gen/bgm_embedded.c: $(BGM_WAV)
	@mkdir -p gen
	$(BIN2C) "$<" $@ bgm_wav
endif

convert-music:
	python3 tools/covert-bgm.py

convert-images:
	python3 tools/covert-images.py

all: $(EE_BIN)

gen/irx_%.c: $(PS2SDK)/iop/irx/%.irx
	@mkdir -p gen
	$(BIN2C) $< $@ $*_irx

gen/img_%.c: assets/%.rgba
	@mkdir -p gen
	$(BIN2C) $< $@ $*_rgba

.SECONDARY:

clean:
	rm -rf gen src/*.o $(EE_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
