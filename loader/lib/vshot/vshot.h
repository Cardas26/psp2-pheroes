#ifndef VSHOT_H
#define VSHOT_H

#include <stdint.h>
#include <psp2/ctrl.h>

#define VSHOT_COMBO (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_CROSS)

int vshot_combo(uint32_t buttons);

int vshot_take(const char *data_dir);

#endif
