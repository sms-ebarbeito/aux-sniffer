# Host test for the decoder (Mac, clang). Usage: make -C test
CXX = clang++
CXXFLAGS = -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
FW = ../firmware/aux_sniffer

build/test_auxdec: test_auxdec.cpp $(FW)/auxdec.cpp $(FW)/auxdec.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ test_auxdec.cpp $(FW)/auxdec.cpp

test: build/test_auxdec
	./build/test_auxdec ../testvectors_0x600.json

clean:
	rm -rf build

.DEFAULT_GOAL := test
.PHONY: test clean
