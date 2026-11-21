/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * monitor.c - the magia> kernel debug monitor on the serial console.
 *
 * A line-oriented shell that runs as the boot thread after init. It is a
 * debugging aid, not a userland: commands inspect kernel state directly
 * (threads, memory) or through the Rust debug helpers (processes, jails,
 * sysctl, files).
 */
#include <magia/acpi.h>
#include <magia/cpu.h>
#include <magia/fbcon.h>
#include <magia/hwinfo.h>
#include <magia/kbd.h>
#include <magia/klog.h>
#include <magia/ktest.h>
#include <magia/mm.h>
#include <magia/pci.h>
#include <magia/usb.h>
#include <magia/rust.h>
#include <magia/syscall.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define MAX_ARGS 4

int monitor_exec(char *line);
void monitor_main(void);

static void cmd_help(int argc, char **argv);

static void cmd_uname(int argc, char **argv) { rs_dbg_uname(); }
static void cmd_ps(int argc, char **argv) { rs_dbg_ps(); }
static void cmd_threads(int argc, char **argv) { thread_list(); }
static void cmd_jails(int argc, char **argv) { rs_dbg_jails(); }

static void cmd_mem(int argc, char **argv)
{
	pmm_dump();
	kmem_dump();
}

static void cmd_uptime(int argc, char **argv)
{
	kprintf("up %lu.%03lu s, %lu context switches, %lu syscalls, %lu timer IRQs\n", ticks / 1000,
		ticks % 1000, context_switches, syscall_count, irq_counts[IRQ_TIMER]);
}

static void cmd_cpu(int argc, char **argv) { cpu_report(); }

static void cmd_ls(int argc, char **argv)
{
	const char *p = argc > 1 ? argv[1] : "/";
	rs_dbg_ls(p, strlen(p));
}

static void cmd_cat(int argc, char **argv)
{
	if (argc < 2) {
		kprintf("usage: cat <path>\n");
		return;
	}
	rs_dbg_cat(argv[1], strlen(argv[1]));
}

static void cmd_sysctl(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";
	const char *val = argc > 2 ? argv[2] : "";
	rs_dbg_sysctl(name, strlen(name), val, strlen(val));
}

static void cmd_echo(int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
		kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
	kprintf("\n");
}

static void cmd_bt(int argc, char **argv) { backtrace_from(read_rbp(), 0); }
static void cmd_reboot(int argc, char **argv) { acpi_reboot(); }
static void cmd_poweroff(int argc, char **argv) { acpi_poweroff(); }

static void cmd_panic(int argc, char **argv) { panic("requested from the monitor"); }
static void cmd_lspci(int argc, char **argv) { pci_dump(); }
static void cmd_usb(int argc, char **argv) { xhci_dump(); }
static void cmd_hwinfo(int argc, char **argv) { hwinfo(); }

static void cmd_dmesg(int argc, char **argv)
{
	bool ok = true;
	long n = argc > 1 ? katol(argv[1], &ok) : 0;
	if (!ok || n < 0) {
		kprintf("usage: dmesg [lines]\n");
		return;
	}
	klog_dump_tail((unsigned)MIN(n, 100000L));
}

static const struct {
	const char *name;
	void (*fn)(int, char **);
	const char *help;
} cmds[] = {
	{ "help", cmd_help, "list commands" },
	{ "uname", cmd_uname, "system identification" },
	{ "cpu", cmd_cpu, "CPU features and protections" },
	{ "ps", cmd_ps, "process table (Rust)" },
	{ "threads", cmd_threads, "kernel threads" },
	{ "jails", cmd_jails, "jail tree with resource accounting" },
	{ "mem", cmd_mem, "page allocator and kmalloc statistics" },
	{ "uptime", cmd_uptime, "time, switches, syscalls" },
	{ "ls", cmd_ls, "ls [path]: list a directory" },
	{ "cat", cmd_cat, "cat <path>: print a file" },
	{ "sysctl", cmd_sysctl, "sysctl [name [value]]: read/list/set MIB nodes" },
	{ "echo", cmd_echo, "echo words" },
	{ "lspci", cmd_lspci, "PCI/PCIe functions (ECAM)" },
	{ "usb", cmd_usb, "xHCI ports and the USB keyboard" },
	{ "hwinfo", cmd_hwinfo, "PCI and USB IDs on one screen (photograph it)" },
	{ "dmesg", cmd_dmesg, "dmesg [n]: the kernel log (Page Up/Down scroll)" },
	{ "bt", cmd_bt, "backtrace of the monitor thread" },
	{ "panic", cmd_panic, "panic the kernel (test the panic path)" },
	{ "reboot", cmd_reboot, "reset the machine (ACPI reset register, 8042, triple fault)" },
	{ "poweroff", cmd_poweroff, "switch off (ACPI S5)" },
};

static void cmd_help(int argc, char **argv)
{
	for (size_t i = 0; i < ARRAY_SIZE(cmds); i++)
		kprintf("  %-9s %s\n", cmds[i].name, cmds[i].help);
}

/* Returns 0 if the command exists, -1 otherwise. Modifies `line`. */
int monitor_exec(char *line)
{
	char *argv[MAX_ARGS + 1];
	int argc = 0;
	for (char *p = line; *p && argc < MAX_ARGS;) {
		while (*p == ' ' || *p == '\t')
			*p++ = '\0';
		if (!*p)
			break;
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	argv[argc] = NULL;
	if (argc == 0)
		return 0;
	for (size_t i = 0; i < ARRAY_SIZE(cmds); i++)
		if (strcmp(argv[0], cmds[i].name) == 0) {
			cmds[i].fn(argc, argv);
			return 0;
		}
	kprintf("unknown command '%s' (try help)\n", argv[0]);
	return -1;
}

static size_t read_line(char *buf, size_t size)
{
	static struct vt_dec vt;
	size_t n = 0;
	for (;;) {
		int c = vt_decode(&vt, console_getc_wait());
		if (c == VT_PGUP || c == VT_PGDN) { /* scroll the screen by a page */
			int page = fbcon_rows > 1 ? fbcon_rows - 1 : 1;
			fbcon_scroll(c == VT_PGUP ? page : -page);
			continue;
		}
		if (c < 0)
			continue;
		if (c == '\r' || c == '\n') {
			console_write("\n", 1);
			buf[n] = '\0';
			return n;
		}
		if (c == 0x7f || c == '\b') {
			if (n > 0) {
				n--;
				console_write("\b \b", 3);
			}
			continue;
		}
		if (c == 0x15) { /* ^U: kill line */
			while (n > 0) {
				n--;
				console_write("\b \b", 3);
			}
			continue;
		}
		if (c < 0x20 || c > 0x7e || n + 1 >= size)
			continue;
		buf[n++] = (char)c;
		char ch = (char)c;
		console_write(&ch, 1);
	}
}

void monitor_main(void)
{
	kprintf("Type 'help' for monitor commands.\n");
	char line[128];
	for (;;) {
		kprintf("magia> ");
		read_line(line, sizeof(line));
		monitor_exec(line);
	}
}
