PKGDIR ?= ../..
L4DIR  ?= $(PKGDIR)/../..

TARGET  = emmc-drv
SRC_CC  = main.cc \
	  cmd.cc \
	  cpg.cc \
	  drv.cc \
	  drv_sdhci.cc \
	  drv_sdhi.cc \
	  mmc.cc \
	  mmio.cc \
	  util.cc

REQUIRES_LIBS  := libio-vbus libblock-device libpthread

include $(L4DIR)/mk/prog.mk

#WARNINGS += -Werror
