CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
LDFLAGS ?= -static -no-pie
MODFLAGS = -shared -fPIC -O2 -fno-builtin -nostdlib
INCLUDE = -I../../include
OUT ?= ../../build/sd_apps

all: $(OUT)/sd_app_runner $(OUT)/hello/bin/hello $(OUT)/mod_sd_apps.so

$(OUT)/sd_app_runner: runner.c package.c package.h sha256.c sha256.h ../../include/natartos_app.h ../../include/natartos_display.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDE) $(LDFLAGS) runner.c package.c sha256.c -o $@

$(OUT)/hello/bin/hello: ../../examples/sd_apps/hello/hello.c ../../examples/sd_apps/hello/app.json ../../examples/sd_apps/hello/assets/message.txt ../../include/natartos_app.h
	mkdir -p $(OUT)/hello/bin $(OUT)/hello/assets
	$(CC) $(CFLAGS) $(INCLUDE) $(LDFLAGS) ../../examples/sd_apps/hello/hello.c -o $@
	cp ../../examples/sd_apps/hello/app.json $(OUT)/hello/
	cp ../../examples/sd_apps/hello/assets/message.txt $(OUT)/hello/assets/

$(OUT)/mod_sd_apps.so: mod.c ../../include/natartos_common.h
	mkdir -p $(OUT)
	$(CC) $(MODFLAGS) $(INCLUDE) -I.. -Wl,-soname,mod_sd_apps.so mod.c -o $@

.PHONY: all
