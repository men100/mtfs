# STM32N6570-DK simple application

STM32CubeIDEへこのdirectoryのroot project、FSBL、Appliをimportし、FSBLとAppliをbuildします。
実行時はboardのmicroSD slotへFAT12/FAT16/FAT32でformat済みのカードを挿入します。

Appliには未使用のADC/I2C、SAES/RNG、鍵保管用XSPI/BSP、RTC、LFN、console、test sourceを含めません。
外部NORからAppliを起動するFSBLのXSPIはbootに必要なため残します。`MTFS_ENABLE_SEALED_MODEL`は
定義していません。
