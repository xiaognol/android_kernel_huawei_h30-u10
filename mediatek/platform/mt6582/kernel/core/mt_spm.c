#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/delay.h>

#include <mach/irqs.h>
#include <mach/mt_spm.h>
#include <mach/wd_api.h>

/**********************************************************
 * PCM code for normal (v2 @ 2013-04-26)
 **********************************************************/
static const u32 __pcm_normal[] = {
    0x1840001f, 0x00000001, 0x1b00001f, 0x00202000, 0x1b80001f, 0x80001000,
    0x8880000c, 0x00200000, 0xd80001e2, 0x17c07c1f, 0xe8208000, 0x100063e0,
    0x00000002, 0x1b80001f, 0x00001000, 0x809c840d, 0xd8200042, 0x17c07c1f,
    0xa1d78407, 0x1890001f, 0x10006014, 0x18c0001f, 0x10006014, 0xa0978402,
    0xe0c00002, 0x1b80001f, 0x00001000, 0xf0000000
};
static const pcm_desc_t pcm_normal = {
    .base   = __pcm_normal,
    .size   = 28,
};


/**************************************
 * SW code for general and normal
 **************************************/
#define WAKE_SRC_FOR_NORMAL     (WAKE_SRC_THERM)

DEFINE_SPINLOCK(spm_lock);

void spm_go_to_normal(void)
{
    unsigned long flags;

    spin_lock_irqsave(&spm_lock, flags);
    /* reset PCM */
    spm_write(SPM_PCM_CON0, CON0_CFG_KEY | CON0_PCM_SW_RESET);
    spm_write(SPM_PCM_CON0, CON0_CFG_KEY);

    /* init PCM_CON1 (disable non-replace mode) */
    spm_write(SPM_PCM_CON1, CON1_CFG_KEY | CON1_SPM_SRAM_ISO_B |
                            CON1_SPM_SRAM_SLP_B | CON1_MIF_APBEN);

    /* tell IM where is PCM code */
    spm_write(SPM_PCM_IM_PTR, spm_get_base_phys(pcm_normal.base));
    spm_write(SPM_PCM_IM_LEN, pcm_normal.size - 1);

    /* unmask wakeup source */
    spm_write(SPM_SLEEP_WAKEUP_EVENT_MASK, ~WAKE_SRC_FOR_NORMAL);

    /* kick IM and PCM to run */
    spm_write(SPM_PCM_CON0, CON0_CFG_KEY | CON0_IM_KICK | CON0_PCM_KICK);
    spm_write(SPM_PCM_CON0, CON0_CFG_KEY);
    spin_unlock_irqrestore(&spm_lock, flags);
}

static irqreturn_t spm_irq_handler(int irq, void *dev_id)
{
    spm_error("ISR SHOULD NOT BE EXECUTED (0x%x)\n", spm_read(SPM_SLEEP_ISR_STATUS));

    spin_lock(&spm_lock);
    /* clean ISR status */
    spm_write(SPM_SLEEP_ISR_MASK, 0x0f0c);
    spm_write(SPM_SLEEP_ISR_STATUS, 0x000c);
    spm_write(SPM_PCM_SW_INT_CLEAR, 0x0001);    /* PCM_SWINT_0 */
    spin_unlock(&spm_lock);

    return IRQ_HANDLED;
}

void spm_module_init(void)
{
    int r;
    unsigned long flags;
    struct wd_api *wd_api;

    spin_lock_irqsave(&spm_lock, flags);
    /* enable register control */
    spm_write(SPM_POWERON_CONFIG_SET, (SPM_PROJECT_CODE << 16) | (1U << 0));

    /* init power control register (select PCM clock to 26M) */
    spm_write(SPM_POWER_ON_VAL0, 0);
    spm_write(SPM_POWER_ON_VAL1, 0x00015820);
    spm_write(SPM_PCM_PWR_IO_EN, 0);

    /* reset PCM */
    spm_write(SPM_PCM_CON0, CON0_CFG_KEY | CON0_PCM_SW_RESET);
    spm_write(SPM_PCM_CON0, CON0_CFG_KEY);

    /* init PCM control register */
    spm_write(SPM_PCM_CON0, CON0_CFG_KEY | CON0_IM_SLEEP_DVS);
    spm_write(SPM_PCM_CON1, CON1_CFG_KEY | CON1_SPM_SRAM_ISO_B |
                            CON1_SPM_SRAM_SLP_B | CON1_IM_NONRP_EN | CON1_MIF_APBEN);
    spm_write(SPM_PCM_IM_PTR, 0);
    spm_write(SPM_PCM_IM_LEN, 0);

    /* SRCLKENA: POWER_ON_VAL1 (PWR_IO_EN[7]=0) or POWER_ON_VAL1|r7 (PWR_IO_EN[7]=1) */
    /* CLKSQ: POWER_ON_VAL0 (PWR_IO_EN[0]=0) or r0 (PWR_IO_EN[0]=1) */
    /* SRCLKENAI will trigger 26M-wake/sleep event */
    spm_write(SPM_CLK_CON, CC_CXO32K_RM_EN_MD);
    spm_write(SPM_PCM_SRC_REQ, (1U << 1));

    /* clean wakeup event raw status */
    spm_write(SPM_SLEEP_WAKEUP_EVENT_MASK, 0xffffffff);

    /* clean ISR status */
    spm_write(SPM_SLEEP_ISR_MASK, 0x0f0c);
    spm_write(SPM_SLEEP_ISR_STATUS, 0x000c);
    spm_write(SPM_PCM_SW_INT_CLEAR, 0x000f);
    spin_unlock_irqrestore(&spm_lock, flags);

    r = request_irq(MT_SPM_IRQ_ID, spm_irq_handler, IRQF_TRIGGER_LOW,
                    "mt-spm", NULL);
    if (r) {
        spm_error("FAILED TO REQUEST SPM IRQ (%d)\n", r);
        WARN_ON(1);
    }

    get_wd_api(&wd_api);
    if (wd_api->wd_spmwdt_mode_config && wd_api->wd_thermal_mode_config) {
        wd_api->wd_spmwdt_mode_config(WD_REQ_EN, WD_REQ_RST_MODE);
        wd_api->wd_thermal_mode_config(WD_REQ_EN, WD_REQ_RST_MODE);
    } else {
        spm_error("FAILED TO GET WD API\n");
        WARN_ON(1);
    }

    spm_go_to_normal();     /* let PCM help to do thermal protection */
}


/**************************************
 * for PCM WDT to replace RGU WDT
 **************************************/
int spm_wdt_register_fiq(fiq_isr_handler rgu_wdt_handler)
{
    int r;

    r = request_fiq(MT_SPM1_IRQ_ID, rgu_wdt_handler, IRQF_TRIGGER_FALLING, NULL);

    return r;
}

int spm_wdt_register_irq(irq_handler_t rgu_wdt_handler)
{
    int r;

    r = request_irq(MT_SPM1_IRQ_ID, rgu_wdt_handler, IRQF_TRIGGER_FALLING,
                    "mt-spm1", NULL);

    return r;
}

void spm_wdt_set_timeout(u32 sec)
{
    unsigned long flags;

    spin_lock_irqsave(&spm_lock, flags);
    BUG_ON(sec > 36 * 3600);
    spm_write(SPM_PCM_WDT_TIMER_VAL, sec * 32768);
    spin_unlock_irqrestore(&spm_lock, flags);
}

void spm_wdt_enable_timer(void)
{
    unsigned long flags;

    spin_lock_irqsave(&spm_lock, flags);
    spm_write(SPM_PCM_CON1, spm_read(SPM_PCM_CON1) | CON1_CFG_KEY |
                            CON1_PCM_WDT_WAKE_MODE | CON1_PCM_WDT_EN);
    spin_unlock_irqrestore(&spm_lock, flags);
}

void spm_wdt_restart_timer(void)
{
    unsigned long flags;

    spin_lock_irqsave(&spm_lock, flags);
    spm_write(SPM_POWER_ON_VAL1, spm_read(SPM_POWER_ON_VAL1) | R7_WDT_KICK_P);
    spm_write(SPM_POWER_ON_VAL1, spm_read(SPM_POWER_ON_VAL1) & ~R7_WDT_KICK_P);
    spin_unlock_irqrestore(&spm_lock, flags);
}

void spm_wdt_restart_timer_nolock(void)
{
    spm_write(SPM_POWER_ON_VAL1, spm_read(SPM_POWER_ON_VAL1) | R7_WDT_KICK_P);
    spm_write(SPM_POWER_ON_VAL1, spm_read(SPM_POWER_ON_VAL1) & ~R7_WDT_KICK_P);
}

void spm_wdt_disable_timer(void)
{
    unsigned long flags;

    spin_lock_irqsave(&spm_lock, flags);
    spm_write(SPM_POWER_ON_VAL1, spm_read(SPM_POWER_ON_VAL1) | R7_WDT_KICK_P);
    spm_write(SPM_POWER_ON_VAL1, spm_read(SPM_POWER_ON_VAL1) & ~R7_WDT_KICK_P);

    spm_write(SPM_PCM_CON1, CON1_CFG_KEY | (spm_read(SPM_PCM_CON1) & ~CON1_PCM_WDT_EN));
    spm_write(SPM_PCM_SW_INT_CLEAR, 0x0002);    /* PCM_SWINT_1 */
    spin_unlock_irqrestore(&spm_lock, flags);
}


/**************************************
 * for CPU DVFS
 **************************************/
#define MAX_RETRY_COUNT (100)

int spm_dvfs_ctrl_volt(u32 value)
{
    u32 ap_dvfs_con;
    int retry = 0;

    ap_dvfs_con = spm_read(SPM_AP_DVFS_CON_SET);
    spm_write(SPM_AP_DVFS_CON_SET, (ap_dvfs_con & ~(0x7)) | value);
    udelay(5);

    while ((spm_read(SPM_AP_DVFS_CON_SET) & (0x1 << 31)) == 0)
    {
        if (retry >= MAX_RETRY_COUNT)
        {
            printk("FAIL: no response from PMIC wrapper\n");
            return -1;
        }

        retry++;
        printk("wait for ACK signal from PMIC wrapper, retry = %d\n", retry);

        udelay(5);
    }

    return 0;
}

MODULE_DESCRIPTION("MT6582 SPM Driver v0.4");
