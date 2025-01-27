#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/reboot.h>

#include <asm/processor.h>

#define PWRON_BTN       0x01
#define PWRON_RTC_ALARM 0x02
#define PWRON_CABLE     0x04 
#define PWRON_SMPL      0x08 
#define PWRON_WDG       0x10 
#define PWRON_USB       0x20 
#define PWRON_WALL      0x40 

static int __init charge_mode_init(void)
{
        int val = (PWRON_CABLE | PWRON_USB | PWRON_WALL);

	pr_info("%s(): boot_reason: %x\n", __func__, boot_reason);

        if (boot_reason & val)
                kernel_restart(NULL);

        return 0;
}

static void __exit charge_mode_exit(void)
{
	return;
}

module_init(charge_mode_init);
module_exit(charge_mode_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("0xc0cafe");
MODULE_DESCRIPTION("DESCRIPTION");