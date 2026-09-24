# STM32N657 deployment tools

`tools/stm32n657`には、利用者がbuildしたFSBL/Appliのraw binaryから**no-key trusted-header development image**を生成し、STM32N6570-DKのexternal flashへ書き込むための補助toolが含まれています。

## Prerequisiteとlayout

* Python 3
* STM32CubeProgrammer 2.21.0以降（2.22.0で動作確認済み）
* `STM32_SigningTool_CLI`と`STM32_Programmer_CLI`
* `MX66UW1G45G_STM32N6570-DK.stldr`
* build済みのFSBL/Appli `.bin`

| image / reserved range |      address |
| ---------------------- | -----------: |
| FSBL                   | `0x70000000` |
| Appli                  | `0x70100000` |
| fleet key slot A       | `0x77ffe000` |
| fleet key slot B       | `0x77fff000` |

正式なlayoutは[`layout.json`](../tools/stm32n657/layout.json)で定義しています。toolは、external flashの範囲内に収まっていること、各image regionが重複していないこと、fleet key slotと重複していないことを確認します。

mass erase、option byteの変更、OTPへの書き込み、key provisioningは行いません。

## Header生成

まずrepository rootでdry-runを実行します。

```powershell
python tools/stm32n657/make_no_key_trusted_header.py `
  --project tests/targets/stm32n6570_dk `
  --output-dir build/stm32n657-test-images `
  --dry-run --verbose
```

内容に問題がなければ、`--dry-run`を外して実行します。

既存のoutput fileは既定では上書きしません。意図的に置き換える場合のみ`--force`を指定してください。FSBL/Appliの候補が複数見つかる場合は、`--fsbl`/`--appli`で使用するfileを明示します。

scriptはSigning Toolを`-nk`、`-align`、および既定の`-of 0x80000000`を指定して実行します。`-nk`はcryptographic signingを行わない指定であり、生成されるのは**no-key trusted-header development image**です。

生成後には、raw payloadが生成image末尾の内容とbyte単位で一致すること、および`STM2` magicが存在することを確認します。

## Flash

生成されるfile名は、元のstemに`-trusted.bin`を付けたものです。

実際に生成されたfile名を指定し、まずdry-runで内容を確認します。

```powershell
python tools/stm32n657/flash_binary.py `
  --fsbl build/stm32n657-test-images/<fsbl>-trusted.bin `
  --appli build/stm32n657-test-images/<appli>-trusted.bin `
  --target all --dry-run
```

書き込みaddress、external loader、およびfleet key slotと重複しないことを確認したうえで、実際にboardへ書き込む場合は`--confirm-flash`を指定します。

```powershell
python tools/stm32n657/flash_binary.py `
  --fsbl build/stm32n657-test-images/<fsbl>-trusted.bin `
  --appli build/stm32n657-test-images/<appli>-trusted.bin `
  --target all --confirm-flash `
  --private-manifest build/stm32n657-test-images/flash-manifest.json
```

toolはFSBL、Appliの順にprogram/verifyを行い、両方が成功した場合にのみ最後にresetします。

vendor toolを自動検出できない場合や、複数の候補が見つかった場合は、`--programmer`、`--external-loader`、`--probe-serial`を明示的に指定してください。

## 失敗時の扱いと復旧

localでのheader生成にはtemporary fileとatomic replaceを使用しますが、external flashへの書き込み全体をtransactionalにrollbackする仕組みはありません。

たとえば、FSBLの書き込みに成功した後でAppliの書き込みに失敗すると、external flashは部分的に更新された状態になります。この場合、toolは成功manifestを生成せず、最後のresetも実行しませんが、すでに書き込んだFSBLを自動的に以前の内容へ戻すこともありません。

復旧する場合は、電源とST-LINKの接続が安定していることを確認し、hash確認済みの同じFSBL/Appliを指定して、`--target all --confirm-flash`を再実行してください。

両imageのprogramとverifyが完了するまでは、書き込みが正常に完了したものとして扱わないでください。

whole-chip erase/mass eraseを行うとfleet key slotも消去されるため、使用しないでください。
