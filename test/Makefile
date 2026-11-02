# Host test for the decoder (Mac, clang). Usage: make -C test
CXX = clang++
CXXFLAGS = -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
FW = ../firmware/aux_sniffer

build/test_auxdec: test_auxdec.cpp $(FW)/auxdec.cpp $(FW)/auxdec.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ test_auxdec.cpp $(FW)/auxdec.cpp

build/test_panelvcc: test_panelvcc.cpp $(FW)/panelvcc.cpp $(FW)/panelvcc.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ test_panelvcc.cpp $(FW)/panelvcc.cpp

test: build/test_auxdec build/test_panelvcc
	./build/test_auxdec ../testvectors_0x600.json
	./build/test_panelvcc

clean:
	rm -rf build

.DEFAULT_GOAL := test
.PHONY: test clean
