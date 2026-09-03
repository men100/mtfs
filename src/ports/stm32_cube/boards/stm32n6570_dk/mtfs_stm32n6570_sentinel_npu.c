#include "mtfs_stm32n6570_sentinel_npu.h"

#include <stddef.h>
#include <stdint.h>

#include "stm32n6xx_hal.h"

#define ST_EXEC_SIZE (UINT64_C(7576))
#define ST_PARAMS_OFFSET (UINT64_C(7688))
#define ST_PARAMS_STORAGE (UINT64_C(1048))
#define ST_ACTIVATION_ADDRESS (UINT64_C(0x342e0000))
#define ST_ACTIVATION_SIZE (UINT64_C(56))

static const mtfs_sentinel_runtime_region_policy_t policies[] = {
    {
        MTFS_SENTINEL_PROVIDER_ST_NEURAL_ART_RELOC,
        MTFS_SENTINEL_ACCELERATOR_NEURAL_ART,
        MTFS_SENTINEL_REGION_EXECUTABLE_COPY,
        MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE,
        8U, 0U, ST_EXEC_SIZE, ST_EXEC_SIZE,
        MTFS_SENTINEL_REGION_ACCESS_READ | MTFS_SENTINEL_REGION_ACCESS_WRITE,
        MTFS_SENTINEL_REGION_ACCESS_READ | MTFS_SENTINEL_REGION_ACCESS_EXECUTE,
        MTFS_SENTINEL_REGION_REQUIRE_ZEROIZE |
            MTFS_SENTINEL_REGION_REQUIRE_CACHE_COHERENCY |
            MTFS_SENTINEL_REGION_REQUIRE_EXCLUSIVE,
        MTFS_SENTINEL_REGION_POLICY_OWNED |
            MTFS_SENTINEL_REGION_POLICY_EXCLUSIVE |
            MTFS_SENTINEL_REGION_POLICY_CACHE_MAINTENANCE |
            MTFS_SENTINEL_REGION_POLICY_ALLOW_ZEROIZE |
            MTFS_SENTINEL_REGION_POLICY_GLOBAL_SERIALIZATION
    },
    {
        MTFS_SENTINEL_PROVIDER_ST_NEURAL_ART_RELOC,
        MTFS_SENTINEL_ACCELERATOR_NEURAL_ART,
        MTFS_SENTINEL_REGION_PARAMETERS,
        MTFS_SENTINEL_PLACEMENT_BINARY_CONTAINED,
        8U, ST_PARAMS_OFFSET, ST_PARAMS_OFFSET + ST_PARAMS_STORAGE,
        ST_PARAMS_STORAGE, MTFS_SENTINEL_REGION_ACCESS_READ,
        MTFS_SENTINEL_REGION_ACCESS_READ,
        MTFS_SENTINEL_REGION_REQUIRE_CACHE_COHERENCY,
        MTFS_SENTINEL_REGION_POLICY_CACHE_MAINTENANCE
    },
    {
        MTFS_SENTINEL_PROVIDER_ST_NEURAL_ART_RELOC,
        MTFS_SENTINEL_ACCELERATOR_NEURAL_ART,
        MTFS_SENTINEL_REGION_ACTIVATION,
        MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE,
        8U, ST_ACTIVATION_ADDRESS, ST_ACTIVATION_ADDRESS + ST_ACTIVATION_SIZE,
        ST_ACTIVATION_SIZE, MTFS_SENTINEL_REGION_ACCESS_WRITE,
        MTFS_SENTINEL_REGION_ACCESS_READ | MTFS_SENTINEL_REGION_ACCESS_WRITE,
        MTFS_SENTINEL_REGION_REQUIRE_ZEROIZE |
            MTFS_SENTINEL_REGION_REQUIRE_CACHE_COHERENCY |
            MTFS_SENTINEL_REGION_REQUIRE_EXCLUSIVE |
            MTFS_SENTINEL_REGION_REQUIRE_INPUT_OUTPUT_SHARED,
        MTFS_SENTINEL_REGION_POLICY_OWNED |
            MTFS_SENTINEL_REGION_POLICY_EXCLUSIVE |
            MTFS_SENTINEL_REGION_POLICY_CACHE_MAINTENANCE |
            MTFS_SENTINEL_REGION_POLICY_ALLOW_ZEROIZE |
            MTFS_SENTINEL_REGION_POLICY_GLOBAL_SERIALIZATION
    }
};

mtfs_error_t mtfs_stm32n6570_sentinel_npu_policy(
    const mtfs_sentinel_runtime_region_policy_t **result,
    uint32_t *policy_count)
{
    if (result == NULL || policy_count == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    *result = policies;
    *policy_count = (uint32_t)(sizeof(policies) / sizeof(policies[0]));
    return MTFS_OK;
}

mtfs_error_t mtfs_stm32n6570_sentinel_npu_linker_policy_validate(void)
{
#if defined(MTFS_STM32N6570_DK_LINKER_POLICY)
    extern uint8_t __mtfs_npu_activation_start__[];
    extern uint8_t __mtfs_npu_activation_end__[];
    if ((uintptr_t)__mtfs_npu_activation_start__ != ST_ACTIVATION_ADDRESS ||
        (uintptr_t)__mtfs_npu_activation_end__ -
            (uintptr_t)__mtfs_npu_activation_start__ != ST_ACTIVATION_SIZE)
        return MTFS_ERROR_INVALID_STATE;
    return MTFS_OK;
#else
    return MTFS_ERROR_NOT_SUPPORTED;
#endif
}

mtfs_error_t mtfs_stm32n6570_sentinel_npu_hardware_init(void)
{
    RIMC_MasterConfig_t requested = {RIF_CID_1,
        RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV};
    RIMC_MasterConfig_t actual = {0U, 0U};
    mtfs_error_t status = mtfs_stm32n6570_sentinel_npu_linker_policy_validate();
    if (status != MTFS_OK) return status;

    /* The authenticated contract uses the first 56 bytes of secure AXISRAM5. */
    RCC->MEMENR |= RCC_MEMENR_AXISRAM5EN;
    RAMCFG_SRAM5_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    __DSB();

    __HAL_RCC_RIFSC_CLK_ENABLE();
    HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_NPU, &requested);
    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_NPU,
        RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
    HAL_RIF_RIMC_GetConfigMasterAttributes(RIF_MASTER_INDEX_NPU, &actual);
    if (actual.MasterCID != requested.MasterCID ||
        actual.SecPriv != requested.SecPriv ||
        HAL_RIF_RISC_GetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_NPU) !=
            (RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV))
        return MTFS_ERROR_NOT_SUPPORTED;

    __HAL_RCC_NPU_CLK_ENABLE();
    __HAL_RCC_NPU_FORCE_RESET();
    __DSB();
    __HAL_RCC_NPU_RELEASE_RESET();
    __DSB();
    return __HAL_RCC_NPU_IS_CLK_ENABLED() ? MTFS_OK : MTFS_ERROR_NOT_READY;
}
