# Common tests

対象ボードに依存しないテスト基盤とテストケースをまとめます。ランナーはこの領域をソース参照またはビルドシステム経由で取り込みます。

- `benchmarks/`: production libraryへ含めない、固定buffer・callback駆動の非破壊performance benchmark runner
