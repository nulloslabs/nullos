include ../../config.mk

SRC = $(wildcard main/*.c main/*.S)
OBJ = $(patsubst main/%,../../%,$(SRC:.c=.o))
OBJ := $(patsubst %.S,%.o,$(OBJ))

all:
	@needs_build=0; \
	for src in $(SRC); do \
		obj=$$(echo $$src | sed 's|main/|../../|; s|\.[cS]$$|.o|'); \
		if [ ! -f $$obj ] || [ $$src -nt $$obj ]; then \
			needs_build=1; \
			break; \
		fi; \
	done; \
	$(MAKE) -C main; \
	if [ $$needs_build -eq 0 ] && [ -n "$(OBJ)" ]; then \
		printf "  %-7s %s\n" "INFO" "userspace/crt0.o userspace/crt1.o userspace/Scrt1.o userspace/crti.o userspace/crtn.o are already up to date"; \
	fi

clean:
	@$(MAKE) -C main clean

.PHONY: all clean
