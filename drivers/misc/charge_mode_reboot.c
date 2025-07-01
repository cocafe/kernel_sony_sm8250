#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/reboot.h>

#include <asm/processor.h>

#define PWRON_BTN               0x01
#define PWRON_RTC_ALARM         0x02
#define PWRON_CABLE             0x04
#define PWRON_SMPL              0x08
#define PWRON_WDG               0x10
#define PWRON_USB               0x20
#define PWRON_WALL              0x40
#define PWRON_INCHG             (PWRON_CABLE | PWRON_USB | PWRON_WALL)

#define BATTERY_THRESHOLD       5

extern int qpnp_fg_get_capacity(int *ret);

// echo -ne 'recovery' >
static int reboot_now_set(const char *buf, const struct kernel_param *kp)
{
        kernel_restart((char *)buf);
        return 0;
}

static const struct kernel_param_ops reboot_now_param_ops = {
        .set = reboot_now_set,
};

module_param_cb(reboot_now, &reboot_now_param_ops, NULL, 0600);

int is_in_charge_boot(void)
{
        return (boot_reason & (PWRON_INCHG));
}

static int __init charge_mode_init(void)
{
        int lvl = -1;

        if (!qpnp_fg_get_capacity(&lvl)) {
                pr_info("%s(): current battery level: %d\n", __func__, lvl);
        }

	pr_info("%s(): boot_reason: %x\n", __func__, boot_reason);

        if (boot_reason & PWRON_INCHG) {
                // if battery is too low, just let it charge instead of reboot device
                if (lvl >= 0 && lvl <= BATTERY_THRESHOLD) {
                        return 0;
                }

                kernel_restart(NULL);
        }

        return 0;
}

late_initcall(charge_mode_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("0xc0cafe");
MODULE_DESCRIPTION("DESCRIPTION");