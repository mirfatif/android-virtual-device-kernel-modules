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
        hdrs = [
            "uapi/drm/virtgpu_drm.h",
            "uapi/linux/virtio_gpu.h",
            "uapi/goldfish/goldfish_dma.h",
            "uapi/goldfish/goldfish_sync.h",
            "uapi/goldfish/goldfish_address_space.h",
        ],
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
        srcs = [
            "virtio_gpu/virtgpu_fence.c",
            "virtio_gpu/virtgpu_plane.c",
            "virtio_gpu/virtgpu_vram.c",
            "virtio_gpu/virtgpu_trace.h",
            "virtio_gpu/virtgpu_prime.c",
            "virtio_gpu/virtgpu_debugfs.c",
            "virtio_gpu/virtgpu_drv.c",
            "virtio_gpu/virtgpu_trace_points.c",
            "virtio_gpu/virtgpu_kms.c",
            "virtio_gpu/virtgpu_object.c",
            "virtio_gpu/virtgpu_drv.h",
            "virtio_gpu/virtgpu_gem.c",
            "virtio_gpu/virtgpu_display.c",
            "virtio_gpu/virtgpu_ioctl.c",
            "virtio_gpu/virtgpu_vq.c",
        ],
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
