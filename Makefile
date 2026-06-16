RACK_DIR ?= ../..

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res LICENSE

include $(RACK_DIR)/plugin.mk
