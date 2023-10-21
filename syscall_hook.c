#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/unistd.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/uaccess.h>

#include "syscall_hook.h"
#include "our_syscall_table.h"

/* The way we access "sys_call_table" varies as kernel internal changes.
 * - Prior to v5.4 : manual symbol lookup
 * - v5.5 to v5.6  : use kallsyms_lookup_name()
 * - v5.7+         : Kprobes or specific kernel module parameter
 */

/* The in-kernel calls to the ksys_close() syscall were removed in Linux v5.11+.
 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0))

#if LINUX_VERSION_CODE <= KERNEL_VERSION(5, 4, 0)
#define HAVE_KSYS_CLOSE 1
#include <linux/syscalls.h> /* For ksys_close() */
#else
#include <linux/kallsyms.h> /* For kallsyms_lookup_name */
#endif

#else

#if defined(CONFIG_KPROBES)
#define HAVE_KPROBES 1
#include <linux/kprobes.h>
#else
#define HAVE_PARAM 1
#include <linux/kallsyms.h> /* For sprint_symbol */
/* The address of the sys_call_table, which can be obtained with looking up
 * "/boot/System.map" or "/proc/kallsyms". When the kernel version is v5.7+,
 * without CONFIG_KPROBES, you can input the parameter or the module will look
 * up all the memory.
 */
static unsigned long sym = 0;
module_param(sym, ulong, 0644);
#endif                      /* CONFIG_KPROBES */

#endif /* Version < v5.7 */

static DEFINE_MUTEX(syacall_mutex);
static unsigned long *orig_syscall_tale[NR_syscalls];
static unsigned long *our_syscall_table[NR_syscalls];
static void gen_our_syscall(void);

static unsigned long **acquire_sys_call_table(void)
{
#ifdef HAVE_KSYS_CLOSE
    unsigned long int offset = PAGE_OFFSET;
    unsigned long **sct;

    while (offset < ULLONG_MAX)
    {
        sct = (unsigned long **)offset;

        if (sct[__NR_close] == (unsigned long *)ksys_close)
            return sct;

        offset += sizeof(void *);
    }

    return NULL;
#endif

#ifdef HAVE_PARAM
    const char sct_name[15] = "sys_call_table";
    char symbol[40] = {0};

    if (sym == 0)
    {
        pr_alert("For Linux v5.7+, Kprobes is the preferable way to get "
                 "symbol.\n");
        pr_info("If Kprobes is absent, you have to specify the address of "
                "sys_call_table symbol\n");
        pr_info("by /boot/System.map or /proc/kallsyms, which contains all the "
                "symbol addresses, into sym parameter.\n");
        return NULL;
    }
    sprint_symbol(symbol, sym);
    if (!strncmp(sct_name, symbol, sizeof(sct_name) - 1))
        return (unsigned long **)sym;

    return NULL;
#endif

#ifdef HAVE_KPROBES
    unsigned long (*kallsyms_lookup_name)(const char *name);
    struct kprobe kp = {
        .symbol_name = "kallsyms_lookup_name",
    };

    if (register_kprobe(&kp) < 0)
        return NULL;
    kallsyms_lookup_name = (unsigned long (*)(const char *name))kp.addr;
    unregister_kprobe(&kp);
#endif

    return (unsigned long **)kallsyms_lookup_name("sys_call_table");
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
static inline void __write_cr0(unsigned long cr0)
{
    asm volatile("mov %0,%%cr0" : "+r"(cr0) : : "memory");
}
#else
#define __write_cr0 write_cr0
#endif

static void enable_write_protection(void)
{
    unsigned long cr0 = read_cr0();
    set_bit(16, &cr0);
    __write_cr0(cr0);
}

static void disable_write_protection(void)
{
    unsigned long cr0 = read_cr0();
    clear_bit(16, &cr0);
    __write_cr0(cr0);
}

long get_syscall_table(void)
{
    static unsigned long **sys_call_table = NULL;
    if (sys_call_table != NULL)
        return (long)sys_call_table;

    if (!mutex_trylock(&syacall_mutex))
    {
        printk(KERN_ERR "Failed to lock scc syscall mutex\n");
        return EBUSY;
    }

    sys_call_table = acquire_sys_call_table();

    mutex_unlock(&syacall_mutex);

    return (long)sys_call_table;
}

int save_original_syscall(void)
{
    long **table = (long**)get_syscall_table(); // system table should be negative
    if (!((unsigned long)table & 0x8000000000000000))
    {
        printk(KERN_ERR "Failed to get syscall table\n");
        return (int)table;
    }

    if (!mutex_trylock(&syacall_mutex))
    {
        printk(KERN_ERR "Failed to lock scc syscall mutex\n");
        return -EBUSY;
    }
    for (int i = 0; i < NR_syscalls; i++)
        orig_syscall_tale[i] = table[i];
    mutex_unlock(&syacall_mutex);

    return 0;
}

static int hook_syscall_impl(int nr, void *func)
{
    long **table = (long **)get_syscall_table();

    if (!mutex_trylock(&syacall_mutex))
        return -EBUSY;

    disable_write_protection();
    printk(KERN_DEBUG "hooking syscall %d\n", nr);
    table[nr] = (unsigned long *)func;
    enable_write_protection();

    mutex_unlock(&syacall_mutex);
    return 0;
}

int hook_syscall(void)
{
    long **table = (long **)get_syscall_table();
    if (!((unsigned long)table & 0x8000000000000000))
    {
        printk(KERN_ERR "Failed to get syscall table\n");
        return (int)table;
    }

    get_our_syscall_table();
    for (int i = 0; i < NR_syscalls; i++)
    {
        int rc = hook_syscall_impl(i, our_syscall_table[i]);
        if (rc < 0)
        {
            printk(KERN_ERR "Failed to hook openat\n");
            return rc;
        }
    }

    return 0;
}

static inline void clear_orig_syscall(void)
{
    for (int i = 0; i < NR_syscalls; i++)
        orig_syscall_tale[i] = NULL;
}

int unhook_syscall(void)
{
    long **table = (long **)get_syscall_table();
    if (!((unsigned long)table & 0x8000000000000000))
    {
        printk(KERN_ERR "Failed to get syscall table\n");
        return (int)table;
    }

    if (!mutex_trylock(&syacall_mutex))
    {
        printk(KERN_ERR "Failed to lock scc syscall mutex\n");
        return -EBUSY;
    }

    disable_write_protection();
    for (int i = 0; i < NR_syscalls; i++)
        table[i] = orig_syscall_tale[i];
    enable_write_protection();

    clear_orig_syscall();

    mutex_unlock(&syacall_mutex);

    return 0;
}

unsigned long **get_our_syscall_table(void)
{
    gen_our_syscall();
    return our_syscall_table;
}

#define DECLARE_OUR_SYSCALL_TABLE(number) \
    OUR_SYSCALL_IMPL(number, orig_syscall_tale[number])

DECLARE_OUR_SYSCALL_TABLE(0)
DECLARE_OUR_SYSCALL_TABLE(1)
DECLARE_OUR_SYSCALL_TABLE(2)
DECLARE_OUR_SYSCALL_TABLE(3)
DECLARE_OUR_SYSCALL_TABLE(4)
DECLARE_OUR_SYSCALL_TABLE(5)
DECLARE_OUR_SYSCALL_TABLE(6)
DECLARE_OUR_SYSCALL_TABLE(7)
DECLARE_OUR_SYSCALL_TABLE(8)
DECLARE_OUR_SYSCALL_TABLE(9)
DECLARE_OUR_SYSCALL_TABLE(10)
DECLARE_OUR_SYSCALL_TABLE(11)
DECLARE_OUR_SYSCALL_TABLE(12)
DECLARE_OUR_SYSCALL_TABLE(13)
DECLARE_OUR_SYSCALL_TABLE(14)
DECLARE_OUR_SYSCALL_TABLE(15)
DECLARE_OUR_SYSCALL_TABLE(16)
DECLARE_OUR_SYSCALL_TABLE(17)
DECLARE_OUR_SYSCALL_TABLE(18)
DECLARE_OUR_SYSCALL_TABLE(19)
DECLARE_OUR_SYSCALL_TABLE(20)
DECLARE_OUR_SYSCALL_TABLE(21)
DECLARE_OUR_SYSCALL_TABLE(22)
DECLARE_OUR_SYSCALL_TABLE(23)
DECLARE_OUR_SYSCALL_TABLE(24)
DECLARE_OUR_SYSCALL_TABLE(25)
DECLARE_OUR_SYSCALL_TABLE(26)
DECLARE_OUR_SYSCALL_TABLE(27)
DECLARE_OUR_SYSCALL_TABLE(28)
DECLARE_OUR_SYSCALL_TABLE(29)
DECLARE_OUR_SYSCALL_TABLE(30)
DECLARE_OUR_SYSCALL_TABLE(31)
DECLARE_OUR_SYSCALL_TABLE(32)
DECLARE_OUR_SYSCALL_TABLE(33)
DECLARE_OUR_SYSCALL_TABLE(34)
DECLARE_OUR_SYSCALL_TABLE(35)
DECLARE_OUR_SYSCALL_TABLE(36)
DECLARE_OUR_SYSCALL_TABLE(37)
DECLARE_OUR_SYSCALL_TABLE(38)
DECLARE_OUR_SYSCALL_TABLE(39)
DECLARE_OUR_SYSCALL_TABLE(40)
DECLARE_OUR_SYSCALL_TABLE(41)
DECLARE_OUR_SYSCALL_TABLE(42)
DECLARE_OUR_SYSCALL_TABLE(43)
DECLARE_OUR_SYSCALL_TABLE(44)
DECLARE_OUR_SYSCALL_TABLE(45)
DECLARE_OUR_SYSCALL_TABLE(46)
DECLARE_OUR_SYSCALL_TABLE(47)
DECLARE_OUR_SYSCALL_TABLE(48)
DECLARE_OUR_SYSCALL_TABLE(49)
DECLARE_OUR_SYSCALL_TABLE(50)
DECLARE_OUR_SYSCALL_TABLE(51)
DECLARE_OUR_SYSCALL_TABLE(52)
DECLARE_OUR_SYSCALL_TABLE(53)
DECLARE_OUR_SYSCALL_TABLE(54)
DECLARE_OUR_SYSCALL_TABLE(55)
DECLARE_OUR_SYSCALL_TABLE(56)
DECLARE_OUR_SYSCALL_TABLE(57)
DECLARE_OUR_SYSCALL_TABLE(58)
DECLARE_OUR_SYSCALL_TABLE(59)
DECLARE_OUR_SYSCALL_TABLE(60)
DECLARE_OUR_SYSCALL_TABLE(61)
DECLARE_OUR_SYSCALL_TABLE(62)
DECLARE_OUR_SYSCALL_TABLE(63)
DECLARE_OUR_SYSCALL_TABLE(64)
DECLARE_OUR_SYSCALL_TABLE(65)
DECLARE_OUR_SYSCALL_TABLE(66)
DECLARE_OUR_SYSCALL_TABLE(67)
DECLARE_OUR_SYSCALL_TABLE(68)
DECLARE_OUR_SYSCALL_TABLE(69)
DECLARE_OUR_SYSCALL_TABLE(70)
DECLARE_OUR_SYSCALL_TABLE(71)
DECLARE_OUR_SYSCALL_TABLE(72)
DECLARE_OUR_SYSCALL_TABLE(73)
DECLARE_OUR_SYSCALL_TABLE(74)
DECLARE_OUR_SYSCALL_TABLE(75)
DECLARE_OUR_SYSCALL_TABLE(76)
DECLARE_OUR_SYSCALL_TABLE(77)
DECLARE_OUR_SYSCALL_TABLE(78)
DECLARE_OUR_SYSCALL_TABLE(79)
DECLARE_OUR_SYSCALL_TABLE(80)
DECLARE_OUR_SYSCALL_TABLE(81)
DECLARE_OUR_SYSCALL_TABLE(82)
DECLARE_OUR_SYSCALL_TABLE(83)
DECLARE_OUR_SYSCALL_TABLE(84)
DECLARE_OUR_SYSCALL_TABLE(85)
DECLARE_OUR_SYSCALL_TABLE(86)
DECLARE_OUR_SYSCALL_TABLE(87)
DECLARE_OUR_SYSCALL_TABLE(88)
DECLARE_OUR_SYSCALL_TABLE(89)
DECLARE_OUR_SYSCALL_TABLE(90)
DECLARE_OUR_SYSCALL_TABLE(91)
DECLARE_OUR_SYSCALL_TABLE(92)
DECLARE_OUR_SYSCALL_TABLE(93)
DECLARE_OUR_SYSCALL_TABLE(94)
DECLARE_OUR_SYSCALL_TABLE(95)
DECLARE_OUR_SYSCALL_TABLE(96)
DECLARE_OUR_SYSCALL_TABLE(97)
DECLARE_OUR_SYSCALL_TABLE(98)
DECLARE_OUR_SYSCALL_TABLE(99)
DECLARE_OUR_SYSCALL_TABLE(100)
DECLARE_OUR_SYSCALL_TABLE(101)
DECLARE_OUR_SYSCALL_TABLE(102)
DECLARE_OUR_SYSCALL_TABLE(103)
DECLARE_OUR_SYSCALL_TABLE(104)
DECLARE_OUR_SYSCALL_TABLE(105)
DECLARE_OUR_SYSCALL_TABLE(106)
DECLARE_OUR_SYSCALL_TABLE(107)
DECLARE_OUR_SYSCALL_TABLE(108)
DECLARE_OUR_SYSCALL_TABLE(109)
DECLARE_OUR_SYSCALL_TABLE(110)
DECLARE_OUR_SYSCALL_TABLE(111)
DECLARE_OUR_SYSCALL_TABLE(112)
DECLARE_OUR_SYSCALL_TABLE(113)
DECLARE_OUR_SYSCALL_TABLE(114)
DECLARE_OUR_SYSCALL_TABLE(115)
DECLARE_OUR_SYSCALL_TABLE(116)
DECLARE_OUR_SYSCALL_TABLE(117)
DECLARE_OUR_SYSCALL_TABLE(118)
DECLARE_OUR_SYSCALL_TABLE(119)
DECLARE_OUR_SYSCALL_TABLE(120)
DECLARE_OUR_SYSCALL_TABLE(121)
DECLARE_OUR_SYSCALL_TABLE(122)
DECLARE_OUR_SYSCALL_TABLE(123)
DECLARE_OUR_SYSCALL_TABLE(124)
DECLARE_OUR_SYSCALL_TABLE(125)
DECLARE_OUR_SYSCALL_TABLE(126)
DECLARE_OUR_SYSCALL_TABLE(127)
DECLARE_OUR_SYSCALL_TABLE(128)
DECLARE_OUR_SYSCALL_TABLE(129)
DECLARE_OUR_SYSCALL_TABLE(130)
DECLARE_OUR_SYSCALL_TABLE(131)
DECLARE_OUR_SYSCALL_TABLE(132)
DECLARE_OUR_SYSCALL_TABLE(133)
DECLARE_OUR_SYSCALL_TABLE(134)
DECLARE_OUR_SYSCALL_TABLE(135)
DECLARE_OUR_SYSCALL_TABLE(136)
DECLARE_OUR_SYSCALL_TABLE(137)
DECLARE_OUR_SYSCALL_TABLE(138)
DECLARE_OUR_SYSCALL_TABLE(139)
DECLARE_OUR_SYSCALL_TABLE(140)
DECLARE_OUR_SYSCALL_TABLE(141)
DECLARE_OUR_SYSCALL_TABLE(142)
DECLARE_OUR_SYSCALL_TABLE(143)
DECLARE_OUR_SYSCALL_TABLE(144)
DECLARE_OUR_SYSCALL_TABLE(145)
DECLARE_OUR_SYSCALL_TABLE(146)
DECLARE_OUR_SYSCALL_TABLE(147)
DECLARE_OUR_SYSCALL_TABLE(148)
DECLARE_OUR_SYSCALL_TABLE(149)
DECLARE_OUR_SYSCALL_TABLE(150)
DECLARE_OUR_SYSCALL_TABLE(151)
DECLARE_OUR_SYSCALL_TABLE(152)
DECLARE_OUR_SYSCALL_TABLE(153)
DECLARE_OUR_SYSCALL_TABLE(154)
DECLARE_OUR_SYSCALL_TABLE(155)
DECLARE_OUR_SYSCALL_TABLE(156)
DECLARE_OUR_SYSCALL_TABLE(157)
DECLARE_OUR_SYSCALL_TABLE(158)
DECLARE_OUR_SYSCALL_TABLE(159)
DECLARE_OUR_SYSCALL_TABLE(160)
DECLARE_OUR_SYSCALL_TABLE(161)
DECLARE_OUR_SYSCALL_TABLE(162)
DECLARE_OUR_SYSCALL_TABLE(163)
DECLARE_OUR_SYSCALL_TABLE(164)
DECLARE_OUR_SYSCALL_TABLE(165)
DECLARE_OUR_SYSCALL_TABLE(166)
DECLARE_OUR_SYSCALL_TABLE(167)
DECLARE_OUR_SYSCALL_TABLE(168)
DECLARE_OUR_SYSCALL_TABLE(169)
DECLARE_OUR_SYSCALL_TABLE(170)
DECLARE_OUR_SYSCALL_TABLE(171)
DECLARE_OUR_SYSCALL_TABLE(172)
DECLARE_OUR_SYSCALL_TABLE(173)
DECLARE_OUR_SYSCALL_TABLE(174)
DECLARE_OUR_SYSCALL_TABLE(175)
DECLARE_OUR_SYSCALL_TABLE(176)
DECLARE_OUR_SYSCALL_TABLE(177)
DECLARE_OUR_SYSCALL_TABLE(178)
DECLARE_OUR_SYSCALL_TABLE(179)
DECLARE_OUR_SYSCALL_TABLE(180)
DECLARE_OUR_SYSCALL_TABLE(181)
DECLARE_OUR_SYSCALL_TABLE(182)
DECLARE_OUR_SYSCALL_TABLE(183)
DECLARE_OUR_SYSCALL_TABLE(184)
DECLARE_OUR_SYSCALL_TABLE(185)
DECLARE_OUR_SYSCALL_TABLE(186)
DECLARE_OUR_SYSCALL_TABLE(187)
DECLARE_OUR_SYSCALL_TABLE(188)
DECLARE_OUR_SYSCALL_TABLE(189)
DECLARE_OUR_SYSCALL_TABLE(190)
DECLARE_OUR_SYSCALL_TABLE(191)
DECLARE_OUR_SYSCALL_TABLE(192)
DECLARE_OUR_SYSCALL_TABLE(193)
DECLARE_OUR_SYSCALL_TABLE(194)
DECLARE_OUR_SYSCALL_TABLE(195)
DECLARE_OUR_SYSCALL_TABLE(196)
DECLARE_OUR_SYSCALL_TABLE(197)
DECLARE_OUR_SYSCALL_TABLE(198)
DECLARE_OUR_SYSCALL_TABLE(199)
DECLARE_OUR_SYSCALL_TABLE(200)
DECLARE_OUR_SYSCALL_TABLE(201)
DECLARE_OUR_SYSCALL_TABLE(202)
DECLARE_OUR_SYSCALL_TABLE(203)
DECLARE_OUR_SYSCALL_TABLE(204)
DECLARE_OUR_SYSCALL_TABLE(205)
DECLARE_OUR_SYSCALL_TABLE(206)
DECLARE_OUR_SYSCALL_TABLE(207)
DECLARE_OUR_SYSCALL_TABLE(208)
DECLARE_OUR_SYSCALL_TABLE(209)
DECLARE_OUR_SYSCALL_TABLE(210)
DECLARE_OUR_SYSCALL_TABLE(211)
DECLARE_OUR_SYSCALL_TABLE(212)
DECLARE_OUR_SYSCALL_TABLE(213)
DECLARE_OUR_SYSCALL_TABLE(214)
DECLARE_OUR_SYSCALL_TABLE(215)
DECLARE_OUR_SYSCALL_TABLE(216)
DECLARE_OUR_SYSCALL_TABLE(217)
DECLARE_OUR_SYSCALL_TABLE(218)
DECLARE_OUR_SYSCALL_TABLE(219)
DECLARE_OUR_SYSCALL_TABLE(220)
DECLARE_OUR_SYSCALL_TABLE(221)
DECLARE_OUR_SYSCALL_TABLE(222)
DECLARE_OUR_SYSCALL_TABLE(223)
DECLARE_OUR_SYSCALL_TABLE(224)
DECLARE_OUR_SYSCALL_TABLE(225)
DECLARE_OUR_SYSCALL_TABLE(226)
DECLARE_OUR_SYSCALL_TABLE(227)
DECLARE_OUR_SYSCALL_TABLE(228)
DECLARE_OUR_SYSCALL_TABLE(229)
DECLARE_OUR_SYSCALL_TABLE(230)
DECLARE_OUR_SYSCALL_TABLE(231)
DECLARE_OUR_SYSCALL_TABLE(232)
DECLARE_OUR_SYSCALL_TABLE(233)
DECLARE_OUR_SYSCALL_TABLE(234)
DECLARE_OUR_SYSCALL_TABLE(235)
DECLARE_OUR_SYSCALL_TABLE(236)
DECLARE_OUR_SYSCALL_TABLE(237)
DECLARE_OUR_SYSCALL_TABLE(238)
DECLARE_OUR_SYSCALL_TABLE(239)
DECLARE_OUR_SYSCALL_TABLE(240)
DECLARE_OUR_SYSCALL_TABLE(241)
DECLARE_OUR_SYSCALL_TABLE(242)
DECLARE_OUR_SYSCALL_TABLE(243)
DECLARE_OUR_SYSCALL_TABLE(244)
DECLARE_OUR_SYSCALL_TABLE(245)
DECLARE_OUR_SYSCALL_TABLE(246)
DECLARE_OUR_SYSCALL_TABLE(247)
DECLARE_OUR_SYSCALL_TABLE(248)
DECLARE_OUR_SYSCALL_TABLE(249)
DECLARE_OUR_SYSCALL_TABLE(250)
DECLARE_OUR_SYSCALL_TABLE(251)
DECLARE_OUR_SYSCALL_TABLE(252)
DECLARE_OUR_SYSCALL_TABLE(253)
DECLARE_OUR_SYSCALL_TABLE(254)
DECLARE_OUR_SYSCALL_TABLE(255)
DECLARE_OUR_SYSCALL_TABLE(256)
DECLARE_OUR_SYSCALL_TABLE(257)
DECLARE_OUR_SYSCALL_TABLE(258)
DECLARE_OUR_SYSCALL_TABLE(259)
DECLARE_OUR_SYSCALL_TABLE(260)
DECLARE_OUR_SYSCALL_TABLE(261)
DECLARE_OUR_SYSCALL_TABLE(262)
DECLARE_OUR_SYSCALL_TABLE(263)
DECLARE_OUR_SYSCALL_TABLE(264)
DECLARE_OUR_SYSCALL_TABLE(265)
DECLARE_OUR_SYSCALL_TABLE(266)
DECLARE_OUR_SYSCALL_TABLE(267)
DECLARE_OUR_SYSCALL_TABLE(268)
DECLARE_OUR_SYSCALL_TABLE(269)
DECLARE_OUR_SYSCALL_TABLE(270)
DECLARE_OUR_SYSCALL_TABLE(271)
DECLARE_OUR_SYSCALL_TABLE(272)
DECLARE_OUR_SYSCALL_TABLE(273)
DECLARE_OUR_SYSCALL_TABLE(274)
DECLARE_OUR_SYSCALL_TABLE(275)
DECLARE_OUR_SYSCALL_TABLE(276)
DECLARE_OUR_SYSCALL_TABLE(277)
DECLARE_OUR_SYSCALL_TABLE(278)
DECLARE_OUR_SYSCALL_TABLE(279)
DECLARE_OUR_SYSCALL_TABLE(280)
DECLARE_OUR_SYSCALL_TABLE(281)
DECLARE_OUR_SYSCALL_TABLE(282)
DECLARE_OUR_SYSCALL_TABLE(283)
DECLARE_OUR_SYSCALL_TABLE(284)
DECLARE_OUR_SYSCALL_TABLE(285)
DECLARE_OUR_SYSCALL_TABLE(286)
DECLARE_OUR_SYSCALL_TABLE(287)
DECLARE_OUR_SYSCALL_TABLE(288)
DECLARE_OUR_SYSCALL_TABLE(289)
DECLARE_OUR_SYSCALL_TABLE(290)
DECLARE_OUR_SYSCALL_TABLE(291)
DECLARE_OUR_SYSCALL_TABLE(292)
DECLARE_OUR_SYSCALL_TABLE(293)
DECLARE_OUR_SYSCALL_TABLE(294)
DECLARE_OUR_SYSCALL_TABLE(295)
DECLARE_OUR_SYSCALL_TABLE(296)
DECLARE_OUR_SYSCALL_TABLE(297)
DECLARE_OUR_SYSCALL_TABLE(298)
DECLARE_OUR_SYSCALL_TABLE(299)
DECLARE_OUR_SYSCALL_TABLE(300)
DECLARE_OUR_SYSCALL_TABLE(301)
DECLARE_OUR_SYSCALL_TABLE(302)
DECLARE_OUR_SYSCALL_TABLE(303)
DECLARE_OUR_SYSCALL_TABLE(304)
DECLARE_OUR_SYSCALL_TABLE(305)
DECLARE_OUR_SYSCALL_TABLE(306)
DECLARE_OUR_SYSCALL_TABLE(307)
DECLARE_OUR_SYSCALL_TABLE(308)
DECLARE_OUR_SYSCALL_TABLE(309)
DECLARE_OUR_SYSCALL_TABLE(310)
DECLARE_OUR_SYSCALL_TABLE(311)
DECLARE_OUR_SYSCALL_TABLE(312)
DECLARE_OUR_SYSCALL_TABLE(313)
DECLARE_OUR_SYSCALL_TABLE(314)
DECLARE_OUR_SYSCALL_TABLE(315)
DECLARE_OUR_SYSCALL_TABLE(316)
DECLARE_OUR_SYSCALL_TABLE(317)
DECLARE_OUR_SYSCALL_TABLE(318)
DECLARE_OUR_SYSCALL_TABLE(319)
DECLARE_OUR_SYSCALL_TABLE(320)
DECLARE_OUR_SYSCALL_TABLE(321)
DECLARE_OUR_SYSCALL_TABLE(322)
DECLARE_OUR_SYSCALL_TABLE(323)
DECLARE_OUR_SYSCALL_TABLE(324)
DECLARE_OUR_SYSCALL_TABLE(325)
DECLARE_OUR_SYSCALL_TABLE(326)
DECLARE_OUR_SYSCALL_TABLE(327)
DECLARE_OUR_SYSCALL_TABLE(328)
DECLARE_OUR_SYSCALL_TABLE(329)
DECLARE_OUR_SYSCALL_TABLE(330)
DECLARE_OUR_SYSCALL_TABLE(331)
DECLARE_OUR_SYSCALL_TABLE(332)
DECLARE_OUR_SYSCALL_TABLE(333)
DECLARE_OUR_SYSCALL_TABLE(334)
DECLARE_OUR_SYSCALL_TABLE(335)
DECLARE_OUR_SYSCALL_TABLE(336)
DECLARE_OUR_SYSCALL_TABLE(337)
DECLARE_OUR_SYSCALL_TABLE(338)
DECLARE_OUR_SYSCALL_TABLE(339)
DECLARE_OUR_SYSCALL_TABLE(340)
DECLARE_OUR_SYSCALL_TABLE(341)
DECLARE_OUR_SYSCALL_TABLE(342)
DECLARE_OUR_SYSCALL_TABLE(343)
DECLARE_OUR_SYSCALL_TABLE(344)
DECLARE_OUR_SYSCALL_TABLE(345)
DECLARE_OUR_SYSCALL_TABLE(346)
DECLARE_OUR_SYSCALL_TABLE(347)
DECLARE_OUR_SYSCALL_TABLE(348)
DECLARE_OUR_SYSCALL_TABLE(349)
DECLARE_OUR_SYSCALL_TABLE(350)
DECLARE_OUR_SYSCALL_TABLE(351)
DECLARE_OUR_SYSCALL_TABLE(352)
DECLARE_OUR_SYSCALL_TABLE(353)
DECLARE_OUR_SYSCALL_TABLE(354)
DECLARE_OUR_SYSCALL_TABLE(355)
DECLARE_OUR_SYSCALL_TABLE(356)
DECLARE_OUR_SYSCALL_TABLE(357)
DECLARE_OUR_SYSCALL_TABLE(358)
DECLARE_OUR_SYSCALL_TABLE(359)
DECLARE_OUR_SYSCALL_TABLE(360)
DECLARE_OUR_SYSCALL_TABLE(361)
DECLARE_OUR_SYSCALL_TABLE(362)
DECLARE_OUR_SYSCALL_TABLE(363)
DECLARE_OUR_SYSCALL_TABLE(364)
DECLARE_OUR_SYSCALL_TABLE(365)
DECLARE_OUR_SYSCALL_TABLE(366)
DECLARE_OUR_SYSCALL_TABLE(367)
DECLARE_OUR_SYSCALL_TABLE(368)
DECLARE_OUR_SYSCALL_TABLE(369)
DECLARE_OUR_SYSCALL_TABLE(370)
DECLARE_OUR_SYSCALL_TABLE(371)
DECLARE_OUR_SYSCALL_TABLE(372)
DECLARE_OUR_SYSCALL_TABLE(373)
DECLARE_OUR_SYSCALL_TABLE(374)
DECLARE_OUR_SYSCALL_TABLE(375)
DECLARE_OUR_SYSCALL_TABLE(376)
DECLARE_OUR_SYSCALL_TABLE(377)
DECLARE_OUR_SYSCALL_TABLE(378)
DECLARE_OUR_SYSCALL_TABLE(379)
DECLARE_OUR_SYSCALL_TABLE(380)
DECLARE_OUR_SYSCALL_TABLE(381)
DECLARE_OUR_SYSCALL_TABLE(382)
DECLARE_OUR_SYSCALL_TABLE(383)
DECLARE_OUR_SYSCALL_TABLE(384)
DECLARE_OUR_SYSCALL_TABLE(385)
DECLARE_OUR_SYSCALL_TABLE(386)
DECLARE_OUR_SYSCALL_TABLE(387)
DECLARE_OUR_SYSCALL_TABLE(388)
DECLARE_OUR_SYSCALL_TABLE(389)
DECLARE_OUR_SYSCALL_TABLE(390)
DECLARE_OUR_SYSCALL_TABLE(391)
DECLARE_OUR_SYSCALL_TABLE(392)
DECLARE_OUR_SYSCALL_TABLE(393)
DECLARE_OUR_SYSCALL_TABLE(394)
DECLARE_OUR_SYSCALL_TABLE(395)
DECLARE_OUR_SYSCALL_TABLE(396)
DECLARE_OUR_SYSCALL_TABLE(397)
DECLARE_OUR_SYSCALL_TABLE(398)
DECLARE_OUR_SYSCALL_TABLE(399)
DECLARE_OUR_SYSCALL_TABLE(400)
DECLARE_OUR_SYSCALL_TABLE(401)
DECLARE_OUR_SYSCALL_TABLE(402)
DECLARE_OUR_SYSCALL_TABLE(403)
DECLARE_OUR_SYSCALL_TABLE(404)
DECLARE_OUR_SYSCALL_TABLE(405)
DECLARE_OUR_SYSCALL_TABLE(406)
DECLARE_OUR_SYSCALL_TABLE(407)
DECLARE_OUR_SYSCALL_TABLE(408)
DECLARE_OUR_SYSCALL_TABLE(409)
DECLARE_OUR_SYSCALL_TABLE(410)
DECLARE_OUR_SYSCALL_TABLE(411)
DECLARE_OUR_SYSCALL_TABLE(412)
DECLARE_OUR_SYSCALL_TABLE(413)
DECLARE_OUR_SYSCALL_TABLE(414)
DECLARE_OUR_SYSCALL_TABLE(415)
DECLARE_OUR_SYSCALL_TABLE(416)
DECLARE_OUR_SYSCALL_TABLE(417)
DECLARE_OUR_SYSCALL_TABLE(418)
DECLARE_OUR_SYSCALL_TABLE(419)
DECLARE_OUR_SYSCALL_TABLE(420)
DECLARE_OUR_SYSCALL_TABLE(421)
DECLARE_OUR_SYSCALL_TABLE(422)
DECLARE_OUR_SYSCALL_TABLE(423)
DECLARE_OUR_SYSCALL_TABLE(424)
DECLARE_OUR_SYSCALL_TABLE(425)
DECLARE_OUR_SYSCALL_TABLE(426)
DECLARE_OUR_SYSCALL_TABLE(427)
DECLARE_OUR_SYSCALL_TABLE(428)
DECLARE_OUR_SYSCALL_TABLE(429)
DECLARE_OUR_SYSCALL_TABLE(430)
DECLARE_OUR_SYSCALL_TABLE(431)
DECLARE_OUR_SYSCALL_TABLE(432)
DECLARE_OUR_SYSCALL_TABLE(433)
DECLARE_OUR_SYSCALL_TABLE(434)
DECLARE_OUR_SYSCALL_TABLE(435)
DECLARE_OUR_SYSCALL_TABLE(436)
DECLARE_OUR_SYSCALL_TABLE(437)
DECLARE_OUR_SYSCALL_TABLE(438)
DECLARE_OUR_SYSCALL_TABLE(439)

#undef DECLARE_OUR_SYSCALL_TABLE

#define ASSIGN_OUR_SYSCALL_TABLE(number)                       \
    {                                                          \
        our_syscall_table[number] = (void *)NEW_FUNC_##number; \
    }

static void gen_our_syscall(void)
{
    if (our_syscall_table[0] != NULL)
        return;

    ASSIGN_OUR_SYSCALL_TABLE(0);
    ASSIGN_OUR_SYSCALL_TABLE(1);
    ASSIGN_OUR_SYSCALL_TABLE(2);
    ASSIGN_OUR_SYSCALL_TABLE(3);
    ASSIGN_OUR_SYSCALL_TABLE(4);
    ASSIGN_OUR_SYSCALL_TABLE(5);
    ASSIGN_OUR_SYSCALL_TABLE(6);
    ASSIGN_OUR_SYSCALL_TABLE(7);
    ASSIGN_OUR_SYSCALL_TABLE(8);
    ASSIGN_OUR_SYSCALL_TABLE(9);
    ASSIGN_OUR_SYSCALL_TABLE(10);
    ASSIGN_OUR_SYSCALL_TABLE(11);
    ASSIGN_OUR_SYSCALL_TABLE(12);
    ASSIGN_OUR_SYSCALL_TABLE(13);
    ASSIGN_OUR_SYSCALL_TABLE(14);
    ASSIGN_OUR_SYSCALL_TABLE(15);
    ASSIGN_OUR_SYSCALL_TABLE(16);
    ASSIGN_OUR_SYSCALL_TABLE(17);
    ASSIGN_OUR_SYSCALL_TABLE(18);
    ASSIGN_OUR_SYSCALL_TABLE(19);
    ASSIGN_OUR_SYSCALL_TABLE(20);
    ASSIGN_OUR_SYSCALL_TABLE(21);
    ASSIGN_OUR_SYSCALL_TABLE(22);
    ASSIGN_OUR_SYSCALL_TABLE(23);
    ASSIGN_OUR_SYSCALL_TABLE(24);
    ASSIGN_OUR_SYSCALL_TABLE(25);
    ASSIGN_OUR_SYSCALL_TABLE(26);
    ASSIGN_OUR_SYSCALL_TABLE(27);
    ASSIGN_OUR_SYSCALL_TABLE(28);
    ASSIGN_OUR_SYSCALL_TABLE(29);
    ASSIGN_OUR_SYSCALL_TABLE(30);
    ASSIGN_OUR_SYSCALL_TABLE(31);
    ASSIGN_OUR_SYSCALL_TABLE(32);
    ASSIGN_OUR_SYSCALL_TABLE(33);
    ASSIGN_OUR_SYSCALL_TABLE(34);
    ASSIGN_OUR_SYSCALL_TABLE(35);
    ASSIGN_OUR_SYSCALL_TABLE(36);
    ASSIGN_OUR_SYSCALL_TABLE(37);
    ASSIGN_OUR_SYSCALL_TABLE(38);
    ASSIGN_OUR_SYSCALL_TABLE(39);
    ASSIGN_OUR_SYSCALL_TABLE(40);
    ASSIGN_OUR_SYSCALL_TABLE(41);
    ASSIGN_OUR_SYSCALL_TABLE(42);
    ASSIGN_OUR_SYSCALL_TABLE(43);
    ASSIGN_OUR_SYSCALL_TABLE(44);
    ASSIGN_OUR_SYSCALL_TABLE(45);
    ASSIGN_OUR_SYSCALL_TABLE(46);
    ASSIGN_OUR_SYSCALL_TABLE(47);
    ASSIGN_OUR_SYSCALL_TABLE(48);
    ASSIGN_OUR_SYSCALL_TABLE(49);
    ASSIGN_OUR_SYSCALL_TABLE(50);
    ASSIGN_OUR_SYSCALL_TABLE(51);
    ASSIGN_OUR_SYSCALL_TABLE(52);
    ASSIGN_OUR_SYSCALL_TABLE(53);
    ASSIGN_OUR_SYSCALL_TABLE(54);
    ASSIGN_OUR_SYSCALL_TABLE(55);
    ASSIGN_OUR_SYSCALL_TABLE(56);
    ASSIGN_OUR_SYSCALL_TABLE(57);
    ASSIGN_OUR_SYSCALL_TABLE(58);
    ASSIGN_OUR_SYSCALL_TABLE(59);
    ASSIGN_OUR_SYSCALL_TABLE(60);
    ASSIGN_OUR_SYSCALL_TABLE(61);
    ASSIGN_OUR_SYSCALL_TABLE(62);
    ASSIGN_OUR_SYSCALL_TABLE(63);
    ASSIGN_OUR_SYSCALL_TABLE(64);
    ASSIGN_OUR_SYSCALL_TABLE(65);
    ASSIGN_OUR_SYSCALL_TABLE(66);
    ASSIGN_OUR_SYSCALL_TABLE(67);
    ASSIGN_OUR_SYSCALL_TABLE(68);
    ASSIGN_OUR_SYSCALL_TABLE(69);
    ASSIGN_OUR_SYSCALL_TABLE(70);
    ASSIGN_OUR_SYSCALL_TABLE(71);
    ASSIGN_OUR_SYSCALL_TABLE(72);
    ASSIGN_OUR_SYSCALL_TABLE(73);
    ASSIGN_OUR_SYSCALL_TABLE(74);
    ASSIGN_OUR_SYSCALL_TABLE(75);
    ASSIGN_OUR_SYSCALL_TABLE(76);
    ASSIGN_OUR_SYSCALL_TABLE(77);
    ASSIGN_OUR_SYSCALL_TABLE(78);
    ASSIGN_OUR_SYSCALL_TABLE(79);
    ASSIGN_OUR_SYSCALL_TABLE(80);
    ASSIGN_OUR_SYSCALL_TABLE(81);
    ASSIGN_OUR_SYSCALL_TABLE(82);
    ASSIGN_OUR_SYSCALL_TABLE(83);
    ASSIGN_OUR_SYSCALL_TABLE(84);
    ASSIGN_OUR_SYSCALL_TABLE(85);
    ASSIGN_OUR_SYSCALL_TABLE(86);
    ASSIGN_OUR_SYSCALL_TABLE(87);
    ASSIGN_OUR_SYSCALL_TABLE(88);
    ASSIGN_OUR_SYSCALL_TABLE(89);
    ASSIGN_OUR_SYSCALL_TABLE(90);
    ASSIGN_OUR_SYSCALL_TABLE(91);
    ASSIGN_OUR_SYSCALL_TABLE(92);
    ASSIGN_OUR_SYSCALL_TABLE(93);
    ASSIGN_OUR_SYSCALL_TABLE(94);
    ASSIGN_OUR_SYSCALL_TABLE(95);
    ASSIGN_OUR_SYSCALL_TABLE(96);
    ASSIGN_OUR_SYSCALL_TABLE(97);
    ASSIGN_OUR_SYSCALL_TABLE(98);
    ASSIGN_OUR_SYSCALL_TABLE(99);
    ASSIGN_OUR_SYSCALL_TABLE(100);
    ASSIGN_OUR_SYSCALL_TABLE(101);
    ASSIGN_OUR_SYSCALL_TABLE(102);
    ASSIGN_OUR_SYSCALL_TABLE(103);
    ASSIGN_OUR_SYSCALL_TABLE(104);
    ASSIGN_OUR_SYSCALL_TABLE(105);
    ASSIGN_OUR_SYSCALL_TABLE(106);
    ASSIGN_OUR_SYSCALL_TABLE(107);
    ASSIGN_OUR_SYSCALL_TABLE(108);
    ASSIGN_OUR_SYSCALL_TABLE(109);
    ASSIGN_OUR_SYSCALL_TABLE(110);
    ASSIGN_OUR_SYSCALL_TABLE(111);
    ASSIGN_OUR_SYSCALL_TABLE(112);
    ASSIGN_OUR_SYSCALL_TABLE(113);
    ASSIGN_OUR_SYSCALL_TABLE(114);
    ASSIGN_OUR_SYSCALL_TABLE(115);
    ASSIGN_OUR_SYSCALL_TABLE(116);
    ASSIGN_OUR_SYSCALL_TABLE(117);
    ASSIGN_OUR_SYSCALL_TABLE(118);
    ASSIGN_OUR_SYSCALL_TABLE(119);
    ASSIGN_OUR_SYSCALL_TABLE(120);
    ASSIGN_OUR_SYSCALL_TABLE(121);
    ASSIGN_OUR_SYSCALL_TABLE(122);
    ASSIGN_OUR_SYSCALL_TABLE(123);
    ASSIGN_OUR_SYSCALL_TABLE(124);
    ASSIGN_OUR_SYSCALL_TABLE(125);
    ASSIGN_OUR_SYSCALL_TABLE(126);
    ASSIGN_OUR_SYSCALL_TABLE(127);
    ASSIGN_OUR_SYSCALL_TABLE(128);
    ASSIGN_OUR_SYSCALL_TABLE(129);
    ASSIGN_OUR_SYSCALL_TABLE(130);
    ASSIGN_OUR_SYSCALL_TABLE(131);
    ASSIGN_OUR_SYSCALL_TABLE(132);
    ASSIGN_OUR_SYSCALL_TABLE(133);
    ASSIGN_OUR_SYSCALL_TABLE(134);
    ASSIGN_OUR_SYSCALL_TABLE(135);
    ASSIGN_OUR_SYSCALL_TABLE(136);
    ASSIGN_OUR_SYSCALL_TABLE(137);
    ASSIGN_OUR_SYSCALL_TABLE(138);
    ASSIGN_OUR_SYSCALL_TABLE(139);
    ASSIGN_OUR_SYSCALL_TABLE(140);
    ASSIGN_OUR_SYSCALL_TABLE(141);
    ASSIGN_OUR_SYSCALL_TABLE(142);
    ASSIGN_OUR_SYSCALL_TABLE(143);
    ASSIGN_OUR_SYSCALL_TABLE(144);
    ASSIGN_OUR_SYSCALL_TABLE(145);
    ASSIGN_OUR_SYSCALL_TABLE(146);
    ASSIGN_OUR_SYSCALL_TABLE(147);
    ASSIGN_OUR_SYSCALL_TABLE(148);
    ASSIGN_OUR_SYSCALL_TABLE(149);
    ASSIGN_OUR_SYSCALL_TABLE(150);
    ASSIGN_OUR_SYSCALL_TABLE(151);
    ASSIGN_OUR_SYSCALL_TABLE(152);
    ASSIGN_OUR_SYSCALL_TABLE(153);
    ASSIGN_OUR_SYSCALL_TABLE(154);
    ASSIGN_OUR_SYSCALL_TABLE(155);
    ASSIGN_OUR_SYSCALL_TABLE(156);
    ASSIGN_OUR_SYSCALL_TABLE(157);
    ASSIGN_OUR_SYSCALL_TABLE(158);
    ASSIGN_OUR_SYSCALL_TABLE(159);
    ASSIGN_OUR_SYSCALL_TABLE(160);
    ASSIGN_OUR_SYSCALL_TABLE(161);
    ASSIGN_OUR_SYSCALL_TABLE(162);
    ASSIGN_OUR_SYSCALL_TABLE(163);
    ASSIGN_OUR_SYSCALL_TABLE(164);
    ASSIGN_OUR_SYSCALL_TABLE(165);
    ASSIGN_OUR_SYSCALL_TABLE(166);
    ASSIGN_OUR_SYSCALL_TABLE(167);
    ASSIGN_OUR_SYSCALL_TABLE(168);
    ASSIGN_OUR_SYSCALL_TABLE(169);
    ASSIGN_OUR_SYSCALL_TABLE(170);
    ASSIGN_OUR_SYSCALL_TABLE(171);
    ASSIGN_OUR_SYSCALL_TABLE(172);
    ASSIGN_OUR_SYSCALL_TABLE(173);
    ASSIGN_OUR_SYSCALL_TABLE(174);
    ASSIGN_OUR_SYSCALL_TABLE(175);
    ASSIGN_OUR_SYSCALL_TABLE(176);
    ASSIGN_OUR_SYSCALL_TABLE(177);
    ASSIGN_OUR_SYSCALL_TABLE(178);
    ASSIGN_OUR_SYSCALL_TABLE(179);
    ASSIGN_OUR_SYSCALL_TABLE(180);
    ASSIGN_OUR_SYSCALL_TABLE(181);
    ASSIGN_OUR_SYSCALL_TABLE(182);
    ASSIGN_OUR_SYSCALL_TABLE(183);
    ASSIGN_OUR_SYSCALL_TABLE(184);
    ASSIGN_OUR_SYSCALL_TABLE(185);
    ASSIGN_OUR_SYSCALL_TABLE(186);
    ASSIGN_OUR_SYSCALL_TABLE(187);
    ASSIGN_OUR_SYSCALL_TABLE(188);
    ASSIGN_OUR_SYSCALL_TABLE(189);
    ASSIGN_OUR_SYSCALL_TABLE(190);
    ASSIGN_OUR_SYSCALL_TABLE(191);
    ASSIGN_OUR_SYSCALL_TABLE(192);
    ASSIGN_OUR_SYSCALL_TABLE(193);
    ASSIGN_OUR_SYSCALL_TABLE(194);
    ASSIGN_OUR_SYSCALL_TABLE(195);
    ASSIGN_OUR_SYSCALL_TABLE(196);
    ASSIGN_OUR_SYSCALL_TABLE(197);
    ASSIGN_OUR_SYSCALL_TABLE(198);
    ASSIGN_OUR_SYSCALL_TABLE(199);
    ASSIGN_OUR_SYSCALL_TABLE(200);
    ASSIGN_OUR_SYSCALL_TABLE(201);
    ASSIGN_OUR_SYSCALL_TABLE(202);
    ASSIGN_OUR_SYSCALL_TABLE(203);
    ASSIGN_OUR_SYSCALL_TABLE(204);
    ASSIGN_OUR_SYSCALL_TABLE(205);
    ASSIGN_OUR_SYSCALL_TABLE(206);
    ASSIGN_OUR_SYSCALL_TABLE(207);
    ASSIGN_OUR_SYSCALL_TABLE(208);
    ASSIGN_OUR_SYSCALL_TABLE(209);
    ASSIGN_OUR_SYSCALL_TABLE(210);
    ASSIGN_OUR_SYSCALL_TABLE(211);
    ASSIGN_OUR_SYSCALL_TABLE(212);
    ASSIGN_OUR_SYSCALL_TABLE(213);
    ASSIGN_OUR_SYSCALL_TABLE(214);
    ASSIGN_OUR_SYSCALL_TABLE(215);
    ASSIGN_OUR_SYSCALL_TABLE(216);
    ASSIGN_OUR_SYSCALL_TABLE(217);
    ASSIGN_OUR_SYSCALL_TABLE(218);
    ASSIGN_OUR_SYSCALL_TABLE(219);
    ASSIGN_OUR_SYSCALL_TABLE(220);
    ASSIGN_OUR_SYSCALL_TABLE(221);
    ASSIGN_OUR_SYSCALL_TABLE(222);
    ASSIGN_OUR_SYSCALL_TABLE(223);
    ASSIGN_OUR_SYSCALL_TABLE(224);
    ASSIGN_OUR_SYSCALL_TABLE(225);
    ASSIGN_OUR_SYSCALL_TABLE(226);
    ASSIGN_OUR_SYSCALL_TABLE(227);
    ASSIGN_OUR_SYSCALL_TABLE(228);
    ASSIGN_OUR_SYSCALL_TABLE(229);
    ASSIGN_OUR_SYSCALL_TABLE(230);
    ASSIGN_OUR_SYSCALL_TABLE(231);
    ASSIGN_OUR_SYSCALL_TABLE(232);
    ASSIGN_OUR_SYSCALL_TABLE(233);
    ASSIGN_OUR_SYSCALL_TABLE(234);
    ASSIGN_OUR_SYSCALL_TABLE(235);
    ASSIGN_OUR_SYSCALL_TABLE(236);
    ASSIGN_OUR_SYSCALL_TABLE(237);
    ASSIGN_OUR_SYSCALL_TABLE(238);
    ASSIGN_OUR_SYSCALL_TABLE(239);
    ASSIGN_OUR_SYSCALL_TABLE(240);
    ASSIGN_OUR_SYSCALL_TABLE(241);
    ASSIGN_OUR_SYSCALL_TABLE(242);
    ASSIGN_OUR_SYSCALL_TABLE(243);
    ASSIGN_OUR_SYSCALL_TABLE(244);
    ASSIGN_OUR_SYSCALL_TABLE(245);
    ASSIGN_OUR_SYSCALL_TABLE(246);
    ASSIGN_OUR_SYSCALL_TABLE(247);
    ASSIGN_OUR_SYSCALL_TABLE(248);
    ASSIGN_OUR_SYSCALL_TABLE(249);
    ASSIGN_OUR_SYSCALL_TABLE(250);
    ASSIGN_OUR_SYSCALL_TABLE(251);
    ASSIGN_OUR_SYSCALL_TABLE(252);
    ASSIGN_OUR_SYSCALL_TABLE(253);
    ASSIGN_OUR_SYSCALL_TABLE(254);
    ASSIGN_OUR_SYSCALL_TABLE(255);
    ASSIGN_OUR_SYSCALL_TABLE(256);
    ASSIGN_OUR_SYSCALL_TABLE(257);
    ASSIGN_OUR_SYSCALL_TABLE(258);
    ASSIGN_OUR_SYSCALL_TABLE(259);
    ASSIGN_OUR_SYSCALL_TABLE(260);
    ASSIGN_OUR_SYSCALL_TABLE(261);
    ASSIGN_OUR_SYSCALL_TABLE(262);
    ASSIGN_OUR_SYSCALL_TABLE(263);
    ASSIGN_OUR_SYSCALL_TABLE(264);
    ASSIGN_OUR_SYSCALL_TABLE(265);
    ASSIGN_OUR_SYSCALL_TABLE(266);
    ASSIGN_OUR_SYSCALL_TABLE(267);
    ASSIGN_OUR_SYSCALL_TABLE(268);
    ASSIGN_OUR_SYSCALL_TABLE(269);
    ASSIGN_OUR_SYSCALL_TABLE(270);
    ASSIGN_OUR_SYSCALL_TABLE(271);
    ASSIGN_OUR_SYSCALL_TABLE(272);
    ASSIGN_OUR_SYSCALL_TABLE(273);
    ASSIGN_OUR_SYSCALL_TABLE(274);
    ASSIGN_OUR_SYSCALL_TABLE(275);
    ASSIGN_OUR_SYSCALL_TABLE(276);
    ASSIGN_OUR_SYSCALL_TABLE(277);
    ASSIGN_OUR_SYSCALL_TABLE(278);
    ASSIGN_OUR_SYSCALL_TABLE(279);
    ASSIGN_OUR_SYSCALL_TABLE(280);
    ASSIGN_OUR_SYSCALL_TABLE(281);
    ASSIGN_OUR_SYSCALL_TABLE(282);
    ASSIGN_OUR_SYSCALL_TABLE(283);
    ASSIGN_OUR_SYSCALL_TABLE(284);
    ASSIGN_OUR_SYSCALL_TABLE(285);
    ASSIGN_OUR_SYSCALL_TABLE(286);
    ASSIGN_OUR_SYSCALL_TABLE(287);
    ASSIGN_OUR_SYSCALL_TABLE(288);
    ASSIGN_OUR_SYSCALL_TABLE(289);
    ASSIGN_OUR_SYSCALL_TABLE(290);
    ASSIGN_OUR_SYSCALL_TABLE(291);
    ASSIGN_OUR_SYSCALL_TABLE(292);
    ASSIGN_OUR_SYSCALL_TABLE(293);
    ASSIGN_OUR_SYSCALL_TABLE(294);
    ASSIGN_OUR_SYSCALL_TABLE(295);
    ASSIGN_OUR_SYSCALL_TABLE(296);
    ASSIGN_OUR_SYSCALL_TABLE(297);
    ASSIGN_OUR_SYSCALL_TABLE(298);
    ASSIGN_OUR_SYSCALL_TABLE(299);
    ASSIGN_OUR_SYSCALL_TABLE(300);
    ASSIGN_OUR_SYSCALL_TABLE(301);
    ASSIGN_OUR_SYSCALL_TABLE(302);
    ASSIGN_OUR_SYSCALL_TABLE(303);
    ASSIGN_OUR_SYSCALL_TABLE(304);
    ASSIGN_OUR_SYSCALL_TABLE(305);
    ASSIGN_OUR_SYSCALL_TABLE(306);
    ASSIGN_OUR_SYSCALL_TABLE(307);
    ASSIGN_OUR_SYSCALL_TABLE(308);
    ASSIGN_OUR_SYSCALL_TABLE(309);
    ASSIGN_OUR_SYSCALL_TABLE(310);
    ASSIGN_OUR_SYSCALL_TABLE(311);
    ASSIGN_OUR_SYSCALL_TABLE(312);
    ASSIGN_OUR_SYSCALL_TABLE(313);
    ASSIGN_OUR_SYSCALL_TABLE(314);
    ASSIGN_OUR_SYSCALL_TABLE(315);
    ASSIGN_OUR_SYSCALL_TABLE(316);
    ASSIGN_OUR_SYSCALL_TABLE(317);
    ASSIGN_OUR_SYSCALL_TABLE(318);
    ASSIGN_OUR_SYSCALL_TABLE(319);
    ASSIGN_OUR_SYSCALL_TABLE(320);
    ASSIGN_OUR_SYSCALL_TABLE(321);
    ASSIGN_OUR_SYSCALL_TABLE(322);
    ASSIGN_OUR_SYSCALL_TABLE(323);
    ASSIGN_OUR_SYSCALL_TABLE(324);
    ASSIGN_OUR_SYSCALL_TABLE(325);
    ASSIGN_OUR_SYSCALL_TABLE(326);
    ASSIGN_OUR_SYSCALL_TABLE(327);
    ASSIGN_OUR_SYSCALL_TABLE(328);
    ASSIGN_OUR_SYSCALL_TABLE(329);
    ASSIGN_OUR_SYSCALL_TABLE(330);
    ASSIGN_OUR_SYSCALL_TABLE(331);
    ASSIGN_OUR_SYSCALL_TABLE(332);
    ASSIGN_OUR_SYSCALL_TABLE(333);
    ASSIGN_OUR_SYSCALL_TABLE(334);
    ASSIGN_OUR_SYSCALL_TABLE(335);
    ASSIGN_OUR_SYSCALL_TABLE(336);
    ASSIGN_OUR_SYSCALL_TABLE(337);
    ASSIGN_OUR_SYSCALL_TABLE(338);
    ASSIGN_OUR_SYSCALL_TABLE(339);
    ASSIGN_OUR_SYSCALL_TABLE(340);
    ASSIGN_OUR_SYSCALL_TABLE(341);
    ASSIGN_OUR_SYSCALL_TABLE(342);
    ASSIGN_OUR_SYSCALL_TABLE(343);
    ASSIGN_OUR_SYSCALL_TABLE(344);
    ASSIGN_OUR_SYSCALL_TABLE(345);
    ASSIGN_OUR_SYSCALL_TABLE(346);
    ASSIGN_OUR_SYSCALL_TABLE(347);
    ASSIGN_OUR_SYSCALL_TABLE(348);
    ASSIGN_OUR_SYSCALL_TABLE(349);
    ASSIGN_OUR_SYSCALL_TABLE(350);
    ASSIGN_OUR_SYSCALL_TABLE(351);
    ASSIGN_OUR_SYSCALL_TABLE(352);
    ASSIGN_OUR_SYSCALL_TABLE(353);
    ASSIGN_OUR_SYSCALL_TABLE(354);
    ASSIGN_OUR_SYSCALL_TABLE(355);
    ASSIGN_OUR_SYSCALL_TABLE(356);
    ASSIGN_OUR_SYSCALL_TABLE(357);
    ASSIGN_OUR_SYSCALL_TABLE(358);
    ASSIGN_OUR_SYSCALL_TABLE(359);
    ASSIGN_OUR_SYSCALL_TABLE(360);
    ASSIGN_OUR_SYSCALL_TABLE(361);
    ASSIGN_OUR_SYSCALL_TABLE(362);
    ASSIGN_OUR_SYSCALL_TABLE(363);
    ASSIGN_OUR_SYSCALL_TABLE(364);
    ASSIGN_OUR_SYSCALL_TABLE(365);
    ASSIGN_OUR_SYSCALL_TABLE(366);
    ASSIGN_OUR_SYSCALL_TABLE(367);
    ASSIGN_OUR_SYSCALL_TABLE(368);
    ASSIGN_OUR_SYSCALL_TABLE(369);
    ASSIGN_OUR_SYSCALL_TABLE(370);
    ASSIGN_OUR_SYSCALL_TABLE(371);
    ASSIGN_OUR_SYSCALL_TABLE(372);
    ASSIGN_OUR_SYSCALL_TABLE(373);
    ASSIGN_OUR_SYSCALL_TABLE(374);
    ASSIGN_OUR_SYSCALL_TABLE(375);
    ASSIGN_OUR_SYSCALL_TABLE(376);
    ASSIGN_OUR_SYSCALL_TABLE(377);
    ASSIGN_OUR_SYSCALL_TABLE(378);
    ASSIGN_OUR_SYSCALL_TABLE(379);
    ASSIGN_OUR_SYSCALL_TABLE(380);
    ASSIGN_OUR_SYSCALL_TABLE(381);
    ASSIGN_OUR_SYSCALL_TABLE(382);
    ASSIGN_OUR_SYSCALL_TABLE(383);
    ASSIGN_OUR_SYSCALL_TABLE(384);
    ASSIGN_OUR_SYSCALL_TABLE(385);
    ASSIGN_OUR_SYSCALL_TABLE(386);
    ASSIGN_OUR_SYSCALL_TABLE(387);
    ASSIGN_OUR_SYSCALL_TABLE(388);
    ASSIGN_OUR_SYSCALL_TABLE(389);
    ASSIGN_OUR_SYSCALL_TABLE(390);
    ASSIGN_OUR_SYSCALL_TABLE(391);
    ASSIGN_OUR_SYSCALL_TABLE(392);
    ASSIGN_OUR_SYSCALL_TABLE(393);
    ASSIGN_OUR_SYSCALL_TABLE(394);
    ASSIGN_OUR_SYSCALL_TABLE(395);
    ASSIGN_OUR_SYSCALL_TABLE(396);
    ASSIGN_OUR_SYSCALL_TABLE(397);
    ASSIGN_OUR_SYSCALL_TABLE(398);
    ASSIGN_OUR_SYSCALL_TABLE(399);
    ASSIGN_OUR_SYSCALL_TABLE(400);
    ASSIGN_OUR_SYSCALL_TABLE(401);
    ASSIGN_OUR_SYSCALL_TABLE(402);
    ASSIGN_OUR_SYSCALL_TABLE(403);
    ASSIGN_OUR_SYSCALL_TABLE(404);
    ASSIGN_OUR_SYSCALL_TABLE(405);
    ASSIGN_OUR_SYSCALL_TABLE(406);
    ASSIGN_OUR_SYSCALL_TABLE(407);
    ASSIGN_OUR_SYSCALL_TABLE(408);
    ASSIGN_OUR_SYSCALL_TABLE(409);
    ASSIGN_OUR_SYSCALL_TABLE(410);
    ASSIGN_OUR_SYSCALL_TABLE(411);
    ASSIGN_OUR_SYSCALL_TABLE(412);
    ASSIGN_OUR_SYSCALL_TABLE(413);
    ASSIGN_OUR_SYSCALL_TABLE(414);
    ASSIGN_OUR_SYSCALL_TABLE(415);
    ASSIGN_OUR_SYSCALL_TABLE(416);
    ASSIGN_OUR_SYSCALL_TABLE(417);
    ASSIGN_OUR_SYSCALL_TABLE(418);
    ASSIGN_OUR_SYSCALL_TABLE(419);
    ASSIGN_OUR_SYSCALL_TABLE(420);
    ASSIGN_OUR_SYSCALL_TABLE(421);
    ASSIGN_OUR_SYSCALL_TABLE(422);
    ASSIGN_OUR_SYSCALL_TABLE(423);
    ASSIGN_OUR_SYSCALL_TABLE(424);
    ASSIGN_OUR_SYSCALL_TABLE(425);
    ASSIGN_OUR_SYSCALL_TABLE(426);
    ASSIGN_OUR_SYSCALL_TABLE(427);
    ASSIGN_OUR_SYSCALL_TABLE(428);
    ASSIGN_OUR_SYSCALL_TABLE(429);
    ASSIGN_OUR_SYSCALL_TABLE(430);
    ASSIGN_OUR_SYSCALL_TABLE(431);
    ASSIGN_OUR_SYSCALL_TABLE(432);
    ASSIGN_OUR_SYSCALL_TABLE(433);
    ASSIGN_OUR_SYSCALL_TABLE(434);
    ASSIGN_OUR_SYSCALL_TABLE(435);
    ASSIGN_OUR_SYSCALL_TABLE(436);
    ASSIGN_OUR_SYSCALL_TABLE(437);
    ASSIGN_OUR_SYSCALL_TABLE(438);
    ASSIGN_OUR_SYSCALL_TABLE(439);
}

#undef ASSIGN_OUR_SYSCALL_TABLE
