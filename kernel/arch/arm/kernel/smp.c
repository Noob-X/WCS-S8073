/*
 *  linux/arch/arm/kernel/smp.c
 *
 *  Copyright (C) 2002 ARM Limited, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/cache.h>
#include <linux/profile.h>
#include <linux/errno.h>
#include <linux/ftrace.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/seq_file.h>
#include <linux/irq.h>
#include <linux/percpu.h>
#include <linux/clockchips.h>
#include <linux/completion.h>
#include <linux/cpufreq.h>

#include <asm/atomic.h>
#include <asm/cacheflush.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/topology.h>
#include <asm/mmu_context.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/processor.h>
#include <asm/sections.h>
#include <asm/tlbflush.h>
#include <asm/ptrace.h>
#include <asm/localtimer.h>
#include <asm/smp_plat.h>

#include <linux/mt_sched_mon.h>
/*
 * as from 2.5, kernels no longer have an init_tasks structure
 * so we need some other way of telling a new secondary core
 * where to place its SVC stack
 */
struct secondary_data secondary_data;

/*
 * 20120505 marc.huang
 * [PATCH] ARM: smp: Fix Unknown IPI message 0x1
 */
enum ipi_msg_type {
	IPI_CPU_START = 1,
	IPI_TIMER = 2,
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
	IPI_CALL_FUNC_SINGLE,
	IPI_CPU_STOP,
	IPI_CPU_BACKTRACE,
};

int __cpuinit __cpu_up(unsigned int cpu)
{
	struct cpuinfo_arm *ci = &per_cpu(cpu_data, cpu);
	struct task_struct *idle = ci->idle;
	pgd_t *pgd;
	int ret;

	/*
	 * Spawn a new process manually, if not already done.
	 * Grab a pointer to its task struct so we can mess with it
	 */
	if (!idle) {
		idle = fork_idle(cpu);
		if (IS_ERR(idle)) {
			printk(KERN_ERR "CPU%u: fork() failed\n", cpu);
			return PTR_ERR(idle);
		}
		ci->idle = idle;
	} else {
		/*
		 * Since this idle thread is being re-used, call
		 * init_idle() to reinitialize the thread structure.
		 */
		init_idle(idle, cpu);
	}

	/*
	 * Allocate initial page tables to allow the new CPU to
	 * enable the MMU safely.  This essentially means a set
	 * of our "standard" page tables, with the addition of
	 * a 1:1 mapping for the physical address of the kernel.
	 */
	pgd = pgd_alloc(&init_mm);
	if (!pgd)
		return -ENOMEM;

	if (PHYS_OFFSET != PAGE_OFFSET) {
#ifndef CONFIG_HOTPLUG_CPU
		identity_mapping_add(pgd, __pa(__init_begin), __pa(__init_end));
#endif
		identity_mapping_add(pgd, __pa(_stext), __pa(_etext));
		identity_mapping_add(pgd, __pa(_sdata), __pa(_edata));
	}

	/*
	 * We need to tell the secondary core where to find
	 * its stack and the page tables.
	 */
	secondary_data.stack = task_stack_page(idle) + THREAD_START_SP;
	secondary_data.pgdir = virt_to_phys(pgd);
	secondary_data.swapper_pg_dir = virt_to_phys(swapper_pg_dir);
	__cpuc_flush_dcache_area(&secondary_data, sizeof(secondary_data));
	outer_clean_range(__pa(&secondary_data), __pa(&secondary_data + 1));

	/*
	 * Now bring the CPU into our world.
	 */
	ret = boot_secondary(cpu, idle);
	if (ret == 0) {
		unsigned long timeout;

		/*
		 * CPU was successfully started, wait for it
		 * to come online or time out.
		 */
		timeout = jiffies + HZ;
		while (time_before(jiffies, timeout)) {
			if (cpu_online(cpu))
				break;

			udelay(10);
			barrier();
		}

		if (!cpu_online(cpu)) {
			pr_crit("CPU%u: failed to come online\n", cpu);
			ret = -EIO;
		}
	} else {
		pr_err("CPU%u: failed to boot: %d\n", cpu, ret);
	}

	secondary_data.stack = NULL;
	secondary_data.pgdir = 0;

	if (PHYS_OFFSET != PAGE_OFFSET) {
#ifndef CONFIG_HOTPLUG_CPU
		identity_mapping_del(pgd, __pa(__init_begin), __pa(__init_end));
#endif
		identity_mapping_del(pgd, __pa(_stext), __pa(_etext));
		identity_mapping_del(pgd, __pa(_sdata), __pa(_edata));
	}

	pgd_free(&init_mm, pgd);

	return ret;
}

#ifdef CONFIG_HOTPLUG_CPU
static void percpu_timer_stop(void);

/*
 * __cpu_disable runs on the processor to be shutdown.
 */
int __cpu_disable(void)
{
	unsigned int cpu = smp_processor_id();
	struct task_struct *p;
	int ret;

	ret = platform_cpu_disable(cpu);
	if (ret)
		return ret;

	/*
	 * Take this CPU offline.  Once we clear this, we can't return,
	 * and we must not schedule until we're ready to give up the cpu.
	 */
	set_cpu_online(cpu, false);

	/*
	 * OK - migrate IRQs away from this CPU
	 */
	migrate_irqs();

	/*
	 * Stop the local timer for this CPU.
	 */
	percpu_timer_stop();

	/*
	 * Flush user cache and TLB mappings, and then remove this CPU
	 * from the vm mask set of all processes.
	 */
	flush_cache_all();
	local_flush_tlb_all();

	read_lock(&tasklist_lock);
	for_each_process(p) {
		if (p->mm)
			cpumask_clear_cpu(cpu, mm_cpumask(p->mm));
	}
	read_unlock(&tasklist_lock);

	return 0;
}

static DECLARE_COMPLETION(cpu_died);

/*
 * called on the thread which is asking for a CPU to be shutdown -
 * waits until shutdown has completed, or it is timed out.
 */
void __cpu_die(unsigned int cpu)
{
	if (!wait_for_completion_timeout(&cpu_died, msecs_to_jiffies(5000))) {
		pr_err("CPU%u: cpu didn't die\n", cpu);
		return;
	}
	printk(KERN_NOTICE "CPU%u: shutdown\n", cpu);

	if (!platform_cpu_kill(cpu))
		printk("CPU%u: unable to kill\n", cpu);
}

/*
 * Called from the idle thread for the CPU which has been shutdown.
 *
 * Note that we disable IRQs here, but do not re-enable them
 * before returning to the caller. This is also the behaviour
 * of the other hotplug-cpu capable cores, so presumably coming
 * out of idle fixes this.
 */
void __ref cpu_die(void)
{
	unsigned int cpu = smp_processor_id();

	idle_task_exit();

	local_irq_disable();
	mb();

	/* Tell __cpu_die() that this CPU is now safe to dispose of */
	complete(&cpu_died);

	/*
	 * actual CPU shutdown procedure is at least platform (if not
	 * CPU) specific.
	 */
	platform_cpu_die(cpu);

	/*
	 * Do not return to the idle loop - jump back to the secondary
	 * cpu initialisation.  There's some initialisation which needs
	 * to be repeated to undo the effects of taking the CPU offline.
	 */
	__asm__("mov	sp, %0\n"
	"	mov	fp, #0\n"
	"	b	secondary_start_kernel"
		:
		: "r" (task_stack_page(current) + THREAD_SIZE - 8));
}
#endif /* CONFIG_HOTPLUG_CPU */

int __cpu_logical_map[NR_CPUS];

void __init smp_setup_processor_id(void)
{
	int i;
	u32 cpu = is_smp() ? read_cpuid_mpidr() & 0xff : 0;

	cpu_logical_map(0) = cpu;
	for (i = 1; i < NR_CPUS; ++i)
		cpu_logical_map(i) = i == cpu ? 0 : i;

	printk(KERN_INFO "Booting Linux on physical CPU %d\n", cpu);
}

/*
 * Called by both boot and secondaries to move global data into
 * per-processor storage.
 */
static void __cpuinit smp_store_cpu_info(unsigned int cpuid)
{
	struct cpuinfo_arm *cpu_info = &per_cpu(cpu_data, cpuid);

	cpu_info->loops_per_jiffy = loops_per_jiffy;

	store_cpu_topology(cpuid);
}

/*
 * This is the secondary CPU boot entry.  We're using this CPUs
 * idle thread stack, but a set of temporary page tables.
 */
asmlinkage void __cpuinit secondary_start_kernel(void)
{
	struct mm_struct *mm = &init_mm;
	unsigned int cpu = smp_processor_id();
	
	/*
	 * 20120505 marc.huang
	 * Do calibrate_delay() only when 1st bring up to decrease up secondary core time
	 */
	static int is_first_boot = 1;
    
	printk("CPU%u: Booted secondary processor\n", cpu);

	/*
	 * All kernel threads share the same mm context; grab a
	 * reference and switch to it.
	 */
	atomic_inc(&mm->mm_count);
	current->active_mm = mm;
	cpumask_set_cpu(cpu, mm_cpumask(mm));
	cpu_switch_mm(mm->pgd, mm);
	enter_lazy_tlb(mm, current);
	local_flush_tlb_all();

	cpu_init();
	preempt_disable();
	trace_hardirqs_off();

	/*
	 * Give the platform a chance to do its own initialisation.
	 */
	platform_secondary_init(cpu);

	notify_cpu_starting(cpu);

	/*
	 * 20120505 marc.huang
	 * Do calibrate_delay() only when 1st bring up to decrease up secondary core time
	 */
	if (is_first_boot == 1)
	{
		calibrate_delay();
		is_first_boot = 0;
	}

	smp_store_cpu_info(cpu);

	/*
	 * OK, now it's safe to let the boot CPU continue.  Wait for
	 * the CPU migration code to notice that the CPU is online
	 * before we continue.
	 */
	set_cpu_online(cpu, true);

	/*
	 * Setup the percpu timer for this CPU.
	 */
	percpu_timer_setup();

	local_irq_enable();
	local_fiq_enable();

	/*
	 * OK, it's off to the idle thread for us
	 */
	cpu_idle();
}

void __init smp_cpus_done(unsigned int max_cpus)
{
	int cpu;
	unsigned long bogosum = 0;

	for_each_online_cpu(cpu)
		bogosum += per_cpu(cpu_data, cpu).loops_per_jiffy;

	printk(KERN_INFO "SMP: Total of %d processors activated "
	       "(%lu.%02lu BogoMIPS).\n",
	       num_online_cpus(),
	       bogosum / (500000/HZ),
	       (bogosum / (5000/HZ)) % 100);
}

void __init smp_prepare_boot_cpu(void)
{
	unsigned int cpu = smp_processor_id();

	per_cpu(cpu_data, cpu).idle = current;
}

void __init smp_prepare_cpus(unsigned int max_cpus)
{
	unsigned int ncores = num_possible_cpus();

	init_cpu_topology();

	smp_store_cpu_info(smp_processor_id());

	/*
	 * are we trying to boot more cores than exist?
	 */
	if (max_cpus > ncores)
		max_cpus = ncores;
	if (ncores > 1 && max_cpus) {
		/*
		 * Enable the local timer or broadcast device for the
		 * boot CPU, but only if we have more than one CPU.
		 */
		percpu_timer_setup();

		/*
		 * Initialise the present map, which describes the set of CPUs
		 * actually populated at the present time. A platform should
		 * re-initialize the map in platform_smp_prepare_cpus() if
		 * present != possible (e.g. physical hotplug).
		 */
		init_cpu_present(&cpu_possible_map);

		/*
		 * Initialise the SCU if there are more than one CPU
		 * and let them know where to start.
		 */
		platform_smp_prepare_cpus(max_cpus);
	}
}

static void (*smp_cross_call)(const struct cpumask *, unsigned int);

void __init set_smp_cross_call(void (*fn)(const struct cpumask *, unsigned int))
{
	smp_cross_call = fn;
}

void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	smp_cross_call(mask, IPI_CALL_FUNC);
}

void arch_send_call_function_single_ipi(int cpu)
{
	smp_cross_call(cpumask_of(cpu), IPI_CALL_FUNC_SINGLE);
}

/*
 * 20120505 marc.huang
 * [PATCH] ARM: smp: Fix Unknown IPI message 0x1
 */
static const char *ipi_types[NR_IPI] = {
#define S(x,s)	[x - IPI_CPU_START] = s
	S(IPI_CPU_START, "CPU start interrupts"),
	S(IPI_TIMER, "Timer broadcast interrupts"),
	S(IPI_RESCHEDULE, "Rescheduling interrupts"),
	S(IPI_CALL_FUNC, "Function call interrupts"),
	S(IPI_CALL_FUNC_SINGLE, "Single function call interrupts"),
	S(IPI_CPU_STOP, "CPU stop interrupts"),
	S(IPI_CPU_BACKTRACE, "CPU backtrace"),
};

void show_ipi_list(struct seq_file *p, int prec)
{
	unsigned int cpu, i;

	for (i = 0; i < NR_IPI; i++) {
		seq_printf(p, "%*s%u: ", prec - 1, "IPI", i);

		for_each_present_cpu(cpu)
			seq_printf(p, "%10u ",
				   __get_irq_stat(cpu, ipi_irqs[i]));

		seq_printf(p, " %s\n", ipi_types[i]);
	}
}

u64 smp_irq_stat_cpu(unsigned int cpu)
{
	u64 sum = 0;
	int i;

	for (i = 0; i < NR_IPI; i++)
		sum += __get_irq_stat(cpu, ipi_irqs[i]);

#ifdef CONFIG_LOCAL_TIMERS
	sum += __get_irq_stat(cpu, local_timer_irqs);
#endif

	return sum;
}

/*
 * Timer (local or broadcast) support
 */
static DEFINE_PER_CPU(struct clock_event_device, percpu_clockevent);

static void ipi_timer(void)
{
	struct clock_event_device *evt = &__get_cpu_var(percpu_clockevent);
    int irq_nr = __raw_get_cpu_var(mt_timer_irq); 
    mt_trace_ISR_start(irq_nr);
	irq_enter();
	evt->event_handler(evt);
    mt_trace_ISR_end(irq_nr);
	irq_exit();
}

#ifdef CONFIG_LOCAL_TIMERS
asmlinkage void __exception_irq_entry do_local_timer(struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
    int cpu = smp_processor_id();
#ifdef CONFIG_MT_SCHED_MONITOR
    __raw_get_cpu_var(local_timer_ts) = sched_clock();
    __raw_get_cpu_var(timer_ack_ts) = 0;
    mt_trace_ISR_start(MT_LOCAL_TIMER_IRQ);

    if (local_timer_ack()) {
        __raw_get_cpu_var(timer_ack_ts) = sched_clock();
        __raw_get_cpu_var(mt_timer_irq) = MT_LOCAL_TIMER_IRQ;
		__inc_irq_stat(cpu, local_timer_irqs);
		ipi_timer();
	}else   
        mt_trace_ISR_end(MT_LOCAL_TIMER_IRQ);

    __raw_get_cpu_var(local_timer_te) = sched_clock();
#else
    if (local_timer_ack()) {
		__inc_irq_stat(cpu, local_timer_irqs);
		ipi_timer();
	}
#endif
	set_irq_regs(old_regs);
}

void show_local_irqs(struct seq_file *p, int prec)
{
	unsigned int cpu;

	seq_printf(p, "%*s: ", prec, "LOC");

	for_each_present_cpu(cpu)
		seq_printf(p, "%10u ", __get_irq_stat(cpu, local_timer_irqs));

	seq_printf(p, " Local timer interrupts\n");
}
#endif

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
static void smp_timer_broadcast(const struct cpumask *mask)
{
	smp_cross_call(mask, IPI_TIMER);
}
#else
#define smp_timer_broadcast	NULL
#endif

static void broadcast_timer_set_mode(enum clock_event_mode mode,
	struct clock_event_device *evt)
{
}

static void __cpuinit broadcast_timer_setup(struct clock_event_device *evt)
{
	evt->name	= "dummy_timer";
	evt->features	= CLOCK_EVT_FEAT_ONESHOT |
			  CLOCK_EVT_FEAT_PERIODIC |
			  CLOCK_EVT_FEAT_DUMMY;
	evt->rating	= 400;
	evt->mult	= 1;
	evt->set_mode	= broadcast_timer_set_mode;

	clockevents_register_device(evt);
}

void __cpuinit percpu_timer_setup(void)
{
	unsigned int cpu = smp_processor_id();
	struct clock_event_device *evt = &per_cpu(percpu_clockevent, cpu);

	evt->cpumask = cpumask_of(cpu);
	evt->broadcast = smp_timer_broadcast;

	if (local_timer_setup(evt))
		broadcast_timer_setup(evt);
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * The generic clock events code purposely does not stop the local timer
 * on CPU_DEAD/CPU_DEAD_FROZEN hotplug events, so we have to do it
 * manually here.
 */
static void percpu_timer_stop(void)
{
	unsigned int cpu = smp_processor_id();
	struct clock_event_device *evt = &per_cpu(percpu_clockevent, cpu);

	evt->set_mode(CLOCK_EVT_MODE_UNUSED, evt);
}
#endif

static DEFINE_RAW_SPINLOCK(stop_lock);

/*
 * ipi_cpu_stop - handle IPI from smp_send_stop()
 */
static void ipi_cpu_stop(unsigned int cpu)
{
	if (system_state == SYSTEM_BOOTING ||
	    system_state == SYSTEM_RUNNING) {
		raw_spin_lock(&stop_lock);
		printk(KERN_CRIT "CPU%u: stopping\n", cpu);
		dump_stack();
		raw_spin_unlock(&stop_lock);
	}

	set_cpu_online(cpu, false);

	local_fiq_disable();
	local_irq_disable();

        /* For L1 data coherence with the other cores, 
         * we need to flush this core's l1 cache. by Chia-Hao Hsu 
         */
        flush_cache_all();
        cpu_proc_fin();
        flush_cache_all();

	while (1)
		cpu_relax();
}

static cpumask_t backtrace_mask;
static DEFINE_RAW_SPINLOCK(backtrace_lock);

/* "in progress" flag of arch_trigger_all_cpu_backtrace */
static unsigned long backtrace_flag;

void smp_send_all_cpu_backtrace(void)
{
	unsigned int this_cpu = smp_processor_id();
	int i;

	if (test_and_set_bit(0, &backtrace_flag))
		/*
		 * If there is already a trigger_all_cpu_backtrace() in progress
		 * (backtrace_flag == 1), don't output double cpu dump infos.
		 */
		return;

	cpumask_copy(&backtrace_mask, cpu_online_mask);
	cpu_clear(this_cpu, backtrace_mask);

	pr_info("Backtrace for cpu %d (current):\n", this_cpu);
	dump_stack();

	pr_info("\nsending IPI to all other CPUs:\n");
	smp_cross_call(&backtrace_mask, IPI_CPU_BACKTRACE);

	/* Wait for up to 10 seconds for all other CPUs to do the backtrace */
	for (i = 0; i < 10 * 1000; i++) {
		if (cpumask_empty(&backtrace_mask))
			break;
		mdelay(1);
	}

	clear_bit(0, &backtrace_flag);
	smp_mb__after_clear_bit();
}

/*
 * ipi_cpu_backtrace - handle IPI from smp_send_all_cpu_backtrace()
 */
static void ipi_cpu_backtrace(unsigned int cpu, struct pt_regs *regs)
{
	if (cpu_isset(cpu, backtrace_mask)) {
		raw_spin_lock(&backtrace_lock);
		pr_warning("IPI backtrace for cpu %d\n", cpu);
		show_regs(regs);
		raw_spin_unlock(&backtrace_lock);
		cpu_clear(cpu, backtrace_mask);
	}
}

/*
 * Main handler for inter-processor interrupts
 */
asmlinkage void __exception_irq_entry do_IPI(int ipinr, struct pt_regs *regs)
{
	unsigned int cpu = smp_processor_id();
	struct pt_regs *old_regs = set_irq_regs(regs);

    if(!(ipinr == IPI_TIMER || ipinr == IPI_RESCHEDULE))
        mt_trace_ISR_start(ipinr);
	/*
 	* 20120505 marc.huang
 	* [PATCH] ARM: smp: Fix Unknown IPI message 0x1
 	*/
	if (ipinr >= IPI_CPU_START && ipinr < IPI_CPU_START + NR_IPI)
		__inc_irq_stat(cpu, ipi_irqs[ipinr - IPI_CPU_START]);

	switch (ipinr) {
	/*
 	* 20120505 marc.huang
 	* [PATCH] ARM: smp: Fix Unknown IPI message 0x1
 	*/
	case IPI_CPU_START:
		/* Wake up from WFI/WFE using SGI */
		break;

	case IPI_TIMER:
        __raw_get_cpu_var(mt_timer_irq) = IPI_TIMER;
		ipi_timer();
		break;

	case IPI_RESCHEDULE:
		scheduler_ipi();
		break;

	case IPI_CALL_FUNC:
		generic_smp_call_function_interrupt();
		break;

	case IPI_CALL_FUNC_SINGLE:
		generic_smp_call_function_single_interrupt();
		break;

	case IPI_CPU_STOP:
		ipi_cpu_stop(cpu);
		break;

	case IPI_CPU_BACKTRACE:
		ipi_cpu_backtrace(cpu, regs);
		break;

	default:
		printk(KERN_CRIT "CPU%u: Unknown IPI message 0x%x\n",
		       cpu, ipinr);
		break;
	}
    if(!(ipinr == IPI_TIMER || ipinr == IPI_RESCHEDULE))
        mt_trace_ISR_end(ipinr);
	set_irq_regs(old_regs);
}

void smp_send_reschedule(int cpu)
{
	smp_cross_call(cpumask_of(cpu), IPI_RESCHEDULE);
}
//
//Update Patch
//http://git.kernel.org/?p=linux/kernel/git/torvalds/linux.git;a=commitdiff;h=6fa99b7f80b4a7ed2cf616eae393bb6d9d51ba8f
//
//#ifdef CONFIG_HOTPLUG_CPU
//static void smp_kill_cpus(cpumask_t *mask)
//{
//	unsigned int cpu;
//	for_each_cpu(cpu, mask)
//		platform_cpu_kill(cpu);
//}
//#else
//	static void smp_kill_cpus(cpumask_t *mask) { }
//#endif

void smp_send_stop(void)
{
	unsigned long timeout;
	//struct cpumask mask;

	if (num_online_cpus() > 1) {
		cpumask_t mask = cpu_online_map;
		cpu_clear(smp_processor_id(), mask);

		smp_cross_call(&mask, IPI_CPU_STOP);
	}

	//cpumask_copy(&mask, cpu_online_mask);
	//cpumask_clear_cpu(smp_processor_id(), &mask);
	//smp_cross_call(&mask, IPI_CPU_STOP);
	/* Wait up to one second for other CPUs to stop */
	timeout = USEC_PER_SEC;
	while (num_online_cpus() > 1 && timeout--)
		udelay(1);

	if (num_online_cpus() > 1)
		pr_warning("SMP: failed to stop secondary CPUs\n");
	//smp_kill_cpus(&mask);
}

/*
 * not supported here
 */
int setup_profiling_timer(unsigned int multiplier)
{
	return -EINVAL;
}

#ifdef CONFIG_CPU_FREQ

static DEFINE_PER_CPU(unsigned long, l_p_j_ref);
static DEFINE_PER_CPU(unsigned long, l_p_j_ref_freq);
static unsigned long global_l_p_j_ref;
static unsigned long global_l_p_j_ref_freq;

static int cpufreq_callback(struct notifier_block *nb,
					unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	int cpu = freq->cpu;

	if (freq->flags & CPUFREQ_CONST_LOOPS)
		return NOTIFY_OK;

	if (!per_cpu(l_p_j_ref, cpu)) {
		per_cpu(l_p_j_ref, cpu) =
			per_cpu(cpu_data, cpu).loops_per_jiffy;
		per_cpu(l_p_j_ref_freq, cpu) = freq->old;
		if (!global_l_p_j_ref) {
			global_l_p_j_ref = loops_per_jiffy;
			global_l_p_j_ref_freq = freq->old;
		}
	}

	if ((val == CPUFREQ_PRECHANGE  && freq->old < freq->new) ||
	    (val == CPUFREQ_POSTCHANGE && freq->old > freq->new) ||
	    (val == CPUFREQ_RESUMECHANGE || val == CPUFREQ_SUSPENDCHANGE)) {
		loops_per_jiffy = cpufreq_scale(global_l_p_j_ref,
						global_l_p_j_ref_freq,
						freq->new);
		per_cpu(cpu_data, cpu).loops_per_jiffy =
			cpufreq_scale(per_cpu(l_p_j_ref, cpu),
					per_cpu(l_p_j_ref_freq, cpu),
					freq->new);
	}
	return NOTIFY_OK;
}

static struct notifier_block cpufreq_notifier = {
	.notifier_call  = cpufreq_callback,
};

static int __init register_cpufreq_notifier(void)
{
	return cpufreq_register_notifier(&cpufreq_notifier,
						CPUFREQ_TRANSITION_NOTIFIER);
}
core_initcall(register_cpufreq_notifier);

#endif
