# STM32N6570-DK target test project

これは通常利用向けのsampleではなく、FatFs、hotplug、RTC、SDMMC diagnostics、benchmark、crypto/modelの動作を確認するためのFSBL/Appli test runnerです。

STM32CubeIDEへのimport、build、no-key trusted-header development imageの生成、flashについては[`docs/getting-started.md`](../../../docs/getting-started.md#stm32n6570-dk)と[`docs/stm32-deployment.md`](../../../docs/stm32-deployment.md)、pin/IRQ設定の変更箇所については[`docs/board-configuration.md`](../../../docs/board-configuration.md#stm32n6570-dk)、command groupについては[`docs/applications.md`](../../../docs/applications.md#target-test-application)を参照してください。

pinmux、SDMMC2、RIF、IRQは`mtfs_stm32n6570_dk_test.ioc`で設定します。
