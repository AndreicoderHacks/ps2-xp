EE_BIN = ps2xp.elf
EE_OBJS = src/main.o usbd_irx.o ps2mouse_irx.o ps2kbd_irx.o sio2man_irx.o padman_irx.o iomanX_irx.o fileXio_irx.o bdm_irx.o bdmfs_fatfs_irx.o usbmass_bd_irx.o libsd_irx.o audsrv_irx.o
EE_INCS += -I$(PS2DEV)/gsKit/include
EE_LDFLAGS += -L$(PS2DEV)/gsKit/lib
EE_LIBS = -lgskit -ldmakit -lmouse -lkbd -laudsrv -lpad -lfileXio -lpatches -ldebug -lm -lc

all: $(EE_BIN)

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

clean:
	rm -f $(EE_BIN) $(EE_OBJS) *_irx.c

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
