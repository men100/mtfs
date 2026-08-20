# STM32N6570-DK shared Cube support

STM32N6570-DK向けアプリケーションで共用するSTM32Cube生成資産です。

- `FSBL/`: First Stage Boot Loaderの共通Core、startup、linker script
- `Drivers/`: STM32Cube FW_N6 HAL/CMSIS
- `Middlewares/`: FSBLが使用するSTM32 ExtMem Manager
- `Secure_nsclib/`: Secure application用interface library

このディレクトリはソース資産であり、CubeIDEへ直接importするプロジェクトではありません。
各consumerは自身の親ディレクトリに薄い`FSBL/`プロジェクトを持ち、ここをlinked resourceと
include pathで参照します。

- `tests/targets/stm32n6570_dk/basic/FSBL`: basic test専用wrapper/launch

この構成ではFSBL実装を一か所で保ちつつ、Appliごとのproject名、build output、load image、
debug launch設定をconsumer側へ閉じ込めます。

使用ツールはSTM32CubeIDE 2.1.1、STM32CubeMX 6.17系、STM32Cube FW_N6 V1.3.0です。
`tests/targets/stm32n6570_dk/basic/mtfs_stm32n6570_dk_test_basic.ioc`を再生成すると、
test target側へ`Drivers`、`Middlewares`、`Secure_nsclib`、FSBLソースが再作成される
場合があります。その場合は必要な生成差分をこの共通ディレクトリへ反映し、consumer側には
薄いEclipse wrapper以外の複製を残さないでください。
