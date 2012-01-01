/**
 *   @ingroup hal
 *   @file
 *
 *   Adeos-based Real-Time Abstraction Layer for the NIOS2
 *   architecture.
 *
 *   Copyright (C) 2009 Philippe Gerum.
 *
 *   Xenomai is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License as
 *   published by the Free Software Foundation, Inc., 675 Mass Ave,
 *   Cambridge MA 02139, USA; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   Xenomai is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *   02111-1307, USA.
 *
 *   NIOS2-specific HAL services.
 */
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/ipipe_tickdev.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/xenomai/hal.h>

/*
 * We have a dedicated high resolution timer defined by our design
 * (na_hrtimer), which the interrupt pipeline core initialized at boot
 * up. Therefore, there is not much left to do here.
 */
int rthal_timer_request(void (*tick_handler) (void), int cpu)
{
	int ret;

	ret = ipipe_request_irq(&rthal_archdata.domain,
				RTHAL_TIMER_IRQ,
				(ipipe_irq_handler_t)tick_handler,
				NULL, NULL);
	if (ret)
		return ret;

	ipipe_enable_irq(RTHAL_TIMER_IRQ);

	return 0;
}

void rthal_timer_release(int cpu)
{
	ipipe_disable_irq(RTHAL_TIMER_IRQ);
	ipipe_free_irq(&rthal_archdata.domain, RTHAL_TIMER_IRQ);
}

unsigned long rthal_timer_calibrate(void)
{
	unsigned long flags;
	u64 t, v;
	int n;

	flags = hard_local_irq_save();

	ipipe_read_tsc(t);

	barrier();

	for (n = 1; n <= 100; n++)
		ipipe_read_tsc(v);

	hard_local_irq_restore(flags);

	return rthal_ulldiv(v - t, n, NULL);
}

int rthal_arch_init(void)
{
	if (rthal_timerfreq_arg == 0)
		rthal_timerfreq_arg = (unsigned long)rthal_get_timerfreq();

	if (rthal_clockfreq_arg == 0)
		rthal_clockfreq_arg = (unsigned long)rthal_get_clockfreq();

	return 0;
}

void rthal_arch_cleanup(void)
{
	printk(KERN_INFO "Xenomai: hal/nios2 stopped.\n");
}

EXPORT_SYMBOL_GPL(rthal_arch_init);
EXPORT_SYMBOL_GPL(rthal_arch_cleanup);
EXPORT_SYMBOL_GPL(rthal_thread_switch);
EXPORT_SYMBOL_GPL(rthal_thread_trampoline);
