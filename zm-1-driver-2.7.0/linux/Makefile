KVERSION := $(shell uname -r)
Z_PROTOCOL_ENGINE_PATH  := $(src)/../common/ZProtocolEngine
Z_PROTOCOL_SRCS         := z_Fifo.c z_Command.c z_ProtocolEngine.c z_CommandEngine.c z_Utils.c
Z_PROTOCOL_OBJS         := $(addprefix $(Z_PROTOCOL_ENGINE_PATH)/,$(Z_PROTOCOL_SRCS:.c=.o))

$(info $(Z_PROTOCOL_OBJS))

ccflags-y := -std=gnu11 -Wno-declaration-after-statement -Wno-unused-function 


obj-m += zm_1_driver.o
zm_1_driver-objs   := src/module.o src/device.o src/ftdi.o src/usb.o src/snd.o src/chardev.o src/mixer.o src/defs.o $(Z_PROTOCOL_OBJS)

ccflags-y += -DLINUX_KERNEL_MODULE=1 -I$(src)/../common/ZProtocolEngine

all: build

build:
	cp -f $(PWD)/linux/src/defs.o_shipped $(PWD)/linux/src/defs.o
	touch $(PWD)/linux/src/.defs.o.cmd
	make -C /lib/modules/$(KVERSION)/build M=$(PWD)/linux modules
	cp *.ko ../

clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD)/linux clean
	rm -rf $(Z_PROTOCOL_OBJS)

.PHONY: all build clean
