CC = cc
CFLAGS = -I./main -I./main/lxdialog/ -DCURSES_LOC="<ncurses.h>" -MMD -MP
LD = $(CC)
LDFLAGS = 
LIBS = 
CONF_LIBS = 
MCONF_LIBS = -lncursesw
STRIP = strip
STRIPFLAGS = 
OBJDUMP = objdump
OBJDUMPFLAGS = 
CONF_OUTFILE = conf
MCONF_OUTFILE = mconf
SUBDIR = scripts/kconfig
