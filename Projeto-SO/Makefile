# Compiler variables
CC = gcc
CFLAGS = -g -Wall -Wextra -std=c17 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lncurses -pthread

# Directory variables
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include

# Executable
TARGET = Pacmanist

# Object files - based on what you actually have in src/
OBJS = game.o board.o display.o

# Dependencies
game.o = board.h display.h protocol.h
board.o = board.h
display.o = display.h board.h

# Object files path
vpath %.o $(OBJ_DIR)
vpath %.c $(SRC_DIR)

# Make targets
all: $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(OBJS) | folders
	$(CC) $(CFLAGS) $(addprefix $(OBJ_DIR)/,$(OBJS)) -o $@ $(LDFLAGS)

# Generic rule to create object files
%.o: %.c $($@) | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# Create folders
folders:
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(BIN_DIR)

# Clean object files and executable
clean:
	rm -f $(OBJ_DIR)/*.o
	rm -f $(BIN_DIR)/$(TARGET)

# Identify targets that do not create files
.PHONY: all clean folders
