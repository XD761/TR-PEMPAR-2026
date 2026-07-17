CC       = mpicc
CFLAGS   = -O2 -Wall -Wextra $(shell sdl2-config --cflags)
LDFLAGS  = $(shell sdl2-config --libs) -lm
TARGET   = fire_sim
SRC      = main.c

# jumlah proses default saat `make run`
NP ?= 4

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

run: $(TARGET)
	mpirun -np $(NP) ./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
