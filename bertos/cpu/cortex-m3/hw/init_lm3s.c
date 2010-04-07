/**
 * \file
 * <!--
 * This file is part of BeRTOS.
 *
 * Bertos is free software; you can redistribute it and/or modify
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
 * As a special exception, you may use this file as part of a free software
 * library without restriction.  Specifically, if other files instantiate
 * templates or use macros or inline functions from this file, or you compile
 * this file and link it with other files to produce an executable, this
 * file does not by itself cause the resulting executable to be covered by
 * the GNU General Public License.  This exception does not however
 * invalidate any other reasons why the executable file might be covered by
 * the GNU General Public License.
 *
 * Copyright 2010 Develer S.r.l. (http://www.develer.com/)
 *
 * -->
 *
 * \brief Cortex-M3 architecture's entry point
 *
 * \author Andrea Righi <arighi@develer.com>
 */

#include <cfg/compiler.h>
#include <cfg/cfg_proc.h> /* CONFIG_KERN_PREEMPT */
#include <kern/proc_p.h>
#include <cfg/debug.h>
#include <cpu/attr.h> /* PAUSE */
#include <cpu/irq.h> /* IRQ_DISABLE */
#include <cpu/types.h>
#include "drv/irq_lm3s.h"
#include "drv/clock_lm3s.h"
#include "io/lm3s.h"

extern size_t __text_end, __data_start, __data_end, __bss_start, __bss_end;

extern void __init2(void);

#if CONFIG_KERN_PREEMPT
/*
 * Kernel preemption: implementation details.
 *
 * The kernel preemption is implemented using the PendSV IRQ. Inside the
 * SysTick handler when a process needs to be interrupted (expires its time
 * quantum or a high-priority process is awakend) a pending PendSV call is
 * triggered.
 *
 * The PendSV handler is called immediately after the SysTick handler, using
 * the architecture's tail-chaining functionality (an ISR call without the
 * overhead of state saving and restoration between different IRQs). Inside the
 * PendSV handler we perform the stack-switching between the old and new
 * processes.
 *
 * Voluntary context switch is implemented as a soft-interrupt call (SVCall),
 * so any process is always suspended and resumed from an interrupt context.
 *
 * NOTE: interrupts must be disabled or enabled when resuming a process context
 * depending of the type of the previous suspension. If a process was suspended
 * by a voluntary context switch IRQs must be disabled on resume (voluntary
 * context switch always happen with IRQs disabled). Instead, if a process was
 * suspended by the kernel preemption IRQs must be always re-enabled, because
 * the PendSV handler resumes directly the process context. To keep track of
 * this, we save the state of the IRQ priority in register r3 before performing
 * the context switch.
 *
 * If CONFIG_KERN_PREEMPT is not enabled the cooperative implementation
 * fallbacks to the default stack-switching mechanism, performed directly in
 * thread-mode and implemented as a normal function call.
 */

/*
 * Voluntary context switch handler.
 */
static void NAKED svcall_handler(void)
{
	asm volatile (
	/* Save context */
		"mrs r3, basepri\n\t"
		"mrs ip, psp\n\t"
		"stmdb ip!, {r3-r11, lr}\n\t"
	/* Stack switch */
		"str ip, [r1]\n\t"
		"ldr ip, [r0]\n\t"
	/* Restore context */
		"ldmia ip!, {r3-r11, lr}\n\t"
		"msr psp, ip\n\t"
		"msr basepri, r3\n\t"
		"bx lr" : : : "memory");
}

/*
 * Preemptible context switch handler.
 */
static void NAKED pendsv_handler(void)
{
	register cpu_stack_t *stack asm("ip");

	asm volatile (
		"mrs r3, basepri\n\t"
		"mov %0, %2\n\t"
		"msr basepri, %0\n\t"
		"mrs %0, psp\n\t"
		"stmdb %0!, {r3-r11, lr}\n\t"
		: "=r"(stack)
		: "r"(stack), "i"(IRQ_PRIO_DISABLED)
		: "r3", "memory");
	proc_current()->stack = stack;
	proc_preempt();
	stack = proc_current()->stack;
	asm volatile (
		"ldmia %0!, {r3-r11, lr}\n\t"
		"msr psp, %0\n\t"
		"msr basepri, r3\n\t"
		"bx lr"
		: "=r"(stack) : "r"(stack)
		: "memory");
}
#endif

/* Architecture's entry point */
void __init2(void)
{
	/*
	 * The main application expects IRQs disabled.
	 */
	IRQ_DISABLE;

	/*
	 * PLL may not function properly at default LDO setting.
	 *
	 * Description:
	 *
	 * In designs that enable and use the PLL module, unstable device
	 * behavior may occur with the LDO set at its default of 2.5 volts or
	 * below (minimum of 2.25 volts). Designs that do not use the PLL
	 * module are not affected.
	 *
	 * Workaround: Prior to enabling the PLL module, it is recommended that
	 * the default LDO voltage setting of 2.5 V be adjusted to 2.75 V using
	 * the LDO Power Control (LDOPCTL) register.
	 *
	 * Silicon Revision Affected: A1, A2
	 *
	 * See also: Stellaris LM3S1968 A2 Errata documentation.
	 */
	if (REVISION_IS_A1 | REVISION_IS_A2)
		HWREG(SYSCTL_LDOPCTL) = SYSCTL_LDOPCTL_2_75V;

	/* Set the appropriate clocking configuration */
	clock_set_rate();

	/* Initialize IRQ vector table in RAM */
	sysirq_init();

#if CONFIG_KERN_PREEMPT
	/*
	 * Voluntary context switch handler.
	 *
	 * This software interrupt can always be triggered and must be
	 * dispatched as soon as possible, thus we just disable IRQ priority
	 * for it.
	 */
	sysirq_setHandler(FAULT_SVCALL, svcall_handler);
	sysirq_setPriority(FAULT_SVCALL, IRQ_PRIO_MAX);
	/*
	 * Preemptible context switch handler
	 *
	 * The priority of this IRQ must be the lowest priority in the system
	 * in order to run last in the interrupt service routines' chain.
	 */
	sysirq_setHandler(FAULT_PENDSV, pendsv_handler);
	sysirq_setPriority(FAULT_PENDSV, IRQ_PRIO_MIN);
#endif
}