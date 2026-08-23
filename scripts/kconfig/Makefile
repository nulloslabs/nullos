ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

$(shell test -d main/.git || { printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/main/" >&2; git submodule update --init --depth 1 -- main > /dev/null 2>&1; })

CONF_SRC := main/conf.c main/zconf.tab.c
CONF_OBJ := $(CONF_SRC:.c=.o)
CONF_DEP := $(CONF_OBJ:.o=.d)

MCONF_SRC := main/mconf.c main/zconf.tab.c $(wildcard main/lxdialog/*.c)
MCONF_OBJ := $(MCONF_SRC:.c=.o)
MCONF_DEP := $(MCONF_OBJ:.o=.d)

-include $(CONF_DEP)
-include $(MCONF_DEP)

.DEFAULT_GOAL := all

all: kconfig

gen:
	@for f in zconf.tab.c zconf.lex.c zconf.hash.c; do \
		if [ ! -e "main/$$f" ]; then \
			printf "  %-7s %s\n" "GEN" "$(SUBDIR)/main/$$f" && \
			cp "main/$${f}_shipped" "main/$$f"; \
		fi; \
	done

kconfig:
	@$(MAKE) gen && \
	$(MAKE) $(CONF_OBJ) $(MCONF_OBJ) && \
	if [ ! -f "main/$(CONF_OUTFILE)" ] || [ config.mk -nt main/$(CONF_OUTFILE) ] || [ Makefile -nt main/$(CONF_OUTFILE) ] || ( for o in $(CONF_OBJ); do [ "$$o" -nt main/$(CONF_OUTFILE) ] && exit 0; done; exit 1 ); then \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/main/$(CONF_OUTFILE)" && \
		$(LD) $(LDFLAGS) $(CONF_OBJ) -o main/$(CONF_OUTFILE) $(CONF_LIBS) $(LIBS) && \
		printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/main/$(CONF_OUTFILE)" && \
		$(STRIP) $(STRIPFLAGS) main/$(CONF_OUTFILE); \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/main/$(CONF_OUTFILE) is already up to date"; \
	fi && \
	if [ ! -f "main/$(MCONF_OUTFILE)" ] || [ config.mk -nt main/$(MCONF_OUTFILE) ] || [ Makefile -nt main/$(MCONF_OUTFILE) ] || ( for o in $(MCONF_OBJ); do [ "$$o" -nt main/$(MCONF_OUTFILE) ] && exit 0; done; exit 1 ); then \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/main/$(MCONF_OUTFILE)" && \
		$(LD) $(LDFLAGS) $(MCONF_OBJ) -o main/$(MCONF_OUTFILE) $(MCONF_LIBS) $(LIBS) && \
		printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/main/$(MCONF_OUTFILE)" && \
		$(STRIP) $(STRIPFLAGS) main/$(MCONF_OUTFILE); \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/main/$(MCONF_OUTFILE) is already up to date"; \
	fi

main/conf.o: main/conf.c config.mk Makefile
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

main/mconf.o: main/mconf.c config.mk Makefile
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

main/zconf.tab.o: main/zconf.tab.c config.mk Makefile
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

main/lxdialog/%.o: main/lxdialog/%.c config.mk Makefile
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

.PHONY: all kconfig gen clean mrproper

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/zconf.*.c"
	@rm -rf main/zconf.tab.c main/zconf.lex.c main/zconf.hash.c
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/conf $(SUBDIR)/main/mconf"
	@rm -rf main/conf main/mconf
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/*.o $(SUBDIR)/main/*.d"
	@rm -rf main/*.o main/*.d
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/lxdialog/*.o $(SUBDIR)/main/lxdialog/*.d"
	@rm -rf main/lxdialog/*.o main/lxdialog/*.d

mrproper: clean

.PHONY: all gen kconfig clean mrproper
