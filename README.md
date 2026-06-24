# VFD Clock V2 (VFDClockV2)

日本語の簡潔な説明書です。ESP32 を使った VFD（蛍光表示管）クロックのファームウェアです。

**主な特徴**
- BAM（Binary Angle Modulation）によるダイナミック駆動
- 自動オン/オフ（時間帯）機能
- オフ時間中は描画を継続しつつ表示をブランキングして残像を抑制
- オフ時は右端ドットを1秒毎に点滅して動作確認可能
- ボタンによる表示モード切替（オフ中は一部無効化）

**リポジトリ構成（主要ファイル）**
- src/: ソースコード
  - [src/main.cpp](src/main.cpp) - メインロジック、レンダラ、スイッチハンドラ
  - [src/display.cpp](src/display.cpp) - VFD低レベル駆動（BAM ISR、バッファ管理、ブランキング）
  - [src/display.h](src/display.h) - display API定義
  - [src/switches.cpp](src/switches.cpp) - スイッチ入力（デバウンス）
  - [src/wifi_server.cpp](src/wifi_server.cpp) - Wi‑Fi / REST 設定インターフェース

ハードウェアのピン割当（デフォルト、src/display.h を参照）:

- VFD_DAT_PIN: 16
- VFD_CLK_PIN: 2
- VFD_RCLK_PIN: 4
- VFD_RSTN_PIN: 15
- VFD_FILAMENT1_PIN: 12
- VFD_FILAMENT2_PIN: 13

スイッチピン（src/switches.cpp）:

- SW_PIN_A: 21
- SW_PIN_B: 22
- SW_PIN_C: 23

ビルド・書き込み

プラットフォーム: PlatformIO（ESP32）

1. 依存関係のインストール（PlatformIO がインストール済みであること）

```bash
platformio run
```

2. デバイスへアップロード

```bash
platformio run --target upload
```

3. シリアルモニタ（ログ確認）

```bash
platformio device monitor --baud 115200
```

使い方（基礎）

- 電源投入後、通常は時刻が表示されます。
- 自動オン/オフ設定が有効な場合、設定された時間帯外では表示がブランキング（画面は消灯）されますが、内部描画は継続します。
- オフ中は右端桁の点（ドット）が1秒毎に点滅します — これで動作確認ができます。
- オン時にボタンを押すとモード切替等が可能です。オフ中はボタン操作は無効化されています（セーフティ）。

設定・カスタマイズ

- 表示エフェクト（ロール、フェード等）は `src/main.cpp` の `displayEffectMode` を変更できます。
- 明るさ制御は現状固定（100%）。BAM 関連の実験や allowed level の調整は `src/display.cpp` にあります。

トラブルシューティング

- ビルド時に `platformio: command not found` が出る場合は PlatformIO/VSCode PlatformIO extension をインストールしてください。
- VFD に残像が残る場合はハード的なフィラメントオフ遷移（遅延で段階的に切る）や表示オフ時の追加ラッチを検討してください。

開発メモ

- ブランキングは `display_set_blank(bool)` で制御されます。自動オン/オフは `main.cpp` のロジックで `display_set_blank(true)` を使うように変更されています。
- 右端ドットの点滅は `display_init()` で生成される FreeRTOS タスクにより 1 秒周期でトグルされます。

ライセンス

プロジェクトに適用するライセンスをここに明記してください（例: MIT）。

その他

追加の改善（例: 残像最小化のための電源制御やフィラメント PWM 最適化）について希望があれば手伝います。
