# Set library directory
LIBDIR    := ../lib

# Define compiler flags
CPPFLAGS  += -I$(LIBDIR)
USERCFLAGS := $(CFLAGS)
WFLAGS     = -std=gnu99 -Wall -Wextra -Wundef -Wshadow -Wcast-align -Wstrict-prototypes -Wc++-compat
CFLAGS     = $(WFLAGS) -O2 $(USERCFLAGS)

# Define C++ compiler flags and C++ compiler
CXXFLAGS   = -std=c++17 -Wall -O2 -pthread  # Add pthread for multi-threading
CC = gcc
CXX = g++

# Define the new C++ source file and its object
NEW_CPP_SRC = multithreads_dep.cpp
NEW_CPP_OBJ = multithreads_dep.o

# Define LZ4 library and test file
TESTFILE   = Makefile
SLIBLZ4    := $(LIBDIR)/liblz4.a
LZ4DIR     = ../programs
LZ4        = $(LZ4DIR)/lz4

# Define the default target
default: all

# Build the LZ4 static library if not present
$(SLIBLZ4): $(LIBDIR)/lz4.c $(LIBDIR)/lz4hc.c $(LIBDIR)/lz4frame.c $(LIBDIR)/lz4.h $(LIBDIR)/lz4hc.h $(LIBDIR)/lz4frame.h $(LIBDIR)/lz4frame_static.h
	$(MAKE) -j -C $(LIBDIR) liblz4.a

# Compile the new C++ feature object file (multithreads_dep.cpp)
$(NEW_CPP_OBJ): $(NEW_CPP_SRC)
	$(CXX) $(CXXFLAGS) -c $(NEW_CPP_SRC) -o $(NEW_CPP_OBJ)

# Define all targets
ALL = print_version \
      simple_buffer \
      frameCompress \
      fileCompress \
      blockStreaming_doubleBuffer \
      blockStreaming_ringBuffer \
      streamingHC_ringBuffer \
      blockStreaming_lineByLine \
      dictionaryRandomAccess \
      bench_functions \
      multithreads_dep  # Add new target for C++ multi-threaded program

.PHONY: all
all: $(ALL)

# Add the new C++ object as a dependency for multithreads_dep program
multithreads_dep: $(NEW_CPP_OBJ) $(SLIBLZ4)
	$(CXX) $(NEW_CPP_OBJ) -o multithreads_dep $(SLIBLZ4) -lpthread

# Build all programs
$(ALL): $(SLIBLZ4)

# Build LZ4 if not present
.PHONY: $(LZ4)
$(LZ4):
	$(MAKE) -j -C $(LZ4DIR) lz4

# Test all compiled programs
.PHONY: test
test : all $(LZ4)
	@echo "\n=== Print Version ==="
	./print_version$(EXT)
	@echo "\n=== Simple compression example ==="
	./simple_buffer$(EXT)
	@echo "\n=== Double-buffer ==="
	./blockStreaming_doubleBuffer$(EXT) $(TESTFILE)
	@echo "\n=== Ring Buffer ==="
	./blockStreaming_ringBuffer$(EXT)   $(TESTFILE)
	@echo "\n=== Ring Buffer + LZ4 HC ==="
	./streamingHC_ringBuffer$(EXT) $(TESTFILE)
	@echo "\n=== Compress line by line ==="
	./blockStreaming_lineByLine$(EXT) $(TESTFILE)
	@echo "\n=== Dictionary Random Access ==="
	./dictionaryRandomAccess$(EXT) $(TESTFILE) $(TESTFILE) 1100 1400
	@echo "\n=== Frame compression ==="
	./frameCompress$(EXT) $(TESTFILE)
	$(LZ4) -vt $(TESTFILE).lz4
	@echo "\n=== File compression ==="
	./fileCompress$(EXT) $(TESTFILE)
	$(LZ4) -vt $(TESTFILE).lz4
	@echo "\n=== Q&D benchmark ==="
	./bench_functions$(EXT) 10000
	@echo "\n=== Multi-threaded example ==="
	./multithreads_dep$(EXT)

# Clean up object files, executables, and temporary files
.PHONY: clean
clean:
	@$(RM) core *.o *.dec *-0 *-9 *-8192 *.lz4s *.lz4 $(ALL)
	@echo Cleaning completed
