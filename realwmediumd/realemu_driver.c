

#include <linux/module.h>
// #include <linux/init.h>
#include <linux/kernel.h>
#include "realemu_driver.h"


MODULE_AUTHOR("gtx");
MODULE_DESCRIPTION("RealEme driver to satisfy Mininet-Wifi interface");
MODULE_LICENSE("GPL 3.0");

static int nodes = 2;
module_param(nodes, int, 0444);
MODULE_PARM_DESC(nodes, "Number of simulated nodes");




