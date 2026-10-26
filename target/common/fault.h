#ifndef SB_FAULT_H
#define SB_FAULT_H

#include <stdint.h>

/* Enable the configurable fault handlers and remember what is running, for
 * the crash record. */
void sb_fault_init(uint32_t image_version, uint32_t slot);

/* Record a software-detected failure (fault_type = thread mode, 0) and reset. */
__attribute__((noreturn)) void sb_fault_app_assert(uint32_t code);

#endif
