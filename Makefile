# WFC Connector - build with MinGW-w64 (for example from WSL or MSYS2: make)
CC      = x86_64-w64-mingw32-gcc
WINDRES = x86_64-w64-mingw32-windres
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_WIN32_WINNT=0x0A00
LDFLAGS = -static -static-libgcc -s

OUT  = build
SRCS = src/gui.c src/app.c src/driver.c src/wfc_usb.c src/rt2570.c src/ap.c src/net.c
HDRS = $(wildcard src/*.h)

all: $(OUT)/WFCConnector.exe

$(OUT):
	mkdir -p $(OUT)

$(OUT)/app_res.o: res/app.rc res/app.ico res/app.manifest | $(OUT)
	$(WINDRES) -I res res/app.rc -O coff -o $@

$(OUT)/WFCConnector.exe: $(SRCS) $(HDRS) $(OUT)/app_res.o | $(OUT)
	$(CC) $(CFLAGS) -mwindows -o $@ $(SRCS) $(OUT)/app_res.o $(LDFLAGS) \
		-lwinusb -lsetupapi -lws2_32 -lcomctl32 -lshell32 -ladvapi32 -lgdi32 -luser32

# Host test: a simulated DS joins the hotspot and loads web pages through it
# (needs an internet connection; run build/selftest.exe on Windows).
$(OUT)/selftest.exe: tests/selftest.c src/ap.c src/net.c $(HDRS) | $(OUT)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/selftest.c src/ap.c src/net.c $(LDFLAGS) -lws2_32

test: $(OUT)/selftest.exe

res/app.ico: res/make_icon.py
	python3 res/make_icon.py $@

clean:
	rm -rf $(OUT)

.PHONY: all test clean
