PKGDIR ?= ../..
L4DIR  ?= $(PKGDIR)/../..

SYSTEMS	:= x86-l4f amd64-l4f arm-l4f arm64-l4f

TARGET  = emmc-drv
SRC_CC  = main.cc \
	  cmd.cc \
	  cpg.cc \
	  drv_sdhci.cc \
	  drv_sdhi.cc \
	  mmc.cc \
	  mmio.cc \
	  util.cc

REQUIRES_LIBS  := libio-vbus libblock-device libpthread

include $(L4DIR)/mk/prog.mk

#WARNINGS += -Werror
