# Makefile for GStreamer pipeline compilation

# Target executable
TARGET = recording_pipeline

# Source file
SOURCE = recording_pipeline.cpp

# Compiler
CC = g++

PKGS:= gstreamer-1.0 glib-2.0

GSTREAMER_FLAGS = $(shell pkg-config --cflags $(PKGS))

LIBS:= $(shell pkg-config --libs $(PKGS))

LIBS += -L/usr/local/lib -lglog

# Build rule
all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) -o $@ $< $(GSTREAMER_FLAGS) $(LIBS)

# Clean rule
clean:
	rm -f $(TARGET)

# Run rule (requires sudo)
run: all
	sudo ./$(TARGET)
