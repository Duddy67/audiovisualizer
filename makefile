# Compiler
CXX := g++

# Target executable
TARGET := waveform_viewer

# Source files
SRCS := main.cpp

# Compiler flags
CXXFLAGS := -Wall -Wextra

# Linker flags
LDFLAGS := -L/usr/local/lib
LDLIBS := -lfltk_gl -lfltk -lGL -lGLU -lX11 -lXext -lXft \
           -lfontconfig -lXrender -lXcursor -lXinerama \
           -lXfixes -lpthread -lm -ldl

# Default target
all: $(TARGET)

# Build target
$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

# Clean target
clean:
	rm -f $(TARGET)

.PHONY: all clean
