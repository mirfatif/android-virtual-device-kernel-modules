# Copyright (C) 2022 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load(
    "//build/kernel/kleaf:kernel.bzl",
    "kernel_images",
)

def define_virt_tests():
    """Define some tests with the virtual device targets.

    These are useful because the GKI package (//common) does not have external
    modules. These tests allows us to test some functionalites of Kleaf.
    """

    NONE = "no_vendor_boot"
    VENDOR_BOOT = "vendor_boot"
    VENDOR_KERNEL_BOOT = "vendor_kernel_boot"

    for arch in ("x86_64", "aarch64"):
        targets = []
        for build_boot in (True, False):
            for vendor_boot in (NONE, VENDOR_BOOT, VENDOR_KERNEL_BOOT):
                # TODO(b/227705464) for initramfs
                for build_initramfs in (True,):
                    build_vendor_boot = vendor_boot == VENDOR_BOOT
                    build_vendor_kernel_boot = vendor_boot == VENDOR_KERNEL_BOOT

                    suffix = "{arch}_{initramfs}_{boot}_{vendor_boot}".format(
                        arch = arch,
                        initramfs = "initramfs" if build_initramfs else "no_initramfs",
                        boot = "boot" if build_boot else "no_boot",
                        vendor_boot = vendor_boot,
                    )

                    kernel_images(
                        name = "virtual_device_{}".format(suffix),
                        build_initramfs = build_initramfs,
                        build_boot = build_boot,
                        build_vendor_boot = build_vendor_boot,
                        build_vendor_kernel_boot = build_vendor_kernel_boot,
                        kernel_build = ":virtual_device_{}".format(arch),
                        kernel_modules_install = ":virtual_device_{}_modules_install".format(arch),
                        # FIXME use a generated ramfs
                        vendor_ramdisk_binaries = [":virtual_device_{}_images_initramfs".format(arch)] if not build_initramfs else [],
                    )
                    targets.append(":virtual_device_{}".format(suffix))

        native.filegroup(
            name = "virtual_device_{}_test_images".format(arch),
            srcs = targets,
        )
