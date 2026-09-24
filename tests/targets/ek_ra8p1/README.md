# EK-RA8P1 target test project

これは通常利用向けのsampleではなく、FatFs、hotplug、RTC、diagnostics、benchmark、crypto/modelの動作を確認するためのtest runnerです。

e² studioへのimport、build、flash、最初に実行するcommandについては[`docs/getting-started.md`](../../../docs/getting-started.md#ek-ra8p1)、PMOD2/Pmod MicroSDの配線とpin設定の変更箇所については[`docs/board-configuration.md`](../../../docs/board-configuration.md#ek-ra8p1)、command groupについては[`docs/applications.md`](../../../docs/applications.md#target-test-application)を参照してください。

FSP pinmuxは、このdirectoryの`configuration.xml`で設定します。
