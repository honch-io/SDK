HONCH_MICROPYTHON_DIR := $(USERMOD_DIR)
HONCH_CORE_DIR := $(HONCH_MICROPYTHON_DIR)/../../../../core

SRC_USERMOD_C += \
	$(HONCH_MICROPYTHON_DIR)/modhonch_core.c \
	$(HONCH_MICROPYTHON_DIR)/mphal_adapter.c \
	$(HONCH_MICROPYTHON_DIR)/mpstorage_adapter.c \
	$(HONCH_MICROPYTHON_DIR)/mptransport_adapter.c \
	$(HONCH_CORE_DIR)/src/honch_core.c \
	$(HONCH_CORE_DIR)/src/honch_event_record.c \
	$(HONCH_CORE_DIR)/src/honch_lifecycle.c \
	$(HONCH_CORE_DIR)/src/honch_packetizer.c \
	$(HONCH_CORE_DIR)/src/honch_queue_policy.c \
	$(HONCH_CORE_DIR)/src/honch_ram_queue.c \
	$(HONCH_CORE_DIR)/src/honch_support.c \
	$(HONCH_CORE_DIR)/src/honch_wire_v2.c

CFLAGS_USERMOD += \
	-I$(HONCH_MICROPYTHON_DIR) \
	-I$(HONCH_MICROPYTHON_DIR)/compat \
	-I$(HONCH_CORE_DIR)/include \
	-I$(HONCH_CORE_DIR)/src \
	-DMICROPY_PY_HONCH_CORE=1 \
	-DMICROPY_C_HEAP_SIZE=65536
