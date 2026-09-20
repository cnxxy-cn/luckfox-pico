obj-$(CONFIG_ESP_HOSTED_SDIO) += esp32_sdio.o

esp32_sdio-y := \
	main.o \
	esp_cmd.o \
	esp_utils.o \
	esp_cfg80211.o \
	esp_stats.o \
	esp_debugfs.o \
	esp_log.o \
	esp_custom.o \
	sdio/esp_sdio.o \
	sdio/esp_sdio_api.o

ifneq ($(CONFIG_BT),)
esp32_sdio-y += esp_bt.o
endif

ccflags-y += -I$(srctree)/$(src) -I$(srctree)/$(src)/include -I$(srctree)/$(src)/sdio
ccflags-$(CONFIG_ESP_HOSTED_AP_MODE) += -DCONFIG_AP_MODE
CFLAGS_esp_log.o += -DDEBUG
