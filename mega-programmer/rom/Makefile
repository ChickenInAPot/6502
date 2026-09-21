BUILD_DIR := build
SOURCE := counting.s
CONFIG := counting.cfg
OBJECT := $(BUILD_DIR)/counting.o
IMAGE := $(BUILD_DIR)/counting.bin
MAP := $(BUILD_DIR)/counting.map
LISTING := $(BUILD_DIR)/counting.lst

.PHONY: all clean

all: $(IMAGE)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJECT): $(SOURCE) | $(BUILD_DIR)
	ca65 --cpu 65c02 --listing $(LISTING) -o $(OBJECT) $(SOURCE)

$(IMAGE): $(OBJECT) $(CONFIG)
	ld65 -C $(CONFIG) -m $(MAP) -o $(IMAGE) $(OBJECT)

clean:
	rm -f $(OBJECT) $(IMAGE) $(MAP) $(LISTING)
