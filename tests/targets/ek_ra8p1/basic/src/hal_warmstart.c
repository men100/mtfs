#include "hal_data.h"

void R_BSP_WarmStart(bsp_warm_start_event_t event)
{
    if (event == BSP_WARM_START_POST_C) {
        (void)R_IOPORT_Open(&IOPORT_CFG_CTRL, &IOPORT_CFG_NAME);
    }
}
