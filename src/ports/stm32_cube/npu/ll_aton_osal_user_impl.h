#ifndef LL_ATON_OSAL_USER_IMPL_H
#define LL_ATON_OSAL_USER_IMPL_H

#ifndef APP_HAS_PARALLEL_NETWORKS
#define APP_HAS_PARALLEL_NETWORKS 0
#endif

void mtfs_stm32n6_aton_osal_init(void);
void mtfs_stm32n6_aton_osal_deinit(void);
void mtfs_stm32n6_aton_osal_wfe(void);
void mtfs_stm32n6_aton_osal_signal_event(void);

#define LL_ATON_OSAL_INIT() mtfs_stm32n6_aton_osal_init()
#define LL_ATON_OSAL_DEINIT() mtfs_stm32n6_aton_osal_deinit()
#define LL_ATON_OSAL_WFE() mtfs_stm32n6_aton_osal_wfe()
#define LL_ATON_OSAL_SIGNAL_EVENT() mtfs_stm32n6_aton_osal_signal_event()

#endif
