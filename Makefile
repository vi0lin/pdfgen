CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDLIBS    = -lz -lcurl

# WebP support needs libwebp (Debian/Ubuntu: sudo apt install libwebp-dev).
# Build without it:  make NO_WEBP=1
ifdef NO_WEBP
CXXFLAGS += -DPDFGEN_NO_WEBP
else
LDLIBS   += -lwebp
endif

SRC  = $(wildcard src/*.cpp)
CSRC = src/gmail_send.c
OBJ  = $(SRC:.cpp=.o) $(CSRC:.c=.o)

pdfgen: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.cpp $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c $(wildcard src/*.h)
	$(CC) -O2 -Wall -Wextra -c $< -o $@

clean:
	rm -f pdfgen $(OBJ)

.PHONY: clean
