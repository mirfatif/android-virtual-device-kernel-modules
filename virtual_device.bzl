load(
    "//build/kernel/kleaf:kernel.bzl",
    "ddk_headers",
    "ddk_module",
    "kernel_module_group",
)

def virtual_device_external_modules(
        name,
        kernel_build,
        gki_headers,
        **kwargs):
    """Defines external modules target.

    Args:
        name: name of the target pointing to all external modules
        kernel_build: kernel_build
        gki_headers: GKI ddk_headers target
        **kwargs: Additional arguments to the internal target
    """

    private_kwargs = kwargs | {
        "visibility": ["//visibility:private"],
    }

    ddk_headers(
        name = name + "_override_uapi_headers",
        # Bad pattern: glob in macros. However, We know that this glob
        # only reads a small amount of files, and this macro is only invoked
        # once per architecture, so we pay the price of I/O at analysis phase
        # to reduce maintenance cost.
        hdrs = native.glob([
            "uapi/**/*.h",
        ]),
        includes = ["."],
        linux_includes = ["uapi"],
        **private_kwargs
    )

    ddk_headers(
        name = name + "_headers",
        hdrs = [
            # Keep ordering so that uapi/ has a higher priority
            # do not sort
            name + "_override_uapi_headers",
            gki_headers,
        ],
        includes = ["include"],
        linux_includes = ["uapi"],
        **private_kwargs
    )

    ddk_module(
        name = name + "_submodules/goldfish_drivers/goldfish_address_space",
        srcs = [
            "goldfish_drivers/defconfig_test.h",
            "goldfish_drivers/goldfish_address_space.c",
        ],
        out = "goldfish_drivers/goldfish_address_space.ko",
        kernel_build = kernel_build,
        deps = [name + "_headers"],
        **private_kwargs
    )

    ddk_module(
        name = name + "_submodules/goldfish_drivers/goldfish_pipe",
        srcs = [
            "goldfish_drivers/defconfig_test.h",
            "goldfish_drivers/goldfish_pipe.h",
            "goldfish_drivers/goldfish_pipe_base.c",
            "goldfish_drivers/goldfish_pipe_qemu.h",
            "goldfish_drivers/goldfish_pipe_v1.c",
            "goldfish_drivers/goldfish_pipe_v2.c",
        ],
        out = "goldfish_drivers/goldfish_pipe.ko",
        kernel_build = kernel_build,
        deps = [name + "_headers"],
        **private_kwargs
    )

    ddk_module(
        name = name + "_submodules/goldfish_drivers/goldfish_sync",
        srcs = [
            "goldfish_drivers/defconfig_test.h",
            "goldfish_drivers/goldfish_sync.c",
        ],
        out = "goldfish_drivers/goldfish_sync.ko",
        kernel_build = kernel_build,
        deps = [name + "_headers"],
        **private_kwargs
    )

    ddk_module(
        name = name + "_submodules/virtio_gpu/virtio-gpu",
        # Bad pattern: glob in macros. However, We know that this glob
        # only reads a small amount of files, and this macro is only invoked
        # once per architecture, so we pay the price of I/O at analysis phase
        # to reduce maintenance cost.
        srcs = native.glob([
            "virtio_gpu/*.c",
            "virtio_gpu/*.h",
        ]),
        out = "virtio_gpu/virtio-gpu.ko",
        kernel_build = kernel_build,
        deps = [name + "_headers"],
        **private_kwargs
    )

    kernel_module_group(
        name = name,
        srcs = [
            name + "_submodules/goldfish_drivers/goldfish_address_space",
            name + "_submodules/goldfish_drivers/goldfish_pipe",
            name + "_submodules/goldfish_drivers/goldfish_sync",
            name + "_submodules/virtio_gpu/virtio-gpu",
        ],
        **kwargs
    )
