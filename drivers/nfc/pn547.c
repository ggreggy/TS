/*
 * Copyright (C) 2010 Trusted Logic S.A.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
/******************************************************************************
 *
 *  The original Work has been changed by NXP Semiconductors.
 *
 *  Copyright (C) 2013-2020 NXP Semiconductors
 *   *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 ******************************************************************************/

#define pr_fmt(fmt)     "[pn547] %s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include "pn547.h"
#include "cold_reset.h"
#include <linux/wakelock.h>
#include <linux/of_gpio.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <asm/siginfo.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#ifdef CONFIG_ESE_SECURE
#include <linux/smc.h>
#endif
#include <soc/samsung/exynos-pmu.h>

#include "./nfc_logger/nfc_logger.h"

#include <linux/moduleparam.h>
static int wl_nfc = 2;
module_param(wl_nfc, int, 0644);

#define SIG_NFC			44
#define MAX_BUFFER_SIZE		554

/* This macro evaluates to 1 if the cold reset is requested by driver(SPI/UWB). */
#define IS_PROP_CMD_REQUESTED(flags) (flags & (MASK_ESE_COLD_RESET | RST_PROTECTION_ENABLED))
/* This macro evaluates to 1 if eSE cold reset response is received */
#define IS_PROP_RSP(buf) \
		((buf[0] == (MSG_NFCC_RSP | MSG_PROP_GID)) && ((buf[1] == ESE_CLD_RST_OID) || \
		(buf[1] == RST_PROTECTION_OID)))

#define NFC_DEBUG		0

#ifdef CONFIG_ESE_SECURE
enum secure_state {
	NOT_CHECKED,
	ESE_SECURED,
	ESE_NOT_SECURED,
};
static int nfc_ese_secured;
#endif

static struct pn547_dev *pn547_dev;

static atomic_t s_Device_opened = ATOMIC_INIT(1);

#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
static void release_ese_lock(enum p61_access_state  p61_current_state);
static int signal_handler(int state, long nfc_pid);
static void p61_get_access_state(struct pn547_dev *pn547_dev,
	enum p61_access_state *current_state);

static unsigned char svdd_sync_wait;
static unsigned char p61_trans_acc_on;
static struct semaphore dwp_onoff_release_sema;
#endif


struct pn547_dev *get_nfcc_dev_data(void)
{
	return pn547_dev;
}

#ifdef FEATURE_SN100X
void pn547_register_ese_shutdown(void (*func)(void))
{
	if (!lpcharge && pn547_dev)
		pn547_dev->ese_shutdown = func;
}
#endif

static void pn547_disable_irq(struct pn547_dev *pn547_dev)
{
	if (atomic_read(&pn547_dev->irq_enabled) == 1) {
		atomic_set(&pn547_dev->irq_enabled, 0);
		disable_irq_wake(pn547_dev->client->irq);
		disable_irq_nosync(pn547_dev->client->irq);
	}
}

static void pn547_enable_irq(struct pn547_dev *pn547_dev)
{
	if (atomic_read(&pn547_dev->irq_enabled) == 0) {
		atomic_set(&pn547_dev->irq_enabled, 1);
		enable_irq(pn547_dev->client->irq);
		enable_irq_wake(pn547_dev->client->irq);
	}
}
#ifdef FEATURE_NFC_TEST
struct class *nfc_test_class;
#endif

static void set_pd(struct pn547_dev *info, int power)
{
	if (info->ven_gpio > 0) {
		gpio_direction_output(info->ven_gpio, power);
	} else {
		unsigned int val = 0;
		int pd_active = 0;

		val = readl(info->clkctrl);
		pd_active = (val & PN547_NFC_CLKCTRL_PD_POLA);
		if (!pd_active) {
			if (power)
				val &= ~PN547_NFC_CLKCTRL_PD;
			else
				val |= PN547_NFC_CLKCTRL_PD;
		} else {
			if (power)
				val |= PN547_NFC_CLKCTRL_PD;
			else
				val &= ~PN547_NFC_CLKCTRL_PD;
		}
		writel(val, info->clkctrl);
		NFC_LOG_INFO("%s , val : %x\n", __func__, readl(info->clkctrl));
	}
}

static irqreturn_t pn547_dev_irq_handler(int irq, void *dev_id)
{
	struct pn547_dev *pn547_dev = dev_id;

	pn547_disable_irq(pn547_dev);
	if (!gpio_get_value(pn547_dev->irq_gpio)) {
		NFC_LOG_ERR("irq_gpio = %d\n",
			gpio_get_value(pn547_dev->irq_gpio));
		pn547_enable_irq(pn547_dev);
		return IRQ_HANDLED;
	}

	/* Wake up waiting readers */
	atomic_set(&pn547_dev->read_flag, 1);
	wake_up(&pn547_dev->read_wq);

	NFC_LOG_REC("irq handler called\n");

	wake_lock_timeout(&pn547_dev->nfc_wake_lock, wl_nfc*HZ);
	return IRQ_HANDLED;
}

/* clock disable: val = 1, enable: val = 0 */
static int pn547_pmu_update(struct pn547_dev *pn547_dev, u32 mask, u32 val)
{
#ifdef FEATURE_USE_PMU_DEBUG1_IOREMAP
	if (pn547_dev->pmu_debug1_ctrl) {
		u32 tmp = 0;
		u32 shift;

		for (shift = 0; shift < 32; shift++) {
			if ((mask >> shift) & 0x1)
				break;
		}

		tmp = readl(pn547_dev->pmu_debug1_ctrl);
		tmp = (tmp & ~mask) | (mask & (val << shift));
		writel(tmp, pn547_dev->pmu_debug1_ctrl);

		NFC_LOG_REC("pmu_debug1: 0x%X\n",  tmp);
	}
#else
	if (pn547_dev->pmu_debug1) {
		u32 pmu_debug1_offset = pn547_dev->pmu_debug1 & 0xFFFF;

		exynos_pmu_update(pmu_debug1_offset, mask, val);
	}
#endif

	return 0;
}

static int pn547_pmu_read(struct pn547_dev *pn547_dev, u32 *val)
{
#ifdef FEATURE_USE_PMU_DEBUG1_IOREMAP
	if (pn547_dev->pmu_debug1_ctrl)
		*val = readl(pn547_dev->pmu_debug1_ctrl);
#else
	if (pn547_dev->pmu_debug1) {
		u32 pmu_debug1_offset = pn547_dev->pmu_debug1 & 0xFFFF;

		exynos_pmu_read(pmu_debug1_offset, val);
	}
#endif

	return 0;
}

static irqreturn_t pn547_nfc_clk_irq_isr(int irq, void *dev_id)
{
	struct pn547_dev *pn547_dev = dev_id;
	int clk_req = gpio_get_value(pn547_dev->clk_req_gpio);
	u32 val;
	unsigned long flag;

	if (clk_req == pn547_dev->clk_state) {
		NFC_LOG_ERR("clk_req and clk_state are the same : %d\n", clk_req);
		return IRQ_HANDLED;
	}

	spin_lock_irqsave(&pn547_dev->clk_ctrl_slock, flag);

	if (clk_req) {
		pn547_pmu_update(pn547_dev, 1, 0);
		pn547_pmu_read(pn547_dev, &val);
		NFC_LOG_REC("clk_req_isr: %d, pmu val: 0x%x\n", clk_req, val);
	} else {
		pn547_pmu_read(pn547_dev, &val);
		NFC_LOG_REC("clk_req_isr: %d, pmu val: 0x%x\n", clk_req, val);
#ifndef FEATURE_DELAYED_CLOCK_DISABLE
		pn547_pmu_update(pn547_dev, 1, 1);
#endif
	}
	pn547_dev->clk_state = clk_req;

	spin_unlock_irqrestore(&pn547_dev->clk_ctrl_slock, flag);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t pn547_nfc_clk_irq(int irq, void *dev_id)
{
	struct pn547_dev *pn547_dev = dev_id;

	NFC_LOG_REC("clk_state: %d\n", pn547_dev->clk_state);

	if (pn547_dev->clk_state) {
		if (!pn547_dev->pmu_debug1) {
			int ret;

			ret = clk_prepare_enable(pn547_dev->nfc_clock);
			if (ret) {
				NFC_LOG_ERR("clock enable failed\n");
				return IRQ_HANDLED;
			}
		}
		if (!wake_lock_active(&pn547_dev->nfc_clk_wake_lock))
			wake_lock(&pn547_dev->nfc_clk_wake_lock);
#ifdef FEATURE_DELAYED_CLOCK_DISABLE
		cancel_delayed_work_sync(&pn547_dev->clk_ctrl_work);
#endif

	} else {
#ifdef FEATURE_DELAYED_CLOCK_DISABLE
		cancel_delayed_work_sync(&pn547_dev->clk_ctrl_work);
		schedule_delayed_work(&pn547_dev->clk_ctrl_work, msecs_to_jiffies(500));
#else
		if (!pn547_dev->pmu_debug1)
			clk_disable_unprepare(pn547_dev->nfc_clock);
#endif
		if (wake_lock_active(&pn547_dev->nfc_clk_wake_lock))
			wake_unlock(&pn547_dev->nfc_clk_wake_lock);
	}

	return IRQ_HANDLED;
}

#ifdef FEATURE_DELAYED_CLOCK_DISABLE
static void pn547_clk_ctrl_work(struct work_struct *work)
{
	unsigned long flag;
	struct pn547_dev *pn547_dev = container_of(work, struct pn547_dev, clk_ctrl_work.work);

	spin_lock_irqsave(&pn547_dev->clk_ctrl_slock, flag);
	if (!pn547_dev->clk_state) {
		pn547_pmu_update(pn547_dev, 1, 1);
		NFC_LOG_REC("clock disable\n");
	} else {
		NFC_LOG_REC("clk_req is still high\n");
	}
	spin_unlock_irqrestore(&pn547_dev->clk_ctrl_slock, flag);
}
#endif

ssize_t pn547_dev_read(struct file *filp, char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn547_dev *pn547_dev = filp->private_data;
	int ret = 0;
	char *r_buf = pn547_dev->r_buf;
#ifdef FEATURE_CORE_RESET_NTF_CHECK
	static int is_error_ntf = STATE_NORMAL;
#endif

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	NFC_LOG_DBG("reading %zu bytes. irq=%s\n", count,
			gpio_get_value(pn547_dev->irq_gpio) ? "1" : "0");

#if NFC_DEBUG
	NFC_LOG_INFO("+ r\n");
#endif

	mutex_lock(&pn547_dev->read_mutex);
	memset(r_buf, 0, count);

	if (!gpio_get_value(pn547_dev->irq_gpio)) {
		atomic_set(&pn547_dev->read_flag, 0);
		if (filp->f_flags & O_NONBLOCK) {
			NFC_LOG_ERR("O_NONBLOCK\n");
			ret = -EAGAIN;
			goto fail;
		}

#if NFC_DEBUG
		NFC_LOG_INFO("wait_event_interruptible : in\n");
#endif
		while (1) {
			if (!gpio_get_value(pn547_dev->irq_gpio)) {
				pn547_enable_irq(pn547_dev);
				ret = wait_event_interruptible(pn547_dev->read_wq,
						atomic_read(&pn547_dev->read_flag));
			}
			pn547_disable_irq(pn547_dev);

#if NFC_DEBUG
			NFC_LOG_INFO("h\n");
#endif

			if (pn547_dev->cancel_read) {
				pn547_dev->cancel_read = false;
				ret = -1;
				goto fail;
			}

			if (ret)
				goto fail;

#ifdef FEATURE_SN100X
			if (pn547_dev->state_flags & PN547_STATE_NFC_VEN_RESET) {
				NFC_LOG_ERR("releasing read\n");
				pn547_dev->state_flags &= ~PN547_STATE_NFC_VEN_RESET;
				ret =  -EL3RST;
				goto fail;
			}
#endif
			if (gpio_get_value(pn547_dev->irq_gpio))
				break;

			/*
			 * NFC service wanted to close the driver so,
			 * release the calling reader thread asap.
			 *
			 * This can happen in case of nfc node close call from
			 * eSE HAL in that case the NFC HAL reader thread
			 * will again call read system call
			 */
			if (pn547_dev->release_read) {
				NFC_LOG_ERR("%s: releasing read\n", __func__);
				mutex_unlock(&pn547_dev->read_mutex);
				return 0;
			}

			NFC_LOG_ERR("spurious interrupt detected\n");
		}
	}

	/* Read data */
	ret = i2c_master_recv(pn547_dev->client, r_buf, count);
	NFC_LOG_REC("recv size : %d\n", ret);

#if NFC_DEBUG
	NFC_LOG_INFO("i2c_master_recv\n");
#endif

#ifdef FEATURE_SN100X
	/* If ese cold reset has been requested then read the response */
	if (IS_PROP_CMD_REQUESTED(pn547_dev->state_flags) && IS_PROP_RSP(r_buf)) {
		rcv_prop_resp_status(r_buf);
		/* Request is from driver, consume the response */
		mutex_unlock(&pn547_dev->read_mutex);
		return 0;
	}
#endif
	mutex_unlock(&pn547_dev->read_mutex);
	/*
	 * pn547 seems to be slow in handling I2C read requests
	 * so add 1ms delay after recv operation
	 */
	udelay(1000);

	if (ret < 0) {
		NFC_LOG_ERR("i2c_master_recv returned (%d,%d)\n",
				ret, pn547_dev->i2c_probe);
		return ret;
	}

	if (ret > count) {
		NFC_LOG_ERR("received too many bytes from i2c (%d)\n",
				ret);
		return -EIO;
	}

#ifdef FEATURE_CORE_RESET_NTF_CHECK
	if (is_error_ntf == STATE_CORE_RESET && ret == 6) {
		u64 ntf_hex = *(u64 *)r_buf & 0xFFFFFFFFFFFFULL;

		switch (ntf_hex) {
		case CORE_RESET_NTF_NO_CLOCK:
			NFC_LOG_ERR("CORE_RESET_NTF: No clock\n");
			break;
		case CORE_RESET_NTF_CLOCK_LOST:
			NFC_LOG_ERR("CORE_RESET_NTF: clock lost\n");
			break;
		default:
			NFC_LOG_INFO("CORE_RESET_NTF: %02X%02X%02X%02X%02X%02X\n",
				r_buf[0], r_buf[1], r_buf[2], r_buf[3], r_buf[4], r_buf[5]);
		}
	} else if (is_error_ntf == STATE_ABNORMAL_POWER && ret == 8) {
		NFC_LOG_INFO("ABNORMAL_POWER(DPD): %02X%02X%02X%02X %02X%02X%02X%02X\n",
			r_buf[0], r_buf[1], r_buf[2], r_buf[3], r_buf[4], r_buf[5], r_buf[6], r_buf[7]);
	}

	/* check CORE_RESET_NTF */
	if (ret == 3 && r_buf[0] == 0x60 && r_buf[1] == 0x00 && r_buf[2] == 0x06)
		is_error_ntf = STATE_CORE_RESET;
	else if (ret == 3 && r_buf[0] == 0x6F && r_buf[1] == 0x2E && r_buf[2] == 0x08)
		is_error_ntf = STATE_ABNORMAL_POWER;
	else
		is_error_ntf = STATE_NORMAL;
#endif

	if (copy_to_user(buf, r_buf, ret)) {
		NFC_LOG_ERR("failed to copy to user space\n");
		return -EFAULT;
	}
	return ret;

fail:
	mutex_unlock(&pn547_dev->read_mutex);
	return ret;
}

static ssize_t pn547_dev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn547_dev *pn547_dev = filp->private_data;
	char *w_buf = pn547_dev->w_buf;
	int ret = 0, retry = 2;

#if NFC_DEBUG
	NFC_LOG_INFO("+ w\n");
#endif

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	ret = copy_from_user(w_buf, buf, count);
	if (ret) {
		NFC_LOG_ERR("failed to copy from user space %d\n", ret);
		return -EFAULT;
	}

	NFC_LOG_REC("writing %zu bytes.\n", count);
	/* Write data */
	do {
		retry--;
		ret = i2c_master_send(pn547_dev->client, w_buf, count);
		if (ret == count)
			break;
		usleep_range(6000, 10000); /* Retry, chip was in standby */
#if NFC_DEBUG
		NFC_LOG_INFO("retry = %d\n", retry);
#endif
	} while (retry);

#if NFC_DEBUG
	NFC_LOG_INFO("- w\n");
#endif

	if (ret != count) {
		NFC_LOG_ERR("i2c_master_send returned (%d,%d)\n",
				ret, pn547_dev->i2c_probe);
		ret = -EIO;
	}

	return ret;
}

static int pn547_dev_open(struct inode *inode, struct file *filp)
{
	struct pn547_dev *pn547_dev = container_of(filp->private_data,
						   struct pn547_dev,
						   pn547_device);

	mutex_lock(&pn547_dev->dev_ref_mutex);

	if (!atomic_dec_and_test(&s_Device_opened)) {
		atomic_inc(&s_Device_opened);
		NFC_LOG_ERR("already opened!\n");
		mutex_unlock(&pn547_dev->dev_ref_mutex);
		return -EBUSY;
	}

	filp->private_data = pn547_dev;
#ifdef FEATURE_SN100X
	pn547_dev->state_flags |= PN547_STATE_NFC_ON;
#endif
	NFC_LOG_INFO("imajor:%d, iminor:%d (%d)\n", imajor(inode), iminor(inode),
			pn547_dev->i2c_probe);
	nfc_logger_set_max_count(-1);

#ifdef CONFIG_ESE_SECURE
	if (nfc_ese_secured == NOT_CHECKED) {
		int ret = 0;

		ret = exynos_smc(0x83000032, 0x1, 0, 0);
		if (ret == EBUSY) {
			nfc_ese_secured = ESE_NOT_SECURED;
			NFC_LOG_ERR("eSE spi is not Secured\n");
			mutex_unlock(&pn547_dev->dev_ref_mutex);
			return -EBUSY;
		}
		nfc_ese_secured = ESE_SECURED;
	} else if (nfc_ese_secured == ESE_NOT_SECURED) {
		NFC_LOG_ERR("eSE spi is not Secured\n");
		mutex_unlock(&pn547_dev->dev_ref_mutex);
		return -EBUSY;
	}
#endif
	mutex_unlock(&pn547_dev->dev_ref_mutex);

	return 0;
}

static int pn547_dev_flush(struct file *pfile, fl_owner_t id)
{
	struct pn547_dev *pn547_dev = pfile->private_data;

	if (!pn547_dev) {
		NFC_LOG_ERR("%s: pn547 instance is NULL!!\n", __func__);
		return -ENODEV;
	}
	/*
	 * release blocked user thread waiting for pending read during close
	 */
	if (!mutex_trylock(&pn547_dev->read_mutex)) {
		pn547_dev->release_read = true;
		atomic_set(&pn547_dev->read_flag, 1);
		pn547_disable_irq(pn547_dev);
		wake_up(&pn547_dev->read_wq);
		NFC_LOG_ERR("%s: waiting for release of blocked read\n", __func__);
		mutex_lock(&pn547_dev->read_mutex);
		pn547_dev->release_read = false;
	} else {
		NFC_LOG_ERR("%s: read thread already released\n", __func__);
	}
	mutex_unlock(&pn547_dev->read_mutex);
	return 0;
}

static int pn547_dev_release(struct inode *inode, struct file *filp)
{
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	enum p61_access_state current_state;
#endif

	NFC_LOG_INFO("release\n");
	mutex_lock(&pn547_dev->dev_ref_mutex);
	set_force_reset(false);
	if (pn547_dev->firm_gpio)
		gpio_set_value(pn547_dev->firm_gpio, 0);

#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	p61_get_access_state(pn547_dev, &current_state);
	if ((p61_trans_acc_on ==  1) && ((current_state &
			(P61_STATE_SPI|P61_STATE_SPI_PRIO)) == 0))
		release_ese_lock(P61_STATE_WIRED);
#endif
#ifdef FEATURE_SN100X
	pn547_dev->state_flags &= ~(PN547_STATE_NFC_VEN_RESET | PN547_STATE_NFC_ON | PN547_STATE_FW_DNLD);
#endif
	atomic_inc(&s_Device_opened);

	mutex_unlock(&pn547_dev->dev_ref_mutex);
	return 0;
}

#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
static void dwp_onoff(long nfc_service_pid, enum p61_access_state origin)
{
	int timeout = 500; /*500 ms timeout*/
	unsigned long tempJ = msecs_to_jiffies(timeout);

	if (nfc_service_pid) {
		if (signal_handler(origin, nfc_service_pid) == 0) {
			reinit_completion(&pn547_dev->dwp_onoff_comp);
			if (!wait_for_completion_timeout(&pn547_dev->dwp_onoff_comp, tempJ))
				NFC_LOG_INFO("wait protection: Timeout\n");

			NFC_LOG_INFO("wait protection : released\n");
		}
	}
}
static int release_dwp_onoff(void)
{
	int timeout = 500; //500 ms timeout
	unsigned long tempJ = msecs_to_jiffies(timeout);

	NFC_LOG_INFO("enter\n");
	complete(&pn547_dev->dwp_onoff_comp);
	{
		if (down_timeout(&dwp_onoff_release_sema, tempJ) != 0)
			NFC_LOG_INFO("Dwp On/off release wait protection: Timeout");

		NFC_LOG_INFO("Dwp On/Off release wait protection : released");
	}
	return 0;
}

static int set_nfc_pid(unsigned long arg)
{
	pid_t pid = arg;
	struct task_struct *task = NULL;

	pn547_dev->nfc_service_pid = arg;

	if (arg == 0)
		goto done;

	task = get_pid_task(find_vpid(pid), PIDTYPE_PID);
	if (task) {
		NFC_LOG_INFO("task->comm: %s\n", task->comm);
		if (!strncmp(task->comm, "com.android.nfc", 15)) {
			pn547_dev->nfc_service_pid = arg;
			goto done;
		} else {
			NFC_LOG_INFO("it's not nfc pid : %ld, %s\n",
				pn547_dev->nfc_service_pid, task->comm);
		}
	}

	pn547_dev->nfc_service_pid = 0;
done:
	if (task)
		put_task_struct(task);

	NFC_LOG_INFO("The NFC Service PID is %ld\n", pn547_dev->nfc_service_pid);

	return 0;
}

static void p61_update_access_state(struct pn547_dev *pn547_dev,
		enum p61_access_state current_state, bool set)
{
	if (current_state) {
		if (set) {
			if (pn547_dev->p61_current_state == P61_STATE_IDLE)
				pn547_dev->p61_current_state
						= P61_STATE_INVALID;
			pn547_dev->p61_current_state |= current_state;
		} else {
			pn547_dev->p61_current_state &= (unsigned int)(~current_state);
			if (!pn547_dev->p61_current_state)
				pn547_dev->p61_current_state = P61_STATE_IDLE;
		}
	}
	NFC_LOG_INFO("Exit current_state = 0x%x\n",
			pn547_dev->p61_current_state);
}

static void p61_get_access_state(struct pn547_dev *pn547_dev,
	enum p61_access_state *current_state)
{
	if (current_state == NULL)
		NFC_LOG_ERR("invalid state of p61_access_state\n");
	else
		*current_state = pn547_dev->p61_current_state;
}

static void p61_access_lock(struct pn547_dev *pn547_dev)
{
	mutex_lock(&pn547_dev->p61_state_mutex);
}

static void p61_access_unlock(struct pn547_dev *pn547_dev)
{
	mutex_unlock(&pn547_dev->p61_state_mutex);
}

static int signal_handler(int state, long nfc_pid)
{
	struct siginfo sinfo;
	pid_t pid;
	struct task_struct *task = NULL;
	int sigret = 0;
	int ret = 0;

	NFC_LOG_INFO("pid:%ld\n", nfc_pid);
	if (nfc_pid == 0) {
		NFC_LOG_ERR("nfc_pid is clear don't call.\n");
		return -EPERM;
	}

	memset(&sinfo, 0, sizeof(struct siginfo));
	sinfo.si_signo = SIG_NFC;
	sinfo.si_code = SI_QUEUE;
	sinfo.si_int = state;
	pid = nfc_pid;

	task = get_pid_task(find_vpid(pid), PIDTYPE_PID);
	if (task) {
		NFC_LOG_INFO("task->comm: %s.\n", task->comm);
		sigret = send_sig_info(SIG_NFC, &sinfo, task);
		if (sigret < 0) {
			NFC_LOG_ERR("send_sig_info failed.. %d.\n", sigret);
			ret = -EPERM;
		}

		put_task_struct(task);
	} else {
		NFC_LOG_ERR("finding task from PID failed\n");
		ret = -EPERM;
	}

	return ret;
}

static void svdd_sync_onoff(long nfc_service_pid, enum p61_access_state origin)
{
	int timeout = 500; /*500 ms timeout*/
	unsigned long tempJ = msecs_to_jiffies(timeout);

	NFC_LOG_INFO("Enter nfc_service_pid: %ld\n", nfc_service_pid);
	if (nfc_service_pid) {
		if (signal_handler(origin, nfc_service_pid) == 0) {
			reinit_completion(&pn547_dev->svdd_sync_comp);
			svdd_sync_wait = 1;
			NFC_LOG_INFO("Waiting for svdd protection response");
			/*if (down_timeout(&svdd_sync_onoff_sema, tempJ) != 0)*/
			if (!wait_for_completion_timeout(&pn547_dev->svdd_sync_comp, tempJ))
				NFC_LOG_ERR("svdd wait protection: Timeout");

			NFC_LOG_INFO("svdd wait protection : released");
			svdd_sync_wait = 0;
		}
	}
	NFC_LOG_INFO("Exit\n");
}

static int release_svdd_wait(void)
{
	unsigned char i = 0;

	NFC_LOG_INFO("Enter\n");
	for (i = 0; i < 9; i++) {
		if (svdd_sync_wait) {
			complete(&pn547_dev->svdd_sync_comp);
			svdd_sync_wait = 0;
			break;
		}
		usleep_range(10000, 10100);
	}
	NFC_LOG_INFO("Exit\n");
	return 0;
}

static int pn547_set_pwr(struct pn547_dev *pdev, unsigned long arg)
{
	int ret = 0;
	enum p61_access_state current_state;

	p61_get_access_state(pdev, &current_state);
	switch (arg) {
	case MODE_POWER_OFF:
		if (atomic_read(&pdev->irq_enabled) == 1) {
			atomic_set(&pdev->irq_enabled, 0);
			disable_irq_wake(pdev->client->irq);
			disable_irq_nosync(pdev->client->irq);
		}
		if (wake_lock_active(&pn547_dev->nfc_clk_wake_lock))
			wake_unlock(&pn547_dev->nfc_clk_wake_lock);

		if (current_state & P61_STATE_DWNLD)
			p61_update_access_state(pdev, P61_STATE_DWNLD, false);

		if ((current_state & (P61_STATE_WIRED|P61_STATE_SPI
			|P61_STATE_SPI_PRIO)) == 0)
			p61_update_access_state(pdev, P61_STATE_IDLE, true);

		NFC_LOG_INFO("power off, irq=%d\n", atomic_read(&pdev->irq_enabled));
		gpio_set_value(pdev->firm_gpio, 0);

		pdev->nfc_ven_enabled = false;
		/* Don't change Ven state if spi made it high */
#ifndef VEN_ALWAYS_ON
		if (pdev->spi_ven_enabled == false)
			set_pd(pn547_dev, PN547_NFC_PW_OFF);
#endif
		usleep_range(4900, 5000);
		break;
	case MODE_POWER_ON:
		if ((current_state & (P61_STATE_WIRED|P61_STATE_SPI|P61_STATE_SPI_PRIO)) == 0)
			p61_update_access_state(pdev, P61_STATE_IDLE, true);

		if (current_state & P61_STATE_DWNLD)
			p61_update_access_state(pdev, P61_STATE_DWNLD, false);

		gpio_set_value(pdev->firm_gpio, 0);
#ifdef FEATURE_SN100X
		pdev->state_flags &= ~(PN547_STATE_FW_DNLD);
#endif

		pdev->nfc_ven_enabled = true;
#ifndef VEN_ALWAYS_ON
		if (pdev->spi_ven_enabled == false)
			set_pd(pn547_dev, PN547_NFC_PW_ON);
#endif
		usleep_range(4900, 5000);
		pn547_enable_irq(pdev);
		svdd_sync_wait = 0;

		NFC_LOG_INFO("power on, irq=%d\n", atomic_read(&pdev->irq_enabled));
		break;
	case MODE_FW_DWNLD_WITH_VEN:
		if (current_state & (P61_STATE_SPI|P61_STATE_SPI_PRIO)) {
			/* NFCC fw/download should not be allowed if p61 is used by SPI */
			NFC_LOG_ERR("not be allowed to reset/FW download\n");
			return -EBUSY; /* Device or resource busy */
		}
		pdev->nfc_ven_enabled = true;
		if (pdev->spi_ven_enabled == false) {
			/* power on with firmware download (requires hw reset) */
			p61_update_access_state(pdev, P61_STATE_DWNLD, true);
			gpio_set_value(pdev->firm_gpio, 1);
#ifdef FEATURE_SN100X
			pdev->state_flags |= (PN547_STATE_FW_DNLD);
#endif
			usleep_range(4900, 5000);
			set_pd(pn547_dev, PN547_NFC_PW_OFF);
			usleep_range(14900, 15000);
			set_pd(pn547_dev, PN547_NFC_PW_ON);
			usleep_range(4900, 5000);
			pn547_enable_irq(pdev);
			NFC_LOG_INFO("power on with firmware, irq=%d\n",
				atomic_read(&pdev->irq_enabled));
			NFC_LOG_INFO("VEN=%d FIRM=%d\n", gpio_get_value(pdev->ven_gpio),
				gpio_get_value(pdev->firm_gpio));
		}
		break;
#ifdef FEATURE_SN100X
	case MODE_ISO_RST:
		/*NFC Service called ISO-RST*/
		if (current_state & (P61_STATE_SPI|P61_STATE_SPI_PRIO))
			return -EPERM; /* Operation not permitted */

		if (current_state & P61_STATE_WIRED)
			p61_update_access_state(pn547_dev, P61_STATE_WIRED, false);
#ifdef ISO_RST
		gpio_set_value(pn547_dev->iso_rst_gpio, 0);
		msleep(50);
		gpio_set_value(pn547_dev->iso_rst_gpio, 1);
		msleep(50);
		NFC_LOG_INFO("%s ISO RESET from DWP DONE\n", __func__);
#endif
		break;
	case MODE_FW_DWND_HIGH:
		NFC_LOG_INFO("%s FW dwldioctl called from NFC \n", __func__);
		/*NFC Service called FW dwnld*/
		if (pn547_dev->firm_gpio) {
			p61_update_access_state(pn547_dev, P61_STATE_DWNLD, true);
			gpio_set_value(pn547_dev->firm_gpio, 1);
#ifdef FEATURE_SN100X
			pn547_dev->state_flags |= PN547_STATE_FW_DNLD;
#endif
			usleep_range(10000, 10100);
		}
		break;
	case MODE_POWER_RESET:
#ifdef FEATURE_SN100X
		pn547_dev->state_flags |= PN547_STATE_NFC_VEN_RESET;
#endif
		wake_up(&pn547_dev->read_wq);
		usleep_range(10000, 10100);
		gpio_set_value(pn547_dev->ven_gpio, 0);
		usleep_range(15000, 15100);
		gpio_set_value(pn547_dev->ven_gpio, 1);
		usleep_range(10000, 10100);
		NFC_LOG_INFO("VEN reset DONE >>>>>>>\n");
		break;
	case MODE_FW_GPIO_LOW:
		if (pn547_dev->firm_gpio) {
			gpio_set_value(pn547_dev->firm_gpio, 0);
#ifdef FEATURE_SN100X
			pn547_dev->state_flags &= ~(PN547_STATE_FW_DNLD);
#endif
			p61_update_access_state(pn547_dev, P61_STATE_DWNLD, false);
		}
		NFC_LOG_INFO("FW GPIO set to 0x00 >>>>>>>\n");
		break;

#elif defined(FEATURE_PN80T)
	case MODE_ISO_RST:
		NFC_LOG_INFO("Read Cancel\n");
		pdev->cancel_read = true;
		atomic_set(&pdev->read_flag, 1);
		wake_up(&pdev->read_wq);
		break;
#endif
	default:
		NFC_LOG_ERR("bad arg %lu\n", arg);
		/* changed the p61 state to idle*/
		ret = -EINVAL;
	}
	return ret;
}

static int pn547_p61_set_spi_pwr(struct pn547_dev *pdev,
	unsigned long arg)
{
	int ret = 0;
	enum p61_access_state current_state;

	p61_get_access_state(pn547_dev, &current_state);
	NFC_LOG_INFO("PN61_SET_SPI_PWR cur=0x%x\n", current_state);
	switch (arg) {
	case 0: /*else if (arg == 0)*/
#if defined(FEATURE_PN80T) || defined(FEATURE_SN100X)
		NFC_LOG_INFO("power off ese PN80T\n");
		if (current_state & P61_STATE_SPI_PRIO) {
			p61_update_access_state(pn547_dev,
				P61_STATE_SPI_PRIO, false);
			if (!(current_state & P61_STATE_JCP_DWNLD)) {
				if (pn547_dev->nfc_service_pid) {
					NFC_LOG_INFO("nfc service pid %ld\n", pn547_dev->nfc_service_pid);
					if (!(current_state & P61_STATE_WIRED)) {
						svdd_sync_onoff(pn547_dev->nfc_service_pid,
							P61_STATE_SPI_SVDD_SYNC_START|P61_STATE_SPI_PRIO_END);
					} else
						signal_handler(P61_STATE_SPI_PRIO_END,
							pn547_dev->nfc_service_pid);
				} else {
					NFC_LOG_INFO("invalid nfc svc pid %ld\n",
						pn547_dev->nfc_service_pid);
				}
			} else if (!(current_state & P61_STATE_WIRED)) {
				svdd_sync_onoff(pn547_dev->nfc_service_pid,
					P61_STATE_SPI_SVDD_SYNC_START);
			}
			pn547_dev->spi_ven_enabled = false;
#ifndef VEN_ALWAYS_ON
			if (!(current_state & P61_STATE_WIRED)) {
#ifdef FEATURE_PN80T
				gpio_set_value(pn547_dev->ese_pwr_req, 0);
				ese_spi_pinctrl(0);/*for factory spi pinctrl*/
#endif
				msleep(60);
				svdd_sync_onoff(pn547_dev->nfc_service_pid,
						P61_STATE_SPI_SVDD_SYNC_END);
			}
			if (pn547_dev->nfc_ven_enabled == false) {
				gpio_set_value(pn547_dev->ven_gpio, 0);
				usleep_range(10000, 10100);
			}
#endif
		} else if (current_state & P61_STATE_SPI) {
			if (!(current_state & P61_STATE_WIRED) && !(current_state & P61_STATE_JCP_DWNLD)) {
				if (pn547_dev->nfc_service_pid) {
					NFC_LOG_INFO("nfc service pid ---- %ld\n",
						pn547_dev->nfc_service_pid);
					svdd_sync_onoff(pn547_dev->nfc_service_pid,
						P61_STATE_SPI_SVDD_SYNC_START|P61_STATE_SPI_END);
				} else {
					NFC_LOG_ERR("invalid nfc svc pid %ld\n",
						pn547_dev->nfc_service_pid);
				}
#if !defined(VEN_ALWAYS_ON) && defined(FEATURE_PN80T)
				gpio_set_value(pn547_dev->ese_pwr_req, 0);
				ese_spi_pinctrl(0);
				msleep(60);
#endif
				p61_update_access_state(pn547_dev,
					P61_STATE_SPI, false);
				svdd_sync_onoff(pn547_dev->nfc_service_pid,
						P61_STATE_SPI_SVDD_SYNC_END);
			}
			/*If JCOP3.2 or 3.3 for handling triple mode protection signal NFC service */
			else {
				if (!(current_state & P61_STATE_JCP_DWNLD)) {
					if (pn547_dev->nfc_service_pid) {
						NFC_LOG_INFO("nfc svc pid %ld\n",
							pn547_dev->nfc_service_pid);
						svdd_sync_onoff(pn547_dev->nfc_service_pid,
							P61_STATE_SPI_SVDD_SYNC_START|P61_STATE_SPI_END);
					} else {
						NFC_LOG_ERR("invalid nfc svc pid %ld\n",
							pn547_dev->nfc_service_pid);
					}
				} else {
					svdd_sync_onoff(
						pn547_dev->nfc_service_pid,
						P61_STATE_SPI_SVDD_SYNC_START);
				}
#ifndef VEN_ALWAYS_ON
#ifdef FEATURE_PN80T
				gpio_set_value(pn547_dev->ese_pwr_req, 0);
				ese_spi_pinctrl(0);
				msleep(60);
#endif
				p61_update_access_state(pn547_dev,
						P61_STATE_SPI, false);
				svdd_sync_onoff(pn547_dev->nfc_service_pid,
					P61_STATE_SPI_SVDD_SYNC_END);
				NFC_LOG_INFO("PN80T ese_pwr_gpio off");
#endif
			}
			pn547_dev->spi_ven_enabled = false;
#ifndef VEN_ALWAYS_ON
			if (pn547_dev->nfc_ven_enabled == false) {
				gpio_set_value(pn547_dev->ven_gpio, 0);
				usleep_range(10000, 10100);
			}
#endif
		} else {
			NFC_LOG_ERR("power off ese failed, current_state = 0x%x\n",
				pn547_dev->p61_current_state);
			ret = -EPERM; /* Operation not permitted */
		}
#else
		NFC_LOG_INFO("power off ese\n");
		if (current_state & P61_STATE_SPI_PRIO) {
			p61_update_access_state(pn547_dev, P61_STATE_SPI_PRIO, false);
			if (!(current_state & P61_STATE_WIRED)) {
				svdd_sync_onoff(pn547_dev->nfc_service_pid,
					P61_STATE_SPI_SVDD_SYNC_START|P61_STATE_SPI_PRIO_END);
#ifdef FEATURE_PN80T
				gpio_set_value(pn547_dev->ese_pwr_req, 0);
				ese_spi_pinctrl(0);
				msleep(60);
#endif
				svdd_sync_onoff(pn547_dev->nfc_service_pid,
						P61_STATE_SPI_SVDD_SYNC_END);
			} else {
				if (pn547_dev->nfc_service_pid) {
					NFC_LOG_INFO("nfc service pid %ld\n",
						pn547_dev->nfc_service_pid);
					signal_handler(P61_STATE_SPI_PRIO_END,	pn547_dev->nfc_service_pid);
				} else {
					NFC_LOG_ERR("invalid nfc svc pid %ld\n",
						pn547_dev->nfc_service_pid);
				}
			}
			pn547_dev->spi_ven_enabled = false;
			if (pn547_dev->nfc_ven_enabled == false) {
				gpio_set_value(pn547_dev->ven_gpio, 0);
				usleep_range(10000, 10100);
			}
		} else if (current_state & P61_STATE_SPI) {
			p61_update_access_state(pn547_dev,
					P61_STATE_SPI, false);
			if (!(current_state & P61_STATE_WIRED)) {
				svdd_sync_onoff(pn547_dev->nfc_service_pid,
					P61_STATE_SPI_SVDD_SYNC_START|P61_STATE_SPI_END);
#ifdef FEATURE_PN80T
				gpio_set_value(pn547_dev->ese_pwr_req, 0);
				ese_spi_pinctrl(0);
				msleep(60);
#endif
				svdd_sync_onoff(pn547_dev->nfc_service_pid,
					P61_STATE_SPI_SVDD_SYNC_END);
			}
			/*If JCOP3.2 or 3.3 for handling triple mode protection signal NFC service */
			else {
				if (pn547_dev->nfc_service_pid) {
					NFC_LOG_INFO("nfc service pid %ld\n",
						pn547_dev->nfc_service_pid);
					signal_handler(P61_STATE_SPI_END, pn547_dev->nfc_service_pid);
				} else {
					NFC_LOG_ERR("invalid nfc service pid.. %ld\n",
						pn547_dev->nfc_service_pid);
				}
			}
			pn547_dev->spi_ven_enabled = false;
			if (pn547_dev->nfc_ven_enabled == false) {
				gpio_set_value(pn547_dev->ven_gpio, 0);
				usleep_range(10000, 10100);
			}
		} else {
			NFC_LOG_ERR("power off ese failed, current_state = 0x%x\n",
				pn547_dev->p61_current_state);
			ret = -EPERM; /* Operation not permitted */
		}
#endif
		break;
	case 1: /* if (arg == 1) */
		NFC_LOG_INFO("power on ese\n");
		if ((current_state & (P61_STATE_SPI|P61_STATE_SPI_PRIO
				|P61_STATE_DWNLD)) == 0) {
			/* To handle triple mode protection signal NFC service when SPI session started */
			if (!(current_state & P61_STATE_JCP_DWNLD)) {
				if (pn547_dev->nfc_service_pid) {
					NFC_LOG_INFO("nfc service pid ---- %ld\n",
						pn547_dev->nfc_service_pid);
					/* signal_handler(P61_STATE_SPI, pn547_dev->nfc_service_pid); */
					dwp_onoff(pn547_dev->nfc_service_pid, P61_STATE_SPI);
				} else {
					NFC_LOG_ERR("invalid nfc svc pid %ld\n",
						pn547_dev->nfc_service_pid);
				}
			}
			pn547_dev->spi_ven_enabled = true;
#ifndef VEN_ALWAYS_ON
			if (pn547_dev->nfc_ven_enabled == false) {
			/* provide power to NFCC if, NFC service not provided */
				gpio_set_value(pn547_dev->ven_gpio, 1);
				usleep_range(10000, 10100);
			}
			/* pull the gpio to high once NFCC is power on*/
#ifdef FEATURE_PN80T
			gpio_set_value(pn547_dev->ese_pwr_req, 1);
			ese_spi_pinctrl(1);
			usleep_range(10000, 10100);
#endif
			p61_update_access_state(pn547_dev, P61_STATE_SPI, true);
			/*
			 * Releasing the DWP Link ON/OF release ioctl after
			 * protecting the critical area
			 */
			if (pn547_dev->nfc_service_pid)
				up(&dwp_onoff_release_sema);
#endif
		} else {
			NFC_LOG_ERR("PN61_SET_SPI_PWR - power on ese failed\n");
			ret = -EBUSY; /* Device or resource busy */
		}
		break;
	case 2: /* else if (arg == 2) */
		NFC_LOG_INFO("reset\n");
		if (current_state &
		(P61_STATE_IDLE|P61_STATE_SPI|P61_STATE_SPI_PRIO)) {
			if (pn547_dev->spi_ven_enabled == false) {
				pn547_dev->spi_ven_enabled = true;
#ifndef VEN_ALWAYS_ON
				if (pn547_dev->nfc_ven_enabled == false) {
					/* provide power to NFCC if NFC service not provided */
					gpio_set_value(pn547_dev->ven_gpio, 1);
					usleep_range(10000, 10100);
				}
#endif
			}
			svdd_sync_onoff(pn547_dev->nfc_service_pid,
				P61_STATE_SPI_SVDD_SYNC_START);
#if !defined(VEN_ALWAYS_ON) && defined(FEATURE_PN80T)
			gpio_set_value(pn547_dev->ese_pwr_req, 0);
			msleep(60);
#endif
			svdd_sync_onoff(pn547_dev->nfc_service_pid,
				P61_STATE_SPI_SVDD_SYNC_END);
#if !defined(VEN_ALWAYS_ON) && defined(FEATURE_PN80T)
			gpio_set_value(pn547_dev->ese_pwr_req, 1);
			usleep_range(10000, 10100);
#endif
		} else {
			NFC_LOG_ERR("reset failed\n");
			ret = -EBUSY; /* Device or resource busy */
		}
		break;
	case 3: /*else if (arg == 3) */
#if defined(FEATURE_SN100X)
		ret = ese_cold_reset(ESE_COLD_RESET_SOURCE_NFC);
#else
		NFC_LOG_INFO("Prio Session Start power on ese\n");
		if ((current_state & (P61_STATE_SPI|P61_STATE_SPI_PRIO|P61_STATE_DWNLD)) == 0) {
			p61_update_access_state(pn547_dev, P61_STATE_SPI_PRIO, true);
			if (current_state & P61_STATE_WIRED) {
				if (pn547_dev->nfc_service_pid) {
					NFC_LOG_INFO("nfc service pid %ld\n",
						pn547_dev->nfc_service_pid);
					/*signal_handler(P61_STATE_SPI_PRIO, pn547_dev->nfc_service_pid);*/
					dwp_onoff(pn547_dev->nfc_service_pid, P61_STATE_SPI_PRIO);
				} else {
					NFC_LOG_ERR("invalid nfc service pid.. %ld\n",
						pn547_dev->nfc_service_pid);
				}
			}
			pn547_dev->spi_ven_enabled = true;

			if (pn547_dev->nfc_ven_enabled == false) {
				/* provide power to NFCC if,	NFC service not provided */
				gpio_set_value(pn547_dev->ven_gpio, 1);
				usleep_range(10000, 10100);
			}
			/* pull the gpio to high once NFCC is power on*/
			gpio_set_value(pn547_dev->ese_pwr_req, 1);
			usleep_range(10000, 10100);
		} else {
			NFC_LOG_ERR("Prio Session Start power on ese failed 0x%x\n",
				current_state);
			ret = -EBUSY; /* Device or resource busy */
		}
#endif
		break;
	case 4: /*else if (arg == 4)*/
		if (current_state & P61_STATE_SPI_PRIO) {
			NFC_LOG_INFO("Prio Session Ending...\n");
			p61_update_access_state(pn547_dev, P61_STATE_SPI_PRIO, false);
			/* after SPI prio timeout, the state is changing from SPI prio to SPI */
			p61_update_access_state(pn547_dev, P61_STATE_SPI, true);
#if defined(FEATURE_PN80T) || defined(FEATURE_SN100X)
			if (current_state & P61_STATE_WIRED)
#endif
			{
				if (pn547_dev->nfc_service_pid) {
					NFC_LOG_INFO("nfc service pid  %ld",
						pn547_dev->nfc_service_pid);
					signal_handler(P61_STATE_SPI_PRIO_END, pn547_dev->nfc_service_pid);
				} else
					NFC_LOG_ERR("invalid nfc service pid.. %ld",
						pn547_dev->nfc_service_pid);
			}
		} else {
			NFC_LOG_ERR("Prio Session End failed 0x%x\n", current_state);
			ret = -EBADRQC; /* Device or resource busy */
		}
		break;
	case 5:
		release_ese_lock(P61_STATE_SPI);
		break;
	case 6:
		/* SPI Service called ISO-RST */
		if (current_state & P61_STATE_WIRED)
			return -EPERM; /* Operation not permitted */

		if (current_state & P61_STATE_SPI)
			p61_update_access_state(pn547_dev, P61_STATE_SPI, false);
		else if (current_state & P61_STATE_SPI_PRIO)
			p61_update_access_state(pn547_dev, P61_STATE_SPI_PRIO, false);
#ifdef ISO_RST
		gpio_set_value(pn547_dev->iso_rst_gpio, 0);
		msleep(50);
		gpio_set_value(pn547_dev->iso_rst_gpio, 1);
		msleep(50);
		NFC_LOG_INFO("ISO RESET from SPI DONE\n");
#endif
		break;
	case 7:
		set_force_reset(true);
		ret = do_reset_protection(true);
		break;
	case 8:
		set_force_reset(false);
		ret = do_reset_protection(false);
		break;
	default:
		NFC_LOG_ERR("bad ese pwr arg %lu\n", arg);
		ret = -EBADRQC; /* Invalid request code */
	}

	return ret;
}

static int pn547_p61_set_wired_access(struct pn547_dev *pdev, unsigned long arg)
{
	enum p61_access_state current_state;
	int ret = 0;

	p61_get_access_state(pn547_dev, &current_state);
	NFC_LOG_INFO("cur=0x%x\n", current_state);
	switch (arg) {
	case 0: /*else if (arg == 0)*/
		NFC_LOG_INFO("disabling\n");
		if (current_state & P61_STATE_WIRED) {
			p61_update_access_state(pn547_dev, P61_STATE_WIRED, false);
#if !defined(FEATURE_PN80T) && !defined(FEATURE_SN100X)
			if ((current_state & (P61_STATE_SPI|P61_STATE_SPI_PRIO)) == 0) {
				svdd_sync_onoff(pn547_dev->nfc_service_pid,
					P61_STATE_DWP_SVDD_SYNC_START);
				gpio_set_value(pn547_dev->ese_pwr_req, 0);
				msleep(60);
				svdd_sync_onoff(pn547_dev->nfc_service_pid,
					P61_STATE_DWP_SVDD_SYNC_END);
			}
#endif
		} else {
			NFC_LOG_ERR("failed, current_state = %x\n",
				pn547_dev->p61_current_state);
			ret = -EPERM; /* Operation not permitted */
		}
		break;
	case 1: /*	if (arg == 1)*/
		if (current_state) {
			NFC_LOG_INFO("enabling\n");
			p61_update_access_state(pn547_dev, P61_STATE_WIRED, true);
			if (current_state & P61_STATE_SPI_PRIO) {
				if (pn547_dev->nfc_service_pid) {
					NFC_LOG_INFO("nfc service pid  %ld",
						pn547_dev->nfc_service_pid);
					signal_handler(P61_STATE_SPI_PRIO, pn547_dev->nfc_service_pid);
				} else {
					NFC_LOG_INFO("invalid nfc service pid.. %ld",
						pn547_dev->nfc_service_pid);
				}
			}
#if !defined(FEATURE_PN80T) && !defined(FEATURE_SN100X)
			if ((current_state & (P61_STATE_SPI|P61_STATE_SPI_PRIO)) == 0) {
				gpio_set_value(pn547_dev->ese_pwr_req, 1);
				usleep_range(10000, 10100);
			}
#endif
		} else {
			NFC_LOG_ERR("enabling failed\n");
			ret = -EBUSY; /* Device or resource busy */
		}
		break;
	case 2: /*	else if(arg == 2)*/
		NFC_LOG_INFO("P61 ESE POWER REQ LOW\n");
#if !defined(FEATURE_PN80T) && !defined(FEATURE_SN100X)
		svdd_sync_onoff(pn547_dev->nfc_service_pid,
			P61_STATE_DWP_SVDD_SYNC_START);
		gpio_set_value(pn547_dev->ese_pwr_req, 0);
		msleep(60);
		svdd_sync_onoff(pn547_dev->nfc_service_pid, P61_STATE_DWP_SVDD_SYNC_END);
#endif
		break;
	case 3: /*	else if(arg == 3)*/
		NFC_LOG_INFO("P61 ESE POWER REQ HIGH\n");
#if !defined(FEATURE_PN80T) && !defined(FEATURE_SN100X)
		gpio_set_value(pn547_dev->ese_pwr_req, 1);
		usleep_range(10000, 10100);
#endif
		break;
	case 4: /*else if(arg == 4)*/
		release_ese_lock(P61_STATE_WIRED);
		break;
	default: /*else*/
		NFC_LOG_INFO("bad arg %lu\n", arg);
		ret = -EBADRQC; /* Invalid request code */
	}

	return ret;
}

int get_ese_lock(enum p61_access_state  p61_current_state, int timeout)
{
	unsigned long tempJ = msecs_to_jiffies(timeout);

	NFC_LOG_INFO("enter p61_current_state=0x%x, timeout=%d, jiffies=%lu\n",
		p61_current_state, timeout, tempJ);
	reinit_completion(&pn547_dev->ese_comp);

	if (p61_trans_acc_on) {
		if (!wait_for_completion_timeout(&pn547_dev->ese_comp, tempJ)) {
			NFC_LOG_ERR("timeout p61_current_state = %d\n", p61_current_state);
			return -EBUSY;
		}
	}

	p61_trans_acc_on = 1;
	NFC_LOG_INFO("exit p61_trans_acc_on =%d, timeout = %d\n",
		p61_trans_acc_on, timeout);
	return 0;
}
EXPORT_SYMBOL(get_ese_lock);

static void release_ese_lock(enum p61_access_state  p61_current_state)
{
	NFC_LOG_INFO("enter p61_current_state = (0x%x)\n",
			p61_current_state);
	p61_trans_acc_on = 0;
	complete(&pn547_dev->ese_comp);
	NFC_LOG_INFO("p61_trans_acc_on =%d exit\n", p61_trans_acc_on);
}

#if defined(FEATURE_PN80T) || defined(FEATURE_SN100X)
static int set_jcop_download_state(unsigned long arg)
{
	enum p61_access_state current_state = P61_STATE_INVALID;
	int ret = 0;

	p61_get_access_state(pn547_dev, &current_state);
	NFC_LOG_INFO("PN547_SET_DWNLD_STATUS:JCOP Dwnld state arg = %ld\n", arg);
	if (arg == JCP_DWNLD_INIT) {
		if (pn547_dev->nfc_service_pid) {
			NFC_LOG_INFO("nfc service pid ---- %ld\n",
				pn547_dev->nfc_service_pid);
			signal_handler(JCP_DWNLD_INIT, pn547_dev->nfc_service_pid);
		} else {
			if (current_state & P61_STATE_JCP_DWNLD)
				ret = -EINVAL;
			else
				p61_update_access_state(pn547_dev,
					P61_STATE_JCP_DWNLD, true);
		}
	} else if (arg == JCP_DWNLD_START) {
		if (current_state & P61_STATE_JCP_DWNLD)
			ret = -EINVAL;
		else
			p61_update_access_state(pn547_dev, P61_STATE_JCP_DWNLD, true);
	} else if (arg == JCP_SPI_DWNLD_COMPLETE) {
		if (pn547_dev->nfc_service_pid) {
			signal_handler(JCP_DWP_DWNLD_COMPLETE,
				pn547_dev->nfc_service_pid);
		}
		p61_update_access_state(pn547_dev, P61_STATE_JCP_DWNLD, false);
	} else if (arg == JCP_DWP_DWNLD_COMPLETE) {
		p61_update_access_state(pn547_dev, P61_STATE_JCP_DWNLD, false);
	} else {
		NFC_LOG_ERR("bad jcop download arg %lu\n", arg);
		return -EBADRQC; /* Invalid request code */
	}
	NFC_LOG_INFO("PN547_SET_DWNLD_STATUS = %x", current_state);

	return ret;
}
#endif
#endif

long pn547_dev_ioctl(struct file *filp,
			   unsigned int cmd, unsigned long arg)
{
	/*struct pn547_dev *pn547_dev = filp->private_data;*/
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	enum p61_access_state current_state;
	int ret = 0;

	/* Free pass autobahn area, not protected. Use it carefullly. START */
	switch (cmd) {
	case P547_GET_ESE_ACCESS:
		return (long)get_ese_lock(P61_STATE_WIRED, arg);
		/*break;*/
	case P547_REL_SVDD_WAIT:
		return (long)release_svdd_wait();
		/*break;*/
	case P547_SET_NFC_SERVICE_PID:
		return (long)set_nfc_pid(arg);
		/*break;*/
	case P547_REL_DWPONOFF_WAIT:
		return (long)release_dwp_onoff();
		/*break;*/
	default:
		break;
	}
	/* Free pass autobahn area, not protected. Use it carefullly. END */
	p61_access_lock(pn547_dev);
	switch (cmd) {
	case PN547_SET_PWR:
		ret = pn547_set_pwr(pn547_dev, arg);
		break;

	case P61_SET_SPI_PWR:
		ret = pn547_p61_set_spi_pwr(pn547_dev, arg);
		break;

	case P61_GET_PWR_STATUS:
		current_state = P61_STATE_INVALID;
		p61_get_access_state(pn547_dev, &current_state);
		NFC_LOG_INFO("P61_GET_PWR_STATUS  = %x\n", current_state);
		put_user(current_state, (int __user *)arg);
		break;

#if defined(FEATURE_PN80T) || defined(FEATURE_SN100X)
	case PN547_SET_DWNLD_STATUS:
		ret = set_jcop_download_state(arg);
		if (ret < 0)
			NFC_LOG_INFO("set_jcop_download_state failed");
		break;
#endif

	case P61_SET_WIRED_ACCESS:
		ret = pn547_p61_set_wired_access(pn547_dev, arg);
		break;
	case PN547_GET_IRQ_STATE:
		ret = gpio_get_value(pn547_dev->irq_gpio);
		break;

	default:
		NFC_LOG_ERR("bad ioctl cmd:%x\n", cmd);
		ret = -EINVAL;
	}
	p61_access_unlock(pn547_dev);
	return (long)ret;
#else
	switch (cmd) {
	case PN547_SET_PWR:
		if (arg == 2) {
			/* power on with firmware download (requires hw reset)
			 */
			set_pd(pn547_dev, PN547_NFC_PW_ON);
			gpio_set_value(pn547_dev->firm_gpio, 1);
			usleep_range(4900, 5000);
			set_pd(pn547_dev, PN547_NFC_PW_OFF);
			usleep_range(4900, 5000);
			set_pd(pn547_dev, PN547_NFC_PW_ON);
			usleep_range(4900, 5000);
			pn547_enable_irq(pn547_dev);
			NFC_LOG_INFO("power on with firmware, irq=%d\n",
				atomic_read(&pn547_dev->irq_enabled));
		} else if (arg == 1) {
			/* power on */
			gpio_set_value(pn547_dev->firm_gpio, 0);
			set_pd(pn547_dev, PN547_NFC_PW_ON);
			usleep_range(4900, 5000);
			pn547_enable_irq(pn547_dev);
			NFC_LOG_INFO("power on, irq=%d\n", atomic_read(&pn547_dev->irq_enabled));
		} else if (arg == 0) {
			/* power off */
			pn547_disable_irq(pn547_dev);
			if (wake_lock_active(&pn547_dev->nfc_clk_wake_lock))
				wake_unlock(&pn547_dev->nfc_clk_wake_lock);
			NFC_LOG_INFO("power off, irq=%d\n", atomic_read(&pn547_dev->irq_enabled));
			gpio_set_value(pn547_dev->firm_gpio, 0);
			set_pd(pn547_dev, PN547_NFC_PW_OFF);
			usleep_range(4900, 5000);
		} else if (arg == 3) {
			NFC_LOG_INFO("Read Cancel\n");
			pn547_dev->cancel_read = true;
			atomic_set(&pn547_dev->read_flag, 1);
			wake_up(&pn547_dev->read_wq);
		} else {
			NFC_LOG_ERR("bad arg %lu\n", arg);
			return -EINVAL;
		}
		break;
	default:
		NFC_LOG_ERR("bad ioctl %u\n", cmd);
		return -EINVAL;
	}
	return 0;
#endif
}
EXPORT_SYMBOL(pn547_dev_ioctl);

static const struct file_operations pn547_dev_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.read = pn547_dev_read,
	.write = pn547_dev_write,
	.open = pn547_dev_open,
	.flush = pn547_dev_flush,
	.release = pn547_dev_release,
	.unlocked_ioctl = pn547_dev_ioctl,
};

static int pn547_parse_dt(struct device *dev,
	struct pn547_dev *pdev)
{
	struct device_node *np = dev->of_node;

	pdev->irq_gpio = of_get_named_gpio(np, "pn547,irq-gpio", 0);
	pdev->ven_gpio = of_get_named_gpio(np, "pn547,ven-gpio", 0);
	pdev->firm_gpio = of_get_named_gpio(np, "pn547,firm-gpio", 0);

#ifdef ISO_RST
	pdev->iso_rst_gpio = of_get_named_gpio(np, "pn547,iso-rst-gpio", 0);
#endif
#if defined(CONFIG_NFC_PN547_ESE_SUPPORT) && defined(FEATURE_PN80T)
	pdev->ese_pwr_req = of_get_named_gpio(np, "pn547,pwr_req", 0);
#endif

	if (of_get_property(dev->of_node, "pn547,ldo_control", NULL)) {
		if (of_property_read_string(np, "pn547,nfc_pvdd", &pdev->nfc_pvdd) < 0) {
			NFC_LOG_ERR("parse_dt() get nfc_pvdd error\n");
			pdev->nfc_pvdd = NULL;
		}
	} else {
		pdev->pvdd = of_get_named_gpio(np, "pn547,pvdd-gpio", 0);
		if (pdev->pvdd < 0) {
			NFC_LOG_ERR("pvdd-gpio is not set.");
			pdev->pvdd = 0;
		}
	}

	if (of_get_property(dev->of_node, "pn547,nfc_pm_clk", NULL)) {
		pdev->clk = clk_get(dev, "rf_clk");
		if (IS_ERR(pdev->clk)) {
			NFC_LOG_ERR("Couldn't get rf_clk\n");
		} else {
			NFC_LOG_INFO("got rf_clk\n");
			/* sdm845: if prepare_enable clk, clk always generated*/
			/*clk_prepare_enable(pn547_dev->clk);*/
		}
	}

	if (of_property_read_u32(np, "clkctrl-reg", (u32 *)&pdev->clkctrl_addr))
		NFC_LOG_ERR("%s: no clkctrl-reg at dt file\n", __func__);

	if (of_property_read_u32(np, "clkctrl-pmu", (u32 *)&pdev->pmu_debug1)) {
		NFC_LOG_ERR("%s: no clkctrl-pmu at dt file\n", __func__);
	} else {
#ifdef FEATURE_USE_PMU_DEBUG1_IOREMAP
		pdev->pmu_debug1_ctrl = ioremap_nocache(pdev->pmu_debug1, 0x4);
		if (!pdev->pmu_debug1_ctrl) {
			dev_err(dev, "cannot remap PMU DEBUG1\n");

			return -ENXIO;
		}
#endif
	}
	if (of_find_property(np, "pn547,nfc_ap_clk", NULL))
		pdev->clk_req_gpio = of_get_named_gpio(np, "pn547,clk_req-gpio", 0);

	if (of_find_property(np, "pn547,nfc_clkreq_int", NULL))
		pdev->clk_req_gpio = of_get_named_gpio(np, "pn547,nfc_clkreq_int", 0);

	NFC_LOG_INFO("irq : %d, ven : %d, firm : %d\n", pdev->irq_gpio,
		pdev->ven_gpio, pdev->firm_gpio);

	return 0;
}

static int pn547_regulator_onoff(struct device *dev,
		struct pn547_dev *pdev, int onoff)
{
	int rc = 0;
	struct regulator *regulator_nfc_pvdd;

	regulator_nfc_pvdd = regulator_get(NULL, pdev->nfc_pvdd);
	if (IS_ERR(regulator_nfc_pvdd) || regulator_nfc_pvdd == NULL) {
		NFC_LOG_ERR("nfc_pvdd regulator_get fail\n");
		return -ENODEV;
	}

	NFC_LOG_INFO("regulator onoff = %d\n", onoff);
	if (onoff == NFC_I2C_LDO_ON) {
#ifdef FEATURE_SN100X
		rc = regulator_set_load(regulator_nfc_pvdd, 300000);
		if (rc) {
			NFC_LOG_ERR("regulator_uwb_vdd set_load failed, rc=%d\n", rc);
			goto done;
		}
#endif
		rc = regulator_enable(regulator_nfc_pvdd);
		if (rc) {
			NFC_LOG_ERR("regulator enable failed, rc=%d\n", rc);
			goto done;
		}
	} else {
		rc = regulator_disable(regulator_nfc_pvdd);
		if (rc) {
			NFC_LOG_ERR("regulator disable failed, rc=%d\n", rc);
			goto done;
		}
	}

	NFC_LOG_INFO("success\n");
done:
	regulator_put(regulator_nfc_pvdd);
	return rc;
}

#ifdef FEATURE_ESE_TEST
int test_get_ese_pwr(void)
{
	return pn547_dev->ese_pwr_req;
}
#endif
#ifdef FEATURE_NFC_TEST
static ssize_t test_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	int size;
	int ret = 0;
	int count = 4;
	char tmp[128] = {0x20, 0x00, 0x01, 0x00, };
	int retry;
	bool old_ven, old_irq;
	int old_read_value;

	NFC_LOG_INFO("start\n");

	old_ven = pn547_dev->nfc_ven_enabled;
	NFC_LOG_INFO("old_ven is %d\n", old_ven);
	retry = 20;
	if (!old_ven) {	/* if nfc status is off */
		pn547_dev->nfc_ven_enabled = true;

		if (pn547_dev->spi_ven_enabled == false)
			set_pd(pn547_dev, PN547_NFC_PW_ON);
		usleep_range(4900, 5000);
	} else {	/* if nfc status is on */
		/*wake up device*/
		set_pd(pn547_dev, PN547_NFC_PW_OFF);
		usleep_range(14900, 15000);
		set_pd(pn547_dev, PN547_NFC_PW_ON);
		usleep_range(4900, 5000);
		//intercept i2c_master_recv
		pn547_dev->cancel_read = 1;
		atomic_set(&pn547_dev->read_flag, 1);
	}
	wake_up_all(&pn547_dev->read_wq);
	while (!mutex_trylock(&pn547_dev->read_mutex) && --retry)
		usleep_range(15, 20);

	if (!retry) {
		NFC_LOG_ERR("mutex_trylock failed. check pn547_dev_read()\n");
		ret = snprintf(buf, SZ_64, "test failed : device in use\n");
		goto fail_lock;
	}

	NFC_LOG_INFO("read_mutex locked. retry : %d\n", retry);

	atomic_set(&pn547_dev->read_flag, 0);
	pn547_dev->cancel_read = 0;
	old_irq = atomic_read(&pn547_dev->irq_enabled);
	NFC_LOG_INFO("old_irq is %d\n", old_irq);
	if (!old_irq) {
		atomic_set(&pn547_dev->irq_enabled, 1);
		enable_irq(pn547_dev->client->irq);
		enable_irq(pn547_dev->clk_req_irq);
		enable_irq_wake(pn547_dev->client->irq);
		enable_irq_wake(pn547_dev->clk_req_irq);
		NFC_LOG_INFO("power on, irq=%d\n", atomic_read(&pn547_dev->irq_enabled));
	}
	retry = 2;
	do {
		ret = i2c_master_send(pn547_dev->client, tmp, count);
		if (count == ret)
			break;

		NFC_LOG_INFO("i2c_master_send error. ret:%d, retry:%d\n", ret, retry);
		usleep_range(6000, 10000); /* Retry, chip was in standby */
	} while (retry--);

	if (ret != count) {
		NFC_LOG_ERR("failed. count error. send=%d, recv=%d\n", count, ret);
		ret = 0;
		goto fail;
	}

	NFC_LOG_INFO("send success. returned: %d\n", ret);

	//wait for reply
#if NFC_DEBUG
	NFC_LOG_INFO("wait_event_interruptible : in\n");
#endif
	ret = 0;
	old_read_value = atomic_read(&pn547_dev->read_flag);
	NFC_LOG_INFO("read_flag %d, cancel_read %d", old_read_value,
		pn547_dev->cancel_read);
	if (!old_read_value) {
		ret = wait_event_interruptible(pn547_dev->read_wq,
			atomic_read(&pn547_dev->read_flag));
	}

#if NFC_DEBUG
	NFC_LOG_DBG("h\n");
#endif

	if (pn547_dev->cancel_read) {
		pn547_dev->cancel_read = false;
		ret = 0;
		//todo : old_ven and old_irq rollback needed
		goto fail;
	}

	if (ret) {
		ret = 0;
		goto fail;
	}

	/* Read data */
	count = 6;
	ret = i2c_master_recv(pn547_dev->client, tmp, count);
#if NFC_DEBUG
	NFC_LOG_INFO("i2c_master_recv\n");
#endif
	mutex_unlock(&pn547_dev->read_mutex);

	if (!old_ven) {	/* if nfc status is off */
		if (pn547_dev->spi_ven_enabled == false)
			set_pd(pn547_dev, PN547_NFC_PW_OFF);
		usleep_range(14900, 15000);
		pn547_dev->nfc_ven_enabled = false;
	}
	if (!old_irq) {
		atomic_set(&pn547_dev->irq_enabled, 0);
		disable_irq_wake(pn547_dev->client->irq);
		disable_irq_wake(pn547_dev->clk_req_irq);
		disable_irq_nosync(pn547_dev->clk_req_irq);
		disable_irq_nosync(pn547_dev->client->irq);
	}
	if (ret < 0 || ret > count) {
		NFC_LOG_ERR("i2c_master_recv returned %d. count : %d\n",
			ret, count);
		return 0;
	}

	size = snprintf(buf, SZ_64, "test completed!! size: %d, data: %X %X %X %X %X %X\n",
		ret, tmp[0], tmp[1], tmp[2], tmp[3], tmp[4], tmp[5]);
	NFC_LOG_INFO("recv success.\n");
	usleep_range(10000, 10100);

	return size;

fail:
	mutex_unlock(&pn547_dev->read_mutex);
fail_lock:
	return ret;
}

static ssize_t test_store(struct class *class,
	struct class_attribute *attr, const char *buf, size_t size)
{
	return size;
}

static CLASS_ATTR_RW(test);
#endif

static ssize_t nfc_support_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	NFC_LOG_INFO("\n");
	return 0;
}

static CLASS_ATTR_RO(nfc_support);

static int pn547_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret;
	int err;
	int addr;
	char tmp[4] = {0x20, 0x00, 0x01, 0x01};
	int addrcnt;
	struct device_node *np = client->dev.of_node;
	int nfc_support = 0;
	struct property *prop;
#ifdef FEATURE_NFC_TEST
	struct class *nfc_class;
#endif
	nfc_logger_init();

	/*separate NFC / non NFC using GPIO*/
	prop = of_find_property(np, "pn547,check_nfc", NULL);
	if (prop) {
		nfc_support = gpio_get_value(of_get_named_gpio(np, "pn547,check_nfc", 0));
		if (nfc_support > 0) {
			NFC_LOG_INFO("%s : nfc support model : %d\n", __func__, nfc_support);
		} else {
			NFC_LOG_INFO("%s : nfc not support model : %d\n", __func__, nfc_support);
			return -ENXIO;
		}
	}

	NFC_LOG_INFO("entered\n");

	if (client->dev.of_node) {
		pn547_dev = devm_kzalloc(&client->dev, sizeof(struct pn547_dev), GFP_KERNEL);
		if (!pn547_dev)
			return -ENOMEM;

		err = pn547_parse_dt(&client->dev, pn547_dev);
		if (err)
			return err;
	} else {
		NFC_LOG_ERR("no dts\n");
		return -ENODEV;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		NFC_LOG_ERR("need I2C_FUNC_I2C\n");
		return -ENODEV;
	}

	ret = gpio_request(pn547_dev->irq_gpio, "nfc_irq");
	if (ret)
		return -ENODEV;
	gpio_direction_input(pn547_dev->irq_gpio);

	ret = gpio_request(pn547_dev->ven_gpio, "nfc_ven");
	if (ret)
		goto err_ven;
	gpio_direction_output(pn547_dev->ven_gpio, 0);
	usleep_range(14900, 15000);

	ret = gpio_request(pn547_dev->firm_gpio, "nfc_firm");
	if (ret)
		goto err_firm;
	gpio_direction_output(pn547_dev->firm_gpio, 0);

#if defined(CONFIG_NFC_PN547_ESE_SUPPORT) && defined(FEATURE_PN80T)
	ret = gpio_request(pn547_dev->ese_pwr_req, "ese_pwr");
	if (ret)
		goto err_ese;
	gpio_direction_output(pn547_dev->ese_pwr_req, 0);
#endif
#ifdef ISO_RST
	ret = gpio_request(pn547_dev->iso_rst_gpio, "iso_rst");
	if (ret)
		goto err_iso_rst;
#endif

	if (pn547_dev->clkctrl_addr != 0) {
		unsigned int val = 0;

		pn547_dev->clkctrl = ioremap_nocache(pn547_dev->clkctrl_addr, 0x4);
		if (!pn547_dev->clkctrl) {
			dev_err(&client->dev, "cannot remap register\n");
			ret = -ENXIO;
			goto err_ioremap;
		}
		val = readl(pn547_dev->clkctrl);
		val |= (PN547_NFC_CLKCTRL_REQ_POLA | PN547_NFC_CLKCTRL_CLK_ENABLE);
		writel(val, pn547_dev->clkctrl);
		NFC_LOG_INFO("%s: clkctrl=0x%X\n", __func__, val);
	}

	if (of_get_property(np, "pn547,ldo_control", NULL)) {
		ret = pn547_regulator_onoff(&client->dev, pn547_dev, NFC_I2C_LDO_ON);
		if (ret < 0)
			NFC_LOG_ERR("%s pn547 regulator_on fail err = %d\n", __func__, ret);
#ifdef FEATURE_SN100X
	usleep_range(4500, 4600); /* spec : VDDIO high -> 4.5 ms -> VEN high*/
#else
		usleep_range(1000, 1100);
#endif
	} else {
		ret = of_get_named_gpio(client->dev.of_node, "pn547,pvdd-gpio", 0);
		if (ret < 0) {
			NFC_LOG_ERR("%s : pvdd-gpio is not set", __func__);
		} else {
			pn547_dev->pvdd = ret;

			ret = gpio_request(pn547_dev->pvdd, "pvdd-gpio");
			if (ret) {
				dev_err(&client->dev, "%s failed to get gpio pvdd-gpio\n", __func__);
				gpio_free(pn547_dev->pvdd);
				goto err_pvdd;
			}
			gpio_direction_output(pn547_dev->pvdd, 1);
			NFC_LOG_INFO("%s pvdd-gpio:%d", __func__, pn547_dev->pvdd);
		}
	}

	client->irq = gpio_to_irq(pn547_dev->irq_gpio);
	NFC_LOG_INFO("IRQ num %d\n", client->irq);

#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	pn547_dev->p61_current_state = P61_STATE_IDLE;
	pn547_dev->nfc_ven_enabled = false;
	pn547_dev->spi_ven_enabled = false;
#endif
	pn547_dev->client = client;
#ifdef ISO_RST
	pn547_dev->iso_rst_gpio = false;
#endif
#ifdef FEATURE_SN100X
	pn547_dev->state_flags = 0x00;
	ese_reset_resource_init();
#endif
	/* init mutex and queues */
	init_waitqueue_head(&pn547_dev->read_wq);
	mutex_init(&pn547_dev->read_mutex);
	mutex_init(&pn547_dev->dev_ref_mutex);
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	p61_trans_acc_on = 0;
	init_completion(&pn547_dev->ese_comp);
	init_completion(&pn547_dev->svdd_sync_comp);
	init_completion(&pn547_dev->dwp_onoff_comp);
	sema_init(&dwp_onoff_release_sema, 0);
	mutex_init(&pn547_dev->p61_state_mutex);
#endif

	pn547_dev->pn547_device.minor = MISC_DYNAMIC_MINOR;
	pn547_dev->pn547_device.name = "pn547";
	pn547_dev->pn547_device.fops = &pn547_dev_fops;

	ret = misc_register(&pn547_dev->pn547_device);
	if (ret) {
		NFC_LOG_ERR("misc_register failed\n");
		goto err_misc_register;
	}

	/* request irq.  the irq is set whenever the chip has data available
	 * for reading.  it is cleared when all data has been read.
	 */
	NFC_LOG_INFO("requesting IRQ %d\n", client->irq);
	gpio_direction_input(pn547_dev->irq_gpio);
	set_pd(pn547_dev, PN547_NFC_PW_OFF);
	gpio_direction_output(pn547_dev->firm_gpio, 0);
#if defined(CONFIG_NFC_PN547_ESE_SUPPORT) && defined(FEATURE_PN80T)
	gpio_direction_output(pn547_dev->ese_pwr_req, 0);
#endif
#ifdef ISO_RST
	gpio_direction_output(pn547_dev->iso_rst_gpio, 0);
#endif
	i2c_set_clientdata(client, pn547_dev);
	wake_lock_init(&pn547_dev->nfc_wake_lock, WAKE_LOCK_SUSPEND, "nfc_wake_lock");
	wake_lock_init(&pn547_dev->nfc_clk_wake_lock, WAKE_LOCK_SUSPEND, "nfc_clk_wake_lock");
	spin_lock_init(&pn547_dev->clk_ctrl_slock);

#ifdef FEATURE_DELAYED_CLOCK_DISABLE
	INIT_DELAYED_WORK(&pn547_dev->clk_ctrl_work, pn547_clk_ctrl_work);
#endif
	ret = request_irq(client->irq, pn547_dev_irq_handler, IRQF_TRIGGER_HIGH,
		"pn547", pn547_dev);
	if (ret) {
		NFC_LOG_ERR("request_irq failed\n");
		goto err_request_irq_failed;
	}
	disable_irq_nosync(pn547_dev->client->irq);
	atomic_set(&pn547_dev->irq_enabled, 0);

	if (of_get_property(np, "pn547,nfc_ap_clk", NULL)) {
		gpio_direction_input(pn547_dev->clk_req_gpio);
		pn547_dev->clk_req_irq = gpio_to_irq(pn547_dev->clk_req_gpio);
		ret = request_threaded_irq(pn547_dev->clk_req_irq, pn547_nfc_clk_irq_isr, pn547_nfc_clk_irq,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "nfc_clk", pn547_dev);
		if (ret < 0) {
			dev_err(&client->dev, "failed to register IRQ handler\n");
			goto err_request_irq_failed;
		} else
			enable_irq_wake(pn547_dev->clk_req_irq);

		pn547_dev->nfc_clock = clk_get(&client->dev, "oscclk_nfc");
		if (IS_ERR(pn547_dev->nfc_clock)) {
			NFC_LOG_ERR("probe() clk not found\n");
			goto err_clk_get_failed;
		}

	}

	set_pd(pn547_dev, PN547_NFC_PW_ON);
	gpio_set_value(pn547_dev->firm_gpio, 1); /* add firmware pin */
	usleep_range(4900, 5000);
	set_pd(pn547_dev, PN547_NFC_PW_OFF);
	usleep_range(14900, 15000);
	set_pd(pn547_dev, PN547_NFC_PW_ON);
	usleep_range(4900, 5000);

	for (addr = 0x2B; addr > 0x27; addr--) {
		client->addr = addr;
		addrcnt = 2;
		do {
			ret = i2c_master_send(client, tmp, 4);
			if (ret > 0) {
				NFC_LOG_INFO("i2c addr(0x%X), ret(%d)\n",
					client->addr, ret);
				pn547_dev->i2c_probe = ret;
				break;
			}
		} while (addrcnt--);
		if (ret > 0)
			break;
	}

	if (ret <= 0) {
		NFC_LOG_INFO("ret(%d), i2c_probe(%d)\n", ret, pn547_dev->i2c_probe);
		client->addr = 0x2B;
	}
	set_pd(pn547_dev, PN547_NFC_PW_OFF);
	gpio_set_value(pn547_dev->firm_gpio, 0); /* add */

#ifdef VEN_ALWAYS_ON
	usleep_range(14900, 15000);
	gpio_set_value(pn547_dev->ven_gpio, 1);
	atomic_set(&pn547_dev->irq_enabled, 1);
	enable_irq(client->irq);
	enable_irq_wake(client->irq);
#endif

	if (ret < 0)
		NFC_LOG_ERR("fail to get i2c addr\n");
	else
		NFC_LOG_INFO("success, i2c_probe(%d)\n", pn547_dev->i2c_probe);

#ifdef FEATURE_NFC_TEST
	nfc_test_class = class_create(THIS_MODULE, "nfc_test");
	if (IS_ERR(&nfc_test_class)) {
		NFC_LOG_ERR("failed to create nfc class\n");
	} else {
		ret = class_create_file(nfc_test_class, &class_attr_test);
		if (ret)
			NFC_LOG_ERR("failed to create file\n");
	}
#endif

	nfc_class = class_create(THIS_MODULE, "nfc");
	if (IS_ERR(&nfc_class)) {
		NFC_LOG_ERR("failed to create nfc class\n");
	} else {
		ret = class_create_file(nfc_class, &class_attr_nfc_support);
		if (ret)
			NFC_LOG_ERR("failed to create nfc_support file\n");
	}

	pn547_dev->r_buf = kzalloc(sizeof(char) * MAX_BUFFER_SIZE, GFP_KERNEL);
	if (pn547_dev->r_buf == NULL) {
		NFC_LOG_ERR("failed to allocate for i2c r_buffer\n");
		ret = -ENOMEM;
		goto err_r_buf_alloc_failed;
	}

	pn547_dev->w_buf = kzalloc(sizeof(char) * MAX_BUFFER_SIZE, GFP_KERNEL);
	if (pn547_dev->w_buf == NULL) {
		NFC_LOG_ERR("failed to allocate for i2c w_buffer\n");
		ret = -ENOMEM;
		goto err_w_buf_alloc_failed;
	}

	return 0;

err_w_buf_alloc_failed:
	kfree(pn547_dev->r_buf);
err_r_buf_alloc_failed:
err_clk_get_failed:
	if (of_get_property(np, "pn547,nfc_ap_clk", NULL))
		clk_put(pn547_dev->nfc_clock);
err_request_irq_failed:
	misc_deregister(&pn547_dev->pn547_device);
	wake_lock_destroy(&pn547_dev->nfc_wake_lock);
err_misc_register:
#ifdef FEATURE_SN100X
	ese_reset_resource_destroy();
#endif
	mutex_destroy(&pn547_dev->dev_ref_mutex);
	mutex_destroy(&pn547_dev->read_mutex);
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	mutex_destroy(&pn547_dev->p61_state_mutex);
#endif
err_pvdd:
	if (!pn547_dev->clkctrl)
		iounmap(pn547_dev->clkctrl);
err_ioremap:
#if defined(CONFIG_NFC_PN547_ESE_SUPPORT) && defined(FEATURE_PN80T)
	gpio_free(pn547_dev->ese_pwr_req);
err_ese:
#endif
	gpio_free(pn547_dev->firm_gpio);
err_firm:
	gpio_free(pn547_dev->ven_gpio);
err_ven:
	gpio_free(pn547_dev->irq_gpio);
#ifdef ISO_RST
err_iso_rst:
	gpio_free(pn547_dev->iso_rst_gpio);
#endif
	devm_kfree(&client->dev, pn547_dev);
	pn547_dev = NULL;
	NFC_LOG_ERR("failed!\n");
	return ret;
}

static int pn547_remove(struct i2c_client *client)
{
	struct pn547_dev *pn547_dev;

	NFC_LOG_INFO("removing pn547 driver\n");
	pn547_dev = i2c_get_clientdata(client);

	wake_lock_destroy(&pn547_dev->nfc_wake_lock);
	free_irq(client->irq, pn547_dev);
	misc_deregister(&pn547_dev->pn547_device);
	mutex_destroy(&pn547_dev->dev_ref_mutex);
	mutex_destroy(&pn547_dev->read_mutex);
	gpio_free(pn547_dev->irq_gpio);
	gpio_free(pn547_dev->ven_gpio);
	gpio_free(pn547_dev->firm_gpio);
#ifdef ISO_RST
	gpio_free(pn547_dev->iso_rst_gpio);
#endif
#ifdef CONFIG_NFC_PN547_ESE_SUPPORT
	pn547_dev->p61_current_state = P61_STATE_INVALID;
	pn547_dev->nfc_ven_enabled = false;
	pn547_dev->spi_ven_enabled = false;
	mutex_destroy(&pn547_dev->p61_state_mutex);
#endif
#ifdef FEATURE_SN100X
	ese_reset_resource_destroy();
#endif

	kfree(pn547_dev->r_buf);
	kfree(pn547_dev->w_buf);
	kfree(pn547_dev);
	return 0;
}

#ifdef CONFIG_NFC_FEATURE_SN100U
static void pn547_shutdown(struct i2c_client *client)
{
	if (pn547_dev) {
		/* ese spi pin configuration should be set to out/pd/low before VDDIO level becomes low to avoid floating */
		if (pn547_dev->ese_shutdown) {
			pn547_dev->ese_shutdown();
			usleep_range(1000, 1100);
		}
	}
}
#endif
#ifdef CONFIG_PM
static int pn547_suspend(struct device *dev)
{
	NFC_LOG_INFO("suspend!\n");

	return 0;
}

static int pn547_resume(struct device *dev)
{
	struct pn547_dev *pn547_dev = i2c_get_clientdata(to_i2c_client(dev));
	int req_pin = gpio_get_value(pn547_dev->clk_req_gpio);
	unsigned long flag;

#ifdef FEATURE_SN100X
	NFC_LOG_INFO("resume! %d, %d\n", req_pin, pn547_dev->state_flags);
	if (!req_pin && ((pn547_dev->state_flags & (PN547_STATE_NFC_VEN_RESET
			| PN547_STATE_NFC_ON | PN547_STATE_FW_DNLD)) == 0)) {
#else
	NFC_LOG_INFO("resume! %d, %d\n", req_pin, pn547_dev->nfc_ven_enabled);
	if (!req_pin && !pn547_dev->nfc_ven_enabled) {
#endif
		if (!pn547_dev->pmu_debug1) {
			spin_lock_irqsave(&pn547_dev->clk_ctrl_slock, flag);
			clk_prepare_enable(pn547_dev->nfc_clock);
			clk_disable_unprepare(pn547_dev->nfc_clock);
			spin_unlock_irqrestore(&pn547_dev->clk_ctrl_slock, flag);
		} else {
			spin_lock_irqsave(&pn547_dev->clk_ctrl_slock, flag);
			pn547_pmu_update(pn547_dev, 1, 1);
			spin_unlock_irqrestore(&pn547_dev->clk_ctrl_slock, flag);
		}
	}

	return 0;
}

static const struct dev_pm_ops pn547_pm_ops = {
	.suspend = pn547_suspend,
	.resume = pn547_resume
};
#endif
static const struct i2c_device_id pn547_id[] = {
	{"pn547", 0},
	{}
};

static const struct of_device_id nfc_match_table[] = {
	{ .compatible = "pn547",},
	{},
};

static struct i2c_driver pn547_driver = {
	.id_table = pn547_id,
	.probe = pn547_probe,
	.remove = pn547_remove,
#ifdef CONFIG_NFC_FEATURE_SN100U
	.shutdown = pn547_shutdown,
#endif
	.driver = {
		.owner = THIS_MODULE,
		.name = "pn547",
#ifdef CONFIG_PM
		.pm = &pn547_pm_ops,
#endif
		.of_match_table = nfc_match_table,
		.suppress_bind_attrs = true,
	},
};

/*
 * module load/unload record keeping
 */
static int __init pn547_dev_init(void)
{
	NFC_LOG_INFO("Loading pn547 driver\n");
	if (lpcharge) {
		NFC_LOG_ERR("LPM, Do not load nfc driver\n");
		return 0;
	} else
		return i2c_add_driver(&pn547_driver);
}

module_init(pn547_dev_init);

static void __exit pn547_dev_exit(void)
{
	NFC_LOG_INFO("Unloading pn547 driver\n");
	i2c_del_driver(&pn547_driver);
}

module_exit(pn547_dev_exit);

MODULE_AUTHOR("Sylvain Fonteneau");
MODULE_DESCRIPTION("NFC PN547 driver");
MODULE_LICENSE("GPL");
