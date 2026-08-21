# EK-RA8P1 simple application

e2 studioへこのdirectoryをexisting projectとしてimportし、FSP code generation後にDebugまたはReleaseを
buildします。SDカードはEK-RA8P1のPmod 2へ接続したDigilent Pmod MicroSDで使用します。

fresh import直後はmanaged-buildの`Debug/makefile`または`Release/makefile`がまだ存在しないため、
最初の操作にCleanを選ぶと`No rule to make target 'clean'`になります。最初に通常のBuild Projectを
1回実行してください。makefile生成後はClean Projectも使用できます。

crypto/RSIP/PSA、OSPI key store、RTC、LFN、console、test sourceはprojectへ含めません。
`MTFS_ENABLE_SEALED_MODEL`も定義していません。
