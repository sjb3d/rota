
CC?=clang
CFLAGS=-std=c99 -O3 -Wall -Wextra -Werror
LDFLAGS=-lm

SRC=rota.c mt19937ar.c
EXE=rota

all: $(EXE)

$(EXE): Makefile $(SRC)
	$(CC) $(LDFLAGS) -o $@ $(CFLAGS) $(SRC)

clean:
	$(RM) $(EXE)
