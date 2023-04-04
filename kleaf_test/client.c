#include <linux/module.h>

MODULE_DESCRIPTION("A test module for Kleaf testing purposes");
MODULE_AUTHOR("Yifan Hong <elsk@google.com>");
MODULE_LICENSE("GPL v2");

extern void mylib(void);

void myclient(void) {
    mylib();
}
