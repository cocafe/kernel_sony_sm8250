LOCAL_PATH := $(call my-dir)
DLKM_DIR := $(TOP)/device/qcom/common/dlkm

include $(CLEAR_VARS)
LOCAL_MODULE              := nxp_sn1x0_spi.ko
LOCAL_MODULE_KBUILD_NAME  := p73.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

SN1X0_I2C_PATH := vendor/semc/hardware/nfc/drivers/sn1x0_i2c
SN1X0_I2C_DIR := $(shell pwd)/$(SN1X0_I2C_PATH)

KBUILD_OPTIONS := SN1X0_I2C_DIR=$(SN1X0_I2C_DIR)
KBUILD_OPTIONS += SN1X0_I2C_PATH=$(SN1X0_I2C_PATH)
KBUILD_OPTIONS_GKI := SN1X0_I2C_DIR=$(SN1X0_I2C_DIR)
KBUILD_OPTIONS_GKI += SN1X0_I2C_PATH=$(SN1X0_I2C_PATH)/gki

#Must be built after the nxp_sn10x_i2c module
LOCAL_ADDITIONAL_DEPENDENCIES := \
    $(TARGET_OUT_INTERMEDIATES)/$(SN1X0_I2C_PATH)/nxp_sn1x0_i2c.ko
LOCAL_ADDITIONAL_DEPENDENCIES_GKI := \
    $(TARGET_OUT_INTERMEDIATES)/$(SN1X0_I2C_PATH)/gki/nxp_sn1x0_i2c.ko

include $(DLKM_DIR)/AndroidKernelModule.mk
