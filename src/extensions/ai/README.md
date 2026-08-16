# AI extensions

I/O テレメトリの特徴抽出、異常アクセス検知、ストレージ最適化などの AI 向け拡張を配置します。

Phase 3.5のcommon block、removable media、RA/ST typed diagnostics snapshotを、将来の固定長feature tensor生成の入力にします。feature extractorはcallerが付与するmonotonic timestampと連続snapshotの差分を使い、`reset_epoch`または`media_generation`が変わったpairを連続値として扱いません。coreへmodel、NPU SDK、推論runtimeを持ち込まず、ファイル名・path・ファイル内容をAI telemetryへ渡しません。
