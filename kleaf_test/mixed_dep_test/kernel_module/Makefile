# SPDX-License-Identifier: GPL-2.0

# Allow this module to properly link to symbols exported by the parent ddk_module.
#
# Note: You should only use this during a transient migration phase. Support for this may be
# discontinued without notice. It is recommended that all kernel_module's are converted to
# ddk_module. You may use this as a reference if you want to convert any kernel_module in your
# dependency tree to ddk_module, without converting the child modules first.
#
# Note: In this example, OUT_DIR=$(COMMON_OUT_DIR)/$(KERNEL_DIR), and KERNEL_DIR=common. So
# you could also use $(COMMON_OUT_DIR) to replace $(OUT_DIR)/.. below. Use the syntax that is
# consistent with your other modules.
EXTRA_SYMBOLS += $(OUT_DIR)/../common-modules/virtual-device/kleaf_test/mixed_dep_test/ddk_module/my_ddk_module_Module.symvers

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(KBUILD_OPTIONS) KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" $(@)
