#ifndef _SELFTESTS_H_
#define _SELFTESTS_H_

#include "include/types.h"
#include "fs/fat32.h"

void selftests_init(struct fat32_volume *fat_vol, bool fat_ready);
void selftests_set_vfs_ready(bool ready);

/* Returns 1 if command was recognized/handled, 0 otherwise. */
int selftests_handle_command(const char *cmd);

#endif
