"""Tests on Kleaf using virtual device as a baseline."""

load("@bazel_skylib//rules:build_test.bzl", "build_test")
load("@bazel_skylib//rules:write_file.bzl", "write_file")
load(
    "//build/kernel/kleaf:kernel.bzl",
    "ddk_headers",
    "ddk_module",
    "ddk_submodule",
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
        arch = "x86_64",
        srcs = [
            "//common:kernel_x86_64_sources",
            "kleaf_test.fragment",
        ],
        kconfig_ext = "Kconfig.ext",
        outs = [],
        base_kernel = "//common:kernel_x86_64",
        build_config = "build.config.kleaf_test",
        module_outs = [],
        make_goals = [
            "modules",
        ],
        **private_kwargs
    )

    _ddk_module_dep_test(
        name = name + "_ddk_module_dep_test",
        kernel_build = name + "_kernel_build",
        **private_kwargs
    )

    _ddk_module_include_test(
        name = name + "_ddk_module_include_test",
        kernel_build = name + "_kernel_build",
        **private_kwargs
    )

    _ddk_module_conditional_srcs_test(
        name = name + "_ddk_module_conditional_srcs_test",
        kernel_build = name + "_kernel_build",
        **private_kwargs
    )

    _ddk_module_config_test(
        name = name + "_ddk_module_config_test",
        kernel_build = name + "_kernel_build",
        **private_kwargs
    )

    _ddk_submodule_config_test(
        name = name + "_ddk_submodule_config_test",
        kernel_build = name + "_kernel_build",
        **private_kwargs
    )

    _ddk_cflags_test(
        name = name + "_ddk_cflags_test",
        kernel_build = name + "_kernel_build",
        **private_kwargs
    )

    _ddk_long_arg_list_test(
        name = name + "_ddk_long_arg_list_test",
        kernel_build = name + "_kernel_build",
        **private_kwargs
    )

    _ddk_submodule_config_conditional_srcs_test(
        name = name + "_ddk_submodule_config_conditional_srcs_test",
        kernel_build = name + "_kernel_build",
        **private_kwargs
    )

    _ddk_core_headers_test(
        name = name + "_ddk_core_headers_test",
        kernel_build = "//common:kernel_x86_64",
        **private_kwargs
    )

    native.test_suite(
        name = name,
        tests = [
            name + "_ddk_module_dep_test",
            name + "_ddk_module_include_test",
            name + "_ddk_module_conditional_srcs_test",
            name + "_ddk_module_config_test",
            name + "_ddk_submodule_config_test",
            name + "_ddk_cflags_test",
            name + "_ddk_long_arg_list_test",
            name + "_ddk_submodule_config_conditional_srcs_test",
            name + "_ddk_core_headers_test",
        ],
        **kwargs
    )

def _ddk_module_dep_test(name, kernel_build, **private_kwargs):
    """Tests that module dependency works.

    This includes that Module.symvers files are restored properly.
    """
    ddk_module(
        name = name + "_module",
        out = name + "_mod.ko",
        kernel_build = kernel_build,
        srcs = ["client.c", "lib.c"],
        deps = ["//common:all_headers_x86_64"],
        **private_kwargs
    )
    ddk_module(
        name = name + "_child",
        out = name + "_child.ko",
        kernel_build = kernel_build,
        srcs = [
            "child.c",
            "child_lib.c",
        ],
        deps = [
            "//common:all_headers_x86_64",
            name + "_module",
        ],
        **private_kwargs
    )

    build_test(
        name = name,
        targets = [
            name + "_child",
        ],
        **private_kwargs
    )

def _ddk_module_include_test(name, kernel_build, **private_kwargs):
    """Tests that include directory works.

    This includes:
    - exported includes via includes and linuxincludes
    - local includes via copts
    """
    ddk_module(
        name = name + "_includes_test_module",
        out = "mymodule.ko",
        kernel_build = kernel_build,
        srcs = [
            "include_test.c",
            "include_test_lib.c",
            "include/include_test_lib.h",
        ],
        includes = ["include"],
        deps = ["//common:all_headers_x86_64"],
        **private_kwargs
    )

    ddk_module(
        name = name + "_linux_includes_test_module",
        out = "mymodule.ko",
        kernel_build = kernel_build,
        srcs = [
            "include_test.c",
            "include_test_lib.c",
            "include/include_test_lib.h",
        ],
        linux_includes = ["include"],
        deps = ["//common:all_headers_x86_64"],
        **private_kwargs
    )

    ddk_headers(
        name = name + "_local_includes_headers",
        hdrs = ["include/include_test_lib.h"],
        includes = ["include"],
        **private_kwargs
    )

    ddk_module(
        name = name + "_local_includes_test_module",
        out = "mymodule.ko",
        kernel_build = kernel_build,
        srcs = [
            "include_test.c",
            "include_test_lib.c",
            "include/include_test_lib.h",
        ],
        deps = [
            "//common:all_headers_x86_64",
            name + "_local_includes_headers",
        ],
        **private_kwargs
    )

    ddk_module(
        name = name + "_include_file_test_module",
        out = "mymodule.ko",
        kernel_build = kernel_build,
        srcs = [
            "include_test_implicit.c",
            "include_test_lib_implicit.c",
            "include/include_test_lib.h",
        ],
        copts = ["-include", "$(location include/include_test_lib.h)"],
        deps = ["//common:all_headers_x86_64"],
        **private_kwargs
    )

    build_test(
        name = name,
        targets = [
            name + "_includes_test_module",
            name + "_linux_includes_test_module",
            name + "_local_includes_test_module",
            name + "_include_file_test_module",
        ],
        **private_kwargs
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

def _ddk_module_config_test(name, kernel_build, **private_kwargs):
    # Test that the module can be built with only the defconfig file.
    # The defconfig mutates an item specified in Kconfig of the kernel_build.
    # NOTE: Changing an item not specified in the Kconfig of this module is
    # not recommended.
    write_file(
        name = name + "_defconfig_only_module_defconfig_target",
        out = name + "_defconfig_only_module_defconfig",
        content = [
            "CONFIG_KLEAF_TEST_MAIN=m",
        ],
        **private_kwargs
    )

    ddk_module(
        name = name + "_defconfig_only_module",
        out = name + "_mod.ko",
        kernel_build = kernel_build,
        defconfig = name + "_defconfig_only_module_defconfig",
        srcs = ["client.c"],
        conditional_srcs = {
            "CONFIG_KLEAF_TEST_MAIN": {
                True: ["lib.c"],
            },
        },
        deps = ["//common:all_headers_x86_64"],
        **private_kwargs
    )

    # Test that the module can be built with only the Kconfig file.
    # The default value of the Kconfig is used.
    write_file(
        name = name + "_kconfig_only_module_kconfig_target",
        out = name + "_kconfig_only_module_kconfig",
        content = [
            "config KLEAF_TEST_EXT_MOD",
            '\ttristate "KLEAF_TEST_EXT_MOD"',
            "\tdefault m",
            "",
        ],
    )

    ddk_module(
        name = name + "_kconfig_only_module",
        out = name + "_mod.ko",
        kernel_build = kernel_build,
        kconfig = name + "_kconfig_only_module_kconfig",
        srcs = ["client.c"],
        conditional_srcs = {
            "CONFIG_KLEAF_TEST_EXT_MOD": {
                True: ["lib.c"],
            },
        },
        deps = ["//common:all_headers_x86_64"],
        **private_kwargs
    )

    # Test that, when Kconfig is set, the Kconfig from kernel_build is still
    # inherited from.
    ddk_module(
        name = name + "_kconfig_only_module_inherit_from_kernel_build",
        out = name + "_mod.ko",
        kernel_build = kernel_build,
        kconfig = name + "_kconfig_only_module_kconfig",
        srcs = ["client.c"],
        conditional_srcs = {
            "CONFIG_KLEAF_TEST_M": {
                True: ["lib.c"],
            },
        },
        deps = ["//common:all_headers_x86_64"],
        **private_kwargs
    )

    # Test that the module can be be built with both the defconfig and the
    # Kconfig file. The value in defconfig is used.
    write_file(
        name = name + "_module_kconfig_target",
        out = name + "_module_kconfig",
        content = [
            "config KLEAF_TEST_EXT_MOD",
            '\ttristate "description"',
            "",
        ],
    )

    write_file(
        name = name + "_module_defconfig_target",
        out = name + "_module_defconfig",
        content = [
            "CONFIG_KLEAF_TEST_EXT_MOD=m",
        ],
    )

    ddk_module(
        name = name + "_module",
        out = name + "_mod.ko",
        kernel_build = kernel_build,
        defconfig = name + "_module_defconfig",
        kconfig = name + "_module_kconfig",
        srcs = ["client.c"],
        conditional_srcs = {
            "CONFIG_KLEAF_TEST_EXT_MOD": {
                True: ["lib.c"],
            },
        },
        deps = ["//common:all_headers_x86_64"],
        **private_kwargs
    )

    # Test inheriting Kconfig and defconfig from dependent module.
    ddk_module(
        name = name + "_child",
        out = name + "_child.ko",
        kernel_build = kernel_build,
        srcs = ["child.c"],
        conditional_srcs = {
            "CONFIG_KLEAF_TEST_EXT_MOD": {
                True: ["child_lib.c"],
            },
        },
        deps = [
            "//common:all_headers_x86_64",
            name + "_module",
        ],
        **private_kwargs
    )

    build_test(
        name = name,
        targets = [
            name + "_defconfig_only_module",
            name + "_kconfig_only_module",
            name + "_kconfig_only_module_inherit_from_kernel_build",
            name + "_module",
            name + "_child",
        ],
        **private_kwargs
    )

def _ddk_submodule_config_test(name, kernel_build, **private_kwargs):
    write_file(
        name = name + "_defconfig_target",
        out = name + "/defconfig",
        content = [
            "CONFIG_KLEAF_MUST_BE_SET=y",
        ],
        **private_kwargs
    )

    write_file(
        name = name + "_kconfig_target",
        out = name + "/Kconfig",
        content = [
            "config KLEAF_MUST_BE_SET",
            '\tbool "the prompt"',
            "",
        ],
        **private_kwargs
    )

    ddk_module(
        name = name + "_module",
        kernel_build = kernel_build,
        defconfig = name + "_defconfig_target",
        kconfig = name + "_kconfig_target",
        deps = [name + "_submodule"],
        **private_kwargs
    )

    ddk_submodule(
        name = name + "_submodule",
        out = "submodule.ko",
        srcs = ["must_be_set.c"],
        deps = ["//common:all_headers_x86_64"],
    )

    build_test(
        name = name,
        targets = [
            name + "_module",
        ],
        **private_kwargs
    )

def _ddk_cflags_test(name, kernel_build, **private_kwargs):
    ddk_module(
        name = name + "_copts",
        kernel_build = kernel_build,
        out = "mymodule.ko",
        srcs = [
            "copts_test/copts.c",
        ],
        deps = ["//common:all_headers_x86_64"],
        local_defines = [
            "NUMBER=123",
            "BOOLDEF",
            "STR=MYTOKEN",
            "MY_FUNC_DECL=VOID FUNC SEMICOLON",
        ],
        copts = [
            "-Wno-null-dereference",
            "-Wno-unused-value",
        ],
        **private_kwargs
    )

    ddk_module(
        name = name + "_copts_source_is_out",
        kernel_build = kernel_build,
        out = "copts.ko",
        srcs = [
            "copts_test/copts.c",
        ],
        deps = ["//common:all_headers_x86_64"],
        local_defines = [
            "NUMBER=123",
            "BOOLDEF",
            "STR=MYTOKEN",
            "MY_FUNC_DECL=VOID FUNC SEMICOLON",
        ],
        copts = [
            "-Wno-null-dereference",
            "-Wno-unused-value",
        ],
        **private_kwargs
    )

    ddk_module(
        name = name + "_copts_out_is_nested",
        kernel_build = kernel_build,
        out = "copts_test/mymodule.ko",
        srcs = [
            "copts_test/copts.c",
        ],
        deps = ["//common:all_headers_x86_64"],
        local_defines = [
            "NUMBER=123",
            "BOOLDEF",
            "STR=MYTOKEN",
            "MY_FUNC_DECL=VOID FUNC SEMICOLON",
        ],
        copts = [
            "-Wno-null-dereference",
            "-Wno-unused-value",
        ],
        **private_kwargs
    )

    build_test(
        name = name,
        targets = [
            name + "_copts",
            name + "_copts_source_is_out",
            name + "_copts_out_is_nested",
        ],
        **private_kwargs
    )

def _ddk_long_arg_list_test(name, kernel_build, **private_kwargs):
    ddk_module(
        name = name + "_module",
        out = name + "_module.ko",
        kernel_build = kernel_build,
        srcs = ["client.c", "lib.c"],
        deps = ["//common:all_headers_x86_64"],
        includes = [str(e) for e in range(100000)],
        **private_kwargs
    )

    build_test(
        name = name,
        targets = [
            name + "_module",
        ],
        **private_kwargs
    )

def _ddk_submodule_config_conditional_srcs_test(name, kernel_build, **private_kwargs):
    write_file(
        name = name + "_defconfig_target",
        out = name + "/defconfig",
        content = [
            "CONFIG_KLEAF_MUST_BE_SET=y",
            "",
        ],
        **private_kwargs
    )

    write_file(
        name = name + "_kconfig_target",
        out = name + "/Kconfig",
        content = [
            "config KLEAF_MUST_BE_SET",
            '\tbool "the prompt"',
            "",
        ],
        **private_kwargs
    )

    ddk_module(
        name = name + "_module",
        kconfig = name + "_kconfig_target",
        defconfig = name + "_defconfig_target",
        kernel_build = kernel_build,
        deps = [name + "_submodule"],
        **private_kwargs
    )

    ddk_submodule(
        name = name + "_submodule",
        out = "mymodule.ko",
        conditional_srcs = {
            "CONFIG_KLEAF_MUST_BE_SET": {
                True: ["must_be_set.c"],
            },
        },
        deps = ["//common:all_headers_x86_64"],
        **private_kwargs
    )

    build_test(
        name = name,
        targets = [
            name + "_module",
        ],
    )

def _ddk_core_headers_test(name, kernel_build, **private_kwargs):
    ddk_module(
        name = name + "_module",
        out = name + "_min.ko",
        kernel_build = kernel_build,
        srcs = ["min.c"],
        deps = ["//common:all_headers_x86_64"],
        **private_kwargs
    )

    build_test(
        name = name,
        targets = [
            name + "_module",
        ],
    )
