ifeq ($(CONFIG_AC97),)
SRC := $(filter-out ac97.c io/ac97.c,$(SRC))
endif
ifeq ($(CONFIG_BGA),)
SRC := $(filter-out bga.c io/bga.c,$(SRC))
endif
ifeq ($(CONFIG_E1000),)
SRC := $(filter-out e1000.c io/e1000.c,$(SRC))
endif
ifeq ($(CONFIG_RTL8139),)
SRC := $(filter-out rtl8139.c io/rtl8139.c,$(SRC))
endif
ifeq ($(CONFIG_SVGA_II),)
SRC := $(filter-out svga_ii.c io/svga_ii.c,$(SRC))
endif
ifeq ($(CONFIG_VIRTIO_GPU),)
SRC := $(filter-out virtio_gpu.c io/virtio_gpu.c,$(SRC))
endif
ifeq ($(CONFIG_IDE),)
SRC := $(filter-out ide.c io/ide.c,$(SRC))
endif
ifeq ($(CONFIG_ATAPI),)
SRC := $(filter-out atapi.c io/atapi.c,$(SRC))
endif
ifeq ($(CONFIG_PATA),)
SRC := $(filter-out pata.c io/pata.c,$(SRC))
endif
ifeq ($(CONFIG_AHCI),)
SRC := $(filter-out ahci.c io/ahci.c,$(SRC))
endif
ifeq ($(CONFIG_SATA),)
SRC := $(filter-out sata.c io/sata.c,$(SRC))
endif
ifeq ($(CONFIG_MBR),)
SRC := $(filter-out mbr.c io/mbr.c,$(SRC))
endif
ifeq ($(CONFIG_GPT),)
SRC := $(filter-out gpt.c io/gpt.c,$(SRC))
endif
ifeq ($(CONFIG_UHCI),)
SRC := $(filter-out uhci.c io/uhci.c,$(SRC))
endif
ifeq ($(CONFIG_OHCI),)
SRC := $(filter-out ohci.c io/ohci.c,$(SRC))
endif
ifeq ($(CONFIG_USB_KEYBOARD),)
SRC := $(filter-out usb_keyboard.c io/usb_keyboard.c,$(SRC))
endif
ifeq ($(CONFIG_VFAT),)
SRC := $(filter-out vfat.c io/vfat.c,$(SRC))
endif
ifeq ($(CONFIG_EXT4),)
SRC := $(filter-out ext4.c io/ext4.c,$(SRC))
endif
ifeq ($(CONFIG_ISO9660),)
SRC := $(filter-out iso9660.c io/iso9660.c,$(SRC))
endif
ifeq ($(CONFIG_OOM_KILLER),)
SRC := $(filter-out oom.c mm/oom.c,$(SRC))
endif
