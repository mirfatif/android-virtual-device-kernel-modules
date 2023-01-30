/* Put all options we expect gki_defconfig to provide us here */

#ifndef CONFIG_BUILTIN_VD
	#ifdef CONFIG_SW_SYNC
	#error CONFIG_SW_SYNC is a module in virtual_device.fragment
	#endif
#endif
