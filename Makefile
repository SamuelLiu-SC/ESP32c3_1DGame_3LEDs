# changed to 0.96" I2C / IIC OLED module
# works with SSD1306 driver, 128x64 pixels, 0.96 inch, I2C interface
# Makefile for ESP-IDF v6.0 under Espressif Installation Manager (EIM)

# Force the use of Bash shell
SHELL := /bin/bash

# Target version managed by EIM (e.g., v6.0 or v6.0.1)
IDF_VER ?= v6.0

.PHONY: all build flash monitor clean menuconfig fullclean uf2

all: build

build:
	eim run "idf.py build" $(IDF_VER)

flash:
	eim run "idf.py flash" $(IDF_VER)

monitor:
	eim run "idf.py monitor" $(IDF_VER)

menuconfig:
	eim run "idf.py menuconfig" $(IDF_VER)

clean:
	eim run "idf.py clean" $(IDF_VER)

fullclean:
	eim run "idf.py fullclean" $(IDF_VER)

uf2:
	eim run "idf.py uf2" $(IDF_VER)
