#
# File: app/Makefile
# Purpose: Build rules for the badge_photo application and uMTP-Responder.
#
ifeq ($(APP_PARAM), )
    APP_PARAM:=../Makefile.param
    include $(APP_PARAM)
endif

export LC_ALL=C
SHELL:=/bin/bash

CURRENT_DIR := $(shell pwd)

PKG_NAME := badge_photo
PKG_BIN ?= out

RK_APP_CFLAGS = -I $(RK_APP_MEDIA_INCLUDE_PATH) \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/uAPI \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/uAPI2 \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/algos \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/common \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/xcore \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/iq_parser \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/iq_parser_v2 \
				-I $(RK_APP_OUTPUT)/include \
				-I $(RK_PROJECT_TOP_DIR)/make_meta/include \
				-I $(CURRENT_DIR)/src

RK_APP_LDFLAGS = -L $(RK_APP_MEDIA_LIBS_PATH) -L$(RK_APP_OUTPUT)/lib
RK_APP_OPTS += -Wl,-rpath-link,$(RK_APP_MEDIA_LIBS_PATH):$(RK_APP_OUTPUT)/lib

FASTBOOT_LDFLAGS := -Wl,-Bstatic \
		-lrockit_tiny -lrockchip_mpp -lrksysutils -lstdc++ \
		-Wl,-Bdynamic \
		-ldl -lm

SRC := $(filter-out $(CURRENT_DIR)/src/overlay_web.c $(CURRENT_DIR)/src/rndis_dhcp.c,$(wildcard $(CURRENT_DIR)/src/*.c))

ifeq ($(PKG_BIN),)
$(error ### $(CURRENT_DIR): PKG_BIN is NULL, Please Check !!!)
endif

.PHONY: all badge_photo-build umtprd-build clean distclean info

all: badge_photo-build overlay_web-build rndis_dhcp-build
	@$(MAKE) --no-print-directory umtprd-build
	$(call MAROC_COPY_PKG_TO_APP_OUTPUT, $(RK_APP_OUTPUT), $(PKG_BIN))
	@echo "build $(PKG_NAME) done"

badge_photo-build:
	rm -rf $(PKG_BIN)
	mkdir -p $(PKG_BIN)/bin $(PKG_BIN)/root/etc/init.d
	cp -f $(CURRENT_DIR)/config/badge_photo.conf $(PKG_BIN)/root/etc/
	cp -f $(CURRENT_DIR)/config/S99badge_photo $(PKG_BIN)/root/etc/init.d/
	chmod +x $(PKG_BIN)/root/etc/init.d/S99badge_photo
	mkdir -p $(PKG_BIN)/root/oem/usr/share/badge
	cp -f $(CURRENT_DIR)/assets/overlays/* $(PKG_BIN)/root/oem/usr/share/badge/
	$(RK_APP_CROSS)-gcc $(SRC) -o $(PKG_BIN)/bin/badge_photo \
		-O2 -mthumb -mthumb-interwork \
		-fdata-sections -ffunction-sections -Wl,--gc-sections \
		-Wall -Wextra \
		$(RK_APP_LDFLAGS) $(RK_APP_CFLAGS) $(RK_APP_OPTS) \
		$(FASTBOOT_LDFLAGS)

overlay_web-build:
	mkdir -p $(PKG_BIN)/bin
	$(RK_APP_CROSS)-gcc $(CURRENT_DIR)/src/overlay_web.c -o $(PKG_BIN)/bin/overlay_web \
		-O2 -mthumb -mthumb-interwork -Wall -Wextra

rndis_dhcp-build:
	mkdir -p $(PKG_BIN)/bin
	$(RK_APP_CROSS)-gcc $(CURRENT_DIR)/src/rndis_dhcp.c -o $(PKG_BIN)/bin/rndis_dhcp \
		-O2 -mthumb -mthumb-interwork -Wall -Wextra

umtprd-build:
	@if [ -f "$(CURRENT_DIR)/third_party/umtprd/Makefile" ]; then \
		echo "building umtprd"; \
		$(MAKE) -C $(CURRENT_DIR)/third_party/umtprd CC=$(RK_APP_CROSS)-gcc; \
		mkdir -p $(PKG_BIN)/usr/sbin; \
		cp -f $(CURRENT_DIR)/third_party/umtprd/umtprd $(PKG_BIN)/usr/sbin/umtprd; \
	else \
		echo "umtprd source not present, skipping"; \
	fi

clean:
	@rm -rf $(PKG_BIN)

distclean: clean

info:
	@echo "PKG_NAME=$(PKG_NAME)"
	@echo "RK_APP_TYPE=$(RK_APP_TYPE)"
	@echo "SRC=$(SRC)"
