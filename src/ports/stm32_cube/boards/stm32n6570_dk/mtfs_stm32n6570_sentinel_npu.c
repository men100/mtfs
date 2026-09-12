#include "mtfs_stm32n6570_sentinel_npu.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#include <stddef.h>
#include <stdint.h>

#include "npu_cache.h"
#include "stm32n6xx_hal.h"

#define ST_EXEC_SIZE (UINT64_C(7640))
#define ST_PARAMS_ADDRESS (UINT64_C(0x34100000))
#define ST_PARAMS_SIZE (UINT64_C(1041))
#define ST_ACTIVATION_SIZE (UINT64_C(24))

#define ST_OWNED_POLICY_FLAGS (MTFS_SENTINEL_REGION_POLICY_OWNED | \
    MTFS_SENTINEL_REGION_POLICY_EXCLUSIVE | \
    MTFS_SENTINEL_REGION_POLICY_CACHE_MAINTENANCE | \
    MTFS_SENTINEL_REGION_POLICY_ALLOW_ZEROIZE | \
    MTFS_SENTINEL_REGION_POLICY_GLOBAL_SERIALIZATION)
#define ST_OWNED_REQUIREMENTS (MTFS_SENTINEL_REGION_REQUIRE_ZEROIZE | \
    MTFS_SENTINEL_REGION_REQUIRE_CACHE_COHERENCY | \
    MTFS_SENTINEL_REGION_REQUIRE_EXCLUSIVE)
#define ST_ACTIVATION_POLICY(address) { \
    MTFS_SENTINEL_PROVIDER_ST_NEURAL_ART_RELOC, \
    MTFS_SENTINEL_ACCELERATOR_NEURAL_ART, \
    MTFS_SENTINEL_REGION_ACTIVATION, \
    MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE, \
    8U, (address), (address) + ST_ACTIVATION_SIZE, ST_ACTIVATION_SIZE, \
    MTFS_SENTINEL_REGION_ACCESS_WRITE, \
    MTFS_SENTINEL_REGION_ACCESS_READ | MTFS_SENTINEL_REGION_ACCESS_WRITE, \
    ST_OWNED_REQUIREMENTS, ST_OWNED_POLICY_FLAGS }

static const mtfs_sentinel_runtime_region_policy_t policies[] = {
    {
        MTFS_SENTINEL_PROVIDER_ST_NEURAL_ART_RELOC,
        MTFS_SENTINEL_ACCELERATOR_NEURAL_ART,
        MTFS_SENTINEL_REGION_EXECUTABLE_COPY,
        MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE,
        8U, 0U, ST_EXEC_SIZE, ST_EXEC_SIZE,
        MTFS_SENTINEL_REGION_ACCESS_READ | MTFS_SENTINEL_REGION_ACCESS_WRITE,
        MTFS_SENTINEL_REGION_ACCESS_READ | MTFS_SENTINEL_REGION_ACCESS_EXECUTE,
        ST_OWNED_REQUIREMENTS, ST_OWNED_POLICY_FLAGS
    },
    {
        MTFS_SENTINEL_PROVIDER_ST_NEURAL_ART_RELOC,
        MTFS_SENTINEL_ACCELERATOR_NEURAL_ART,
        MTFS_SENTINEL_REGION_PARAMETERS,
        MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE,
        8U, ST_PARAMS_ADDRESS, ST_PARAMS_ADDRESS + ST_PARAMS_SIZE,
        ST_PARAMS_SIZE,
        MTFS_SENTINEL_REGION_ACCESS_READ | MTFS_SENTINEL_REGION_ACCESS_WRITE,
        MTFS_SENTINEL_REGION_ACCESS_READ,
        ST_OWNED_REQUIREMENTS, ST_OWNED_POLICY_FLAGS
    },
    ST_ACTIVATION_POLICY(UINT64_C(0x34200000)),
    ST_ACTIVATION_POLICY(UINT64_C(0x34270000)),
    ST_ACTIVATION_POLICY(UINT64_C(0x342e0000))
};

/* Strong hooks used by the ST NPU cache driver during its first enable. */
void npu_cache_enable_clocks_and_reset(void)
{
    __HAL_RCC_CACHEAXI_CLK_ENABLE();
    __HAL_RCC_CACHEAXI_FORCE_RESET();
    __HAL_RCC_CACHEAXI_RELEASE_RESET();
}

void npu_cache_disable_clocks_and_reset(void)
{
    __HAL_RCC_CACHEAXI_FORCE_RESET();
    __HAL_RCC_CACHEAXI_RELEASE_RESET();
    __HAL_RCC_CACHEAXI_CLK_DISABLE();
}

static mtfs_error_t configure_secure_cid1_region(RISAF_TypeDef *risaf,
    uint32_t maximum_address)
{
    RISAF_BaseRegionConfig_t requested = {RISAF_FILTER_ENABLE,
        RIF_ATTRIBUTE_SEC, RIF_CID_1, RIF_CID_1, RIF_CID_1,
        0U, maximum_address};
    RISAF_BaseRegionConfig_t denied_nonsecure = {RISAF_FILTER_ENABLE,
        RIF_ATTRIBUTE_NSEC, RIF_CID_NONE, RIF_CID_NONE, RIF_CID_NONE,
        0U, maximum_address};
    RISAF_BaseRegionConfig_t actual = {0U};
    HAL_RIF_RISAF_ConfigBaseRegion(risaf, 0U, &requested);
    HAL_RIF_RISAF_GetConfigBaseRegion(risaf, 0U, &actual);
    if (actual.Filtering != requested.Filtering ||
        actual.Secure != requested.Secure ||
        actual.PrivWhitelist != requested.PrivWhitelist ||
        actual.ReadWhitelist != requested.ReadWhitelist ||
        actual.WriteWhitelist != requested.WriteWhitelist ||
        actual.StartAddress != requested.StartAddress ||
        actual.EndAddress != requested.EndAddress)
        return MTFS_ERROR_NOT_SUPPORTED;
    HAL_RIF_RISAF_ConfigBaseRegion(risaf, 1U, &denied_nonsecure);
    HAL_RIF_RISAF_GetConfigBaseRegion(risaf, 1U, &actual);
    if (actual.Filtering != denied_nonsecure.Filtering ||
        actual.Secure != denied_nonsecure.Secure ||
        actual.PrivWhitelist != denied_nonsecure.PrivWhitelist ||
        actual.ReadWhitelist != denied_nonsecure.ReadWhitelist ||
        actual.WriteWhitelist != denied_nonsecure.WriteWhitelist ||
        actual.StartAddress != denied_nonsecure.StartAddress ||
        actual.EndAddress != denied_nonsecure.EndAddress)
        return MTFS_ERROR_NOT_SUPPORTED;
    return MTFS_OK;
}

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
    extern uint8_t __mtfs_npu_params_start__[], __mtfs_npu_params_end__[];
    extern uint8_t __mtfs_npu_activation3_start__[], __mtfs_npu_activation3_end__[];
    extern uint8_t __mtfs_npu_activation4_start__[], __mtfs_npu_activation4_end__[];
    extern uint8_t __mtfs_npu_activation5_start__[], __mtfs_npu_activation5_end__[];
    if ((uintptr_t)__mtfs_npu_params_start__ != ST_PARAMS_ADDRESS ||
        (uintptr_t)__mtfs_npu_params_end__ - (uintptr_t)__mtfs_npu_params_start__ !=
            ST_PARAMS_SIZE ||
        (uintptr_t)__mtfs_npu_activation3_start__ != UINT64_C(0x34200000) ||
        (uintptr_t)__mtfs_npu_activation4_start__ != UINT64_C(0x34270000) ||
        (uintptr_t)__mtfs_npu_activation5_start__ != UINT64_C(0x342e0000) ||
        (uintptr_t)__mtfs_npu_activation3_end__ -
            (uintptr_t)__mtfs_npu_activation3_start__ != ST_ACTIVATION_SIZE ||
        (uintptr_t)__mtfs_npu_activation4_end__ -
            (uintptr_t)__mtfs_npu_activation4_start__ != ST_ACTIVATION_SIZE ||
        (uintptr_t)__mtfs_npu_activation5_end__ -
            (uintptr_t)__mtfs_npu_activation5_start__ != ST_ACTIVATION_SIZE)
        return MTFS_ERROR_INVALID_STATE;
    return MTFS_OK;
}

mtfs_error_t mtfs_stm32n6570_sentinel_npu_hardware_init(void)
{
    RIMC_MasterConfig_t requested = {RIF_CID_1,
        RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV};
    RIMC_MasterConfig_t actual = {0U, 0U};
    mtfs_error_t status = mtfs_stm32n6570_sentinel_npu_linker_policy_validate();
    if (status != MTFS_OK) return status;

    /* Enable every internal SRAM bank named by the authenticated descriptor. */
    RCC->MEMENR |= RCC_MEMENR_AXISRAM2EN | RCC_MEMENR_AXISRAM3EN |
        RCC_MEMENR_AXISRAM4EN | RCC_MEMENR_AXISRAM5EN;
    RAMCFG_SRAM2_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    RAMCFG_SRAM3_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    RAMCFG_SRAM4_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    RAMCFG_SRAM5_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    __DSB();

    __HAL_RCC_RIFSC_CLK_ENABLE();
    __HAL_RCC_NPU_CLK_ENABLE();
    status = configure_secure_cid1_region(RISAF2, RISAF2_LIMIT_ADDRESS_SPACE_SIZE);
    if (status == MTFS_OK)
        status = configure_secure_cid1_region(RISAF3, RISAF3_LIMIT_ADDRESS_SPACE_SIZE);
    if (status == MTFS_OK)
        status = configure_secure_cid1_region(RISAF4, RISAF4_LIMIT_ADDRESS_SPACE_SIZE);
    if (status == MTFS_OK)
        status = configure_secure_cid1_region(RISAF5, RISAF5_LIMIT_ADDRESS_SPACE_SIZE);
    if (status == MTFS_OK)
        status = configure_secure_cid1_region(RISAF6, RISAF6_LIMIT_ADDRESS_SPACE_SIZE);
    if (status != MTFS_OK) return status;
    HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_NPU, &requested);
    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_NPU,
        RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
    HAL_RIF_RIMC_GetConfigMasterAttributes(RIF_MASTER_INDEX_NPU, &actual);
    if (actual.MasterCID != requested.MasterCID ||
        actual.SecPriv != requested.SecPriv ||
        HAL_RIF_RISC_GetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_NPU) !=
            (RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV))
        return MTFS_ERROR_NOT_SUPPORTED;

    __HAL_RCC_NPU_FORCE_RESET();
    __DSB();
    __HAL_RCC_NPU_RELEASE_RESET();
    __DSB();
    if (!__HAL_RCC_NPU_IS_CLK_ENABLED()) return MTFS_ERROR_NOT_READY;

    /* Reloc install performs CACHEAXI maintenance before RuntimeInit(). */
    npu_cache_enable();
    return MTFS_OK;
}

#endif
