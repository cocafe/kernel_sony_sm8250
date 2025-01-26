AUDIO_ROOT := $(srctree)/drivers/vendor/qcom/audio-kernel

# auto-detect subdirs
ifeq ($(CONFIG_ARCH_SDXPOORWILLS), y)
include $(srctree)/drivers/vendor/qcom/audio-kernel/config/sdxpoorwillsauto.conf
export
endif
ifeq ($(CONFIG_ARCH_SM8150), y)
include $(srctree)/drivers/vendor/qcom/audio-kernel/config/sm8150auto.conf
export
endif
ifeq ($(CONFIG_ARCH_SDMSHRIKE), y)
include $(srctree)/drivers/vendor/qcom/audio-kernel/config/sm8150auto.conf
export
endif
ifeq ($(CONFIG_ARCH_KONA), y)
include $(srctree)/drivers/vendor/qcom/audio-kernel/config/konaauto.conf
endif
ifeq ($(CONFIG_ARCH_SDM660), y)
include $(srctree)/drivers/vendor/qcom/audio-kernel/config/sdm660auto.conf
endif

# Use USERINCLUDE when you must reference the UAPI directories only.
USERINCLUDE     += \
                -I$(srctree)/drivers/vendor/qcom/audio-kernel/include/uapi \
                -I$(srctree)/drivers/vendor/qcom/audio-kernel/include

# Use LINUXINCLUDE when you must reference the include/ directory.
# Needed to be compatible with the O= option
LINUXINCLUDE    += \
                -I$(srctree)/drivers/vendor/qcom/audio-kernel/include/uapi \
                -I$(srctree)/drivers/vendor/qcom/audio-kernel/include

ifeq ($(CONFIG_ARCH_SDXPOORWILLS), y)
LINUXINCLUDE    += \
                -include $(srctree)/drivers/vendor/qcom/audio-kernel/config/sdxpoorwillsautoconf.h
endif
ifeq ($(CONFIG_ARCH_SM8150), y)
LINUXINCLUDE    += \
                -include $(srctree)/drivers/vendor/qcom/audio-kernel/config/sm8150autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDMSHRIKE), y)
LINUXINCLUDE    += \
                -include $(srctree)/drivers/vendor/qcom/audio-kernel/config/sm8150autoconf.h
endif
ifeq ($(CONFIG_ARCH_KONA), y)
LINUXINCLUDE    += \
                -include $(srctree)/drivers/vendor/qcom/audio-kernel/config/konaautoconf.h
endif
ifeq ($(CONFIG_ARCH_SDM660), y)
LINUXINCLUDE    += \
                -include $(srctree)/drivers/vendor/qcom/audio-kernel/config/sdm660autoconf.h
endif

ccflags-y += $(LINUXINCLUDE)

obj-y += soc/
obj-y += dsp/
obj-y += ipc/
obj-y += asoc/
