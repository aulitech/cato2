/*
 * SPDX-License-Identifier: MIT
 *
 * Fatal error handling: record what crashed in RAM that survives the reset,
 * restart, and report it in the log at the next boot. Without this a crash
 * leaves the board frozen until the reset button is pressed.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include "app.h"

LOG_MODULE_REGISTER(crash, LOG_LEVEL_INF);

#define CRASH_MAGIC 0xc4a54ed1

/*
 * Kept high in RAM, clear of the memory that this firmware, MCUboot and the
 * UF2 bootloader use, so the record survives the trip through both loaders.
 */
#define CRASH_RECORD ((struct crash_record *)0x20030000)

struct crash_record {
	uint32_t magic;
	uint32_t reason;
	uint32_t pc;
	uint32_t lr;
	char thread[16];
};

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	struct crash_record *record = CRASH_RECORD;
	const char *name = k_is_in_isr() ? "ISR" : k_thread_name_get(k_current_get());

	record->reason = reason;
	record->pc = esf ? esf->basic.pc : 0;
	record->lr = esf ? esf->basic.lr : 0;
	strncpy(record->thread, name ? name : "?", sizeof(record->thread) - 1);
	record->thread[sizeof(record->thread) - 1] = '\0';
	record->magic = CRASH_MAGIC;

	LOG_PANIC();
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

void crash_report(void)
{
	struct crash_record *record = CRASH_RECORD;

	if (record->magic != CRASH_MAGIC) {
		return;
	}

	LOG_ERR("Restarted after a crash: reason %u in %s, pc 0x%08x lr 0x%08x",
		record->reason, record->thread, record->pc, record->lr);
	record->magic = 0;
}
