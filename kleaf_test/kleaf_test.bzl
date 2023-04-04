"""Tests on Kleaf using virtual device as a baseline."""

load("@bazel_skylib//rules:build_test.bzl", "build_test")
load(
    "//build/kernel/kleaf:kernel.bzl",
    "ddk_module",
    "kernel_build",
)

def kleaf_test(
        name,
        **kwargs):
    """Define tests on Kleaf using virtual device as a baseline.

    Args:
        name: Name of the test
        **kwargs: additional kwargs common to all rules.
    """

    private_kwargs = kwargs | dict(
        visibility = ["//visibility:private"],
    )

    kernel_build(
        name = name + "_kernel_build",
        srcs = [
            "//common:kernel_x86_64_sources",
            "kleaf_test.fragment",
        ],
        kconfig_ext = "Kconfig.ext",
        outs = [],
        base_kernel = "//common:kernel_x86_64",
        build_config = "build.config.kleaf_test",
        module_outs = [],
        **private_kwargs
    )

    _ddk_module_conditional_srcs_test(
        name = name + "_ddk_module_conditional_srcs_test",
        kernel_build = name + "_kernel_build",
        **private_kwargs
    )

    native.test_suite(
        name = name,
        tests = [
            name + "_ddk_module_conditional_srcs_test",
        ],
        **kwargs
    )

def _ddk_module_conditional_srcs_test(name, kernel_build, **private_kwargs):
    # CONFIG_KLEAF_TEST_M is set to m. Check that lib.c is linked by having
    # client.c using a symbol from lib.c.
    ddk_module(
        name = name + "_module_y",
        kernel_build = kernel_build,
        srcs = ["client.c"],
        conditional_srcs = {
            "CONFIG_KLEAF_TEST_Y": {
                True: ["lib.c"],
            },
        },
        deps = ["//common:all_headers_x86_64"],
        out = name + "_module.ko",
        **private_kwargs
    )

    # CONFIG_KLEAF_TEST_M is set to m. Check that lib.c is linked by having
    # client.c using a symbol from lib.c.
    ddk_module(
        name = name + "_module_m",
        kernel_build = kernel_build,
        srcs = ["client.c"],
        conditional_srcs = {
            "CONFIG_KLEAF_TEST_M": {
                True: ["lib.c"],
            },
        },
        deps = ["//common:all_headers_x86_64"],
        out = name + "_module.ko",
        **private_kwargs
    )

    build_test(
        name = name,
        targets = [
            name + "_module_y",
            name + "_module_m",
        ],
        **private_kwargs
    )
