This test demonstrates how a kernel_module may depend on ddk_module.

**Note**: You should only use this during a transient migration phase. Support for this may be
discontinued without notice. It is recommended that all `kernel_module`s are converted to
`ddk_module`. You may use this as a reference if you want to convert any `kernel_module` in your
dependency tree to `ddk_module`, without converting the child modules first.
