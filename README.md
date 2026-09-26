# README.md

# ESP32-C3 BME680環境データロガー

ESP32-C3、BME680、0.42インチOLEDを使って、温度・湿度・気圧・IAQなどを測定する環境データロガーです。測定値はWi-Fi経由でGoogle Apps Script（GAS）のWeb APIへ送信し、Googleスプレッドシートに記録します。

## 構成

- マイコン: ESP32-C3 DevKitM-1
- 環境センサ: BME680（I2C）
- 表示: 0.42インチOLED SSD1306（I2C）
- センサ処理: Bosch BSEC2
- 開発環境: VS Code + PlatformIO
- 通信: Wi-Fi、NTP、GAS Web API

BSEC2が温度補償、ガス抵抗処理、IAQ計算を行います。IAQは単純なガス抵抗値の換算ではなく、BSECのアルゴリズム出力です。

## 必要なもの

- ESP32-C3 DevKitM-1
- BME680モジュール
- 0.42インチOLED SSD1306 I2Cモジュール
- USBケーブル（データ通信対応）
- Windows PC
- VS Code
- PlatformIO IDE拡張
- Googleアカウント（GASとスプレッドシート用）

## 配線

OLEDとBME680を同じI2Cバスへ接続します。

| 信号 | ESP32-C3 |
| --- | --- |
| SDA | GPIO5 |
| SCL | GPIO6 |
| VCC | 3.3V |
| GND | GND |

想定I2Cアドレスは次のとおりです。

| デバイス | アドレス |
| --- | --- |
| OLED | `0x3C` |
| BME680 | `0x77` |

起動時にI2Cスキャンを行います。シリアルモニターにOLEDとBME680の両方が表示されることを確認してください。

## config.h パラメータ

[src/config.h](src/config.h)を編集します。

```cpp
#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

const char* WIFI_SSID      = "使用するWi-FiのSSID";
const char* WIFI_PASSWORD  = "Wi-Fiパスワード";
const char* SHEET_URL      = "GAS WebアプリのURL";
const char* SHEET_NAME     = "Home-2F"; // Home-LDK, Home-2F, Test

const uint8_t BME680_I2C_ADDRESS = 0x77;
const int SEND_MINUTES[]   = {10};   // GASへ毎時データ送信する分を指定（0～59）。複数指定可。例：{0, 15, 30, 45}
const bool DEBUG_MODE      = false;  // true: デバッグ情報をシリアル出力する、false: 出力しない
const bool ENABLE_OLED     = true;   // false にすると2回目の更新から画面消灯
const bool OLED_ROTATED    = true;   // true: OLEDを180度回転表示する、false: 通常表示
const bool BSEC_USE_ULP    = true;   // true: ULP（約5分）、false: LP（約3秒）
const bool USE_STATIC_IAQ  = true;   // true: Static IAQ（建物・長期設置向け）, false: 通常のIAQ（モバイル・相対変化向け）

// BSEC内部設定用オフセット（プラスの値で表示温度マイナス）
const float BSEC_INTERNAL_TEMP_OFFSET_ULP = 0.6F;
const float BSEC_INTERNAL_TEMP_OFFSET_LP  = 1.6F;

// BSEC標準の温度補正値とは別に、表示・送信用に最後に加える補正
const float DISPLAY_TEMP_OFFSET_ULP = 0.0F;
const float DISPLAY_TEMP_OFFSET_LP  = 0.0F;
const float HUM_OFFSET_RATE         = -0.2F;
const float PRESS_OFFSET            = 0.0F;

const char* NTP_SERVER_PRIMARY   = "ntp.nict.jp";
const char* NTP_SERVER_SECONDARY = "pool.ntp.org";
const char* TIME_ZONE            = "JST-9";

#endif
```

`config.h`にはWi-FiパスワードやGAS URLが含まれるため、公開リポジトリへ実際の値を登録しないでください。実機用設定はローカル環境で保持し、READMEのサンプルはプレースホルダーとして扱います。

`TIME_ZONE`はPOSIX形式のタイムゾーン文字列です。日本時間は`JST-9`を指定します。POSIX形式ではUTCより東側のオフセットを負の値で表すため、`UTC+9`に相当する指定が`JST-9`になります。

> このプロジェクトでは、BSEC内部温度オフセット（`BSEC_INTERNAL_TEMP_OFFSET_ULP/LP`）と、表示・送信時の最終補正（`DISPLAY_TEMP_OFFSET_ULP/LP`）を分けて管理できます。

## ULP/LP動作モード

モードを変更する場合は、[src/config.h](src/config.h)の`BSEC_USE_ULP`を変更します。

```cpp
const bool BSEC_USE_ULP = false;
```

### ULP

```cpp
BsecOperationMode::ULP
```

- 低消費電力
- 約300秒（約5分）周期
- 電池駆動・長期測定向け
- 3.3V用ULP BSEC設定を自動適用
- `BSEC_SAMPLE_RATE_ULP`を自動選択
- 画面更新も約5分周期

### LP

```cpp
BsecOperationMode::LP
```

- ULPより消費電力が大きい
- 約3秒周期
- 温度・湿度・ガス抵抗の変化を短い間隔で確認可能
- 3.3V用LP BSEC設定を自動適用
- `BSEC_SAMPLE_RATE_LP`を自動選択
- 画面更新も約3秒周期

BSEC設定ファイルはBSEC2ライブラリ内の次のファイルを使用します。

```text
3.3V ULP: bme680_iaq_33v_300s_4d/bsec_iaq.txt
3.3V LP : bme680_iaq_33v_3s_4d/bsec_iaq.txt
```

モードを変更したときに、サンプルレートやBSEC設定ファイルを別途変更する必要はありません。

### OLED表示設定（ENABLE_OLED）

`ENABLE_OLED = false` に設定した場合でも、起動直後（1回目の描画）はステータス確認のためにOLEDに表示が行われます。2回目の更新タイミング（LP時約3秒後 / ULP時約5分後）から画面が消灯（全消去）され、消費電力を抑制します。

### 温度補正

補正値は動作モードごとに設定します。

```cpp
// BSEC内部用オフセット
const float BSEC_INTERNAL_TEMP_OFFSET_ULP = 1.3F;
const float BSEC_INTERNAL_TEMP_OFFSET_LP  = 1.6F;

// 表示・送信用の追加補正
const float DISPLAY_TEMP_OFFSET_ULP = 0.0F;
const float DISPLAY_TEMP_OFFSET_LP  = 0.0F;
```

`DISPLAY_TEMP_OFFSET_ULP/LP`はBSEC標準の温度補正とは別に、表示・送信する温度へ最後に加える追加補正です。

```text
BSECの補正温度 = BSEC内部の熱補償出力
表示・送信温度 = BSECの補正温度 + DISPLAY_TEMP_OFFSET_ULP（ULP時）
表示・送信温度 = BSECの補正温度 + DISPLAY_TEMP_OFFSET_LP  （LP時）
```

### 湿度・気圧補正

```text
補正後湿度 = BSEC出力湿度 * (1 + HUM_OFFSET_RATE)
補正後気圧 = BSEC生気圧 + PRESS_OFFSET
```

現在のコードはBSECの熱補償湿度（`CompHum`）または生湿度（`RawHum`）に`HUM_OFFSET_RATE`を適用します。気圧はhPa単位で扱います。

## IAQ計算

BME680の「ガス抵抗」だけをそのまま IAQ とみなすのは、実際には不適切です。ガス抵抗値は環境条件に大きく左右されるため、温度・湿度・気圧の変化や長期的なベースライン変動が混ざります。

このプロジェクトでは、単純な電気抵抗換算ではなく、BSEC（Bosch Sensortec Environmental Cluster）で IAQ を算出しています。BSEC は、次のような情報を同時に扱って内部アルゴリズムで IAQ を推定します。

- `BSEC_OUTPUT_RAW_GAS`: ガス抵抗の生値
- `BSEC_OUTPUT_RAW_TEMPERATURE`: 温度の生値
- `BSEC_OUTPUT_RAW_HUMIDITY`: 湿度の生値
- `BSEC_OUTPUT_RAW_PRESSURE`: 圧力の生値
- `BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE`: 近接した熱影響を補正した温度
- `BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY`: 熱補償を反映した湿度
- `BSEC_OUTPUT_STABILIZATION_STATUS`: 安定化の進行状況
- `BSEC_OUTPUT_RUN_IN_STATUS`: run-in の進行状況

BSEC はこれらの値を組み合わせて、室内の換気・におい・湿度・温度の変化を総合的に評価し、`BSEC_OUTPUT_IAQ` または `BSEC_OUTPUT_STATIC_IAQ` を出力します。`config.h` の `USE_STATIC_IAQ` が `true` の場合は、建物や設置型環境に適した Static IAQ が使用・表示されます。

### 重要なポイント

1. ガス抵抗は生データであり、IAQ そのものではない
   - ばらつきが大きく、温度・湿度の変化に影響されやすい
   - 真の IAQ は、抵抗値だけでなく周囲環境との関係を含めて計算される

2. BSEC は run-in やキャリブレーション（Accuracy）を必要とする
   - センサが一定の環境に慣れて、ベースラインが安定するまで IAQ は不定です
   - `accuracy > 0` になるまでは、GAS送信時のIAQ関連パラメータ（p6〜p9）は空欄（無効値）として送信されます

3. 3.3V 用構成ファイルがモードごとに選ばれる
   - `bme680_iaq_33v_300s_4d` は ULP 向け
   - `bme680_iaq_33v_3s_4d` は LP 向け
   - どちらも BSEC の IAQ モデルに基づく設定ファイルです

このため、今回のコードは「ガス抵抗から手計算で IAQ を作る」のではなく、BSEC が最適化した IAQ アルゴリズムの出力を使う設計になっています。

## GASの準備

詳細は [https://github.com/Take-pachi-pachi/GAS_sensor-data-to-spreadsheet/](https://github.com/Take-pachi-pachi/GAS_sensor-data-to-spreadsheet/) を参照。

### 1. スプレッドシート

1. Googleスプレッドシートを作成します。
2. 書き込み先のシートタブを作成します。
3. シート名を`config.h`の`SHEET_NAME`と一致させます。

### 2. GASプロジェクト

1. スプレッドシートで、[拡張機能] > [Apps Script]を開きます。
2. GASの`Code.gs`を貼り付けて保存します。
3. プロジェクトの設定でスクリプトプロパティを追加します。
4. プロパティ名を`Spread_ID`、値を対象スプレッドシートのIDにします。

スプレッドシートIDは、URLの次の部分です。

```text
https://docs.google.com/spreadsheets/d/ここがスプレッドシートID/edit
```

### 3. Webアプリとしてデプロイ

1. [デプロイ] > [新しいデプロイ]を選択します。
2. 種類を[ウェブアプリ]にします。
3. 次のユーザーとして実行を[自分]にします。
4. アクセスできるユーザーを[全員]にします。
5. デプロイします。
6. 発行されたURLを`config.h`の`SHEET_URL`へ設定します。

## GASへ送信するデータ

HTTP GET クエリパラメータ（`p1`〜`p14`）としてデータを送信します。

| パラメータ | 内容 | 備考 |
| --- | --- | --- |
| `p1` | `SHEET_NAME`のシート名 | 例: `Home-2F` |
| `p2` | NTPで補正されたESP32内部時計の時刻 | 形式: `YYYY-MM-DD_HH:MM` |
| `p3` | 補正後の温度（℃） | 小数点第2位まで |
| `p4` | 補正後の湿度（%） | 小数点第2位まで |
| `p5` | 補正後の気圧（hPa） | 小数点第2位まで |
| `p6` | 通常 IAQ | Accuracy > 0 の場合のみ送信 |
| `p7` | Static IAQ | Accuracy > 0 の場合のみ送信 |
| `p8` | eCO2 (ppm) | Accuracy > 0 の場合のみ送信 |
| `p9` | bVOC equivalent (ppm) | Accuracy > 0 の場合のみ送信 |
| `p10` | ガス抵抗値 (Ohm) | 整数表記 |
| `p11` | ガス割合 (%) | 小数点第2位まで |
| `p12` | Stabilization ステータス | 整数表記 |
| `p13` | Run-in ステータス | 整数表記 |
| `p14` | 不快指数 (Discomfort Index) | 温度・湿度から自動計算 |

GAS Webアプリはリダイレクトを返すため、ESP32側ではリダイレクト追従を有効にしています。

### 送信時刻

`SEND_MINUTES`には、毎時何分に送信するかを`0`から`59`で指定します。

```cpp
const int SEND_MINUTES[] = {10};
```

上の設定では毎時10分に送信します。複数指定もできます。

```cpp
const int SEND_MINUTES[] = {10, 40};
```

起動時にWi-Fiへ接続してNTP同期を行い、その後はWi-Fiを切断します。指定した送信時刻を過ぎた最初のループでWi-Fiへ再接続し、接続直後にNTPでESP32内部時計を補正します。URLの時刻情報は、送信時にこの内部時計を読み取って設定します。送信時にNTPへ直接問い合わせたり、センサを取り直したりする処理はありません。そのためULPモードでは、指定時刻ぴったりではなく最大で約5分後に送信されます。Wi-Fi接続またはNTP同期に失敗した場合は送信を中止します。

## コードの処理の流れ

### 起動時

1. シリアル通信を初期化します。
2. Wi-Fiへ接続し、`configTzTime()`でNTPサーバーとタイムゾーンを設定してESP32内部時計を日本標準時に合わせます。
3. 初期NTP同期後、通常時の消費電力を抑えるためWi-Fiを切断します。
4. OLEDとI2Cバスを初期化し、接続されているI2Cデバイスをスキャンします。
5. BME680を初期化し、`BSEC_USE_ULP`に応じたBSEC設定とセンサ出力を登録します。

### 繰り返し処理

1. BSECを実行（`processBsecOutputs`）し、各種出力（温度・湿度・気圧・IAQ・CO2等）をグローバル変数に格納します。
2. 測定・表示周期タイミング（初回即時実行、以降は ULP: 5分 / LP: 3秒 間隔）に到達したかを判定します。
3. 補正後の温度・湿度・気圧およびIAQをOLEDへ表示します（2回目以降で `ENABLE_OLED = false` の場合は消灯）。
4. `DEBUG_MODE`が`true`の場合、詳細なセンサ値とESP32内部時計をシリアルへ出力します。
5. ESP32内部時計を取得し、送信予定時刻（`SEND_MINUTES`）を通過したか確認します。
6. 送信対象かつ同じ時間帯・予定分で未送信の場合、送信処理へ進みます。
7. Wi-Fiへ接続し、NTP同期で内部時計を補正後、クエリ文字列（`p1`〜`p14`）を作成してHTTP GET送信を行います。
8. 送信処理後にWi-Fiを切断し、次の周期までループを継続します。

### 送信時刻の判定

`SEND_MINUTES`には、毎時何分に送信するかを`0`から`59`で指定します。現在分以下の予定分のうち、最も大きいものを送信対象にします。例えば`{10, 50}`の場合、10分を過ぎると10分枠、50分を過ぎると50分枠として扱います。

同じ時間・同じ予定分では、`lastScheduledHourKey`と`lastScheduledMinute`によって二重送信を防止します。送信に失敗しても同じ予定分に自動再送はせず、次の予定分まで待機します。

## 開発環境の導入

### 1. VS Codeをインストール

公式サイトからVisual Studio Codeをインストールします。

[https://code.visualstudio.com/](https://code.visualstudio.com/)

### 2. PlatformIO IDEをインストール

1. VS Codeを起動します。
2. 左側の拡張機能アイコンを開きます。
3. `PlatformIO IDE`を検索します。
4. PlatformIO IDEをインストールします。
5. VS Codeを再起動します。

PlatformIOは初回ビルド時に、ESP32用プラットフォーム、Arduinoフレームワーク、指定ライブラリを自動的にダウンロードします。初回は数分かかる場合があります。

### 3. プロジェクトを開く

VS Codeで、プロジェクトフォルダー `esp32-c3_OLED042_bme680_SendEnvInfo_to_GAS` を開きます。

プロジェクト直下に次のファイルがあることを確認します。

```text
platformio.ini
src/main.cpp
src/config.h
```

### 4. 使用ライブラリ

`platformio.ini`により、次のライブラリが自動導入されます。

- `olikraus/U8g2`
- `[https://github.com/BoschSensortec/Bosch-BME68x-Library.git](https://github.com/BoschSensortec/Bosch-BME68x-Library.git)`
- `[https://github.com/BoschSensortec/Bosch-BSEC2-Library.git](https://github.com/BoschSensortec/Bosch-BSEC2-Library.git)`
- ESP32 Arduino標準の`WiFi`、`HTTPClient`、`Wire`

## ビルドと書き込み

### PlatformIO画面から実行

左側のPlatformIOアイコンを開き、次を実行します。

1. `Project Tasks`を開く
2. `esp32-c3-devkitm-1`を開く
3. `General` > `Build`
4. ESP32-C3をUSB接続する
5. `General` > `Upload`

### ターミナルから実行

PowerShellでプロジェクトフォルダーへ移動して実行します。

```powershell
platformio run
platformio run --target upload
```

## シリアルモニター

通信速度は`115200`です。

PlatformIOの`Monitor`を実行するか、ターミナルで次を実行します。

```powershell
platformio device monitor --baud 115200
```

正常起動時の主なログは次のようになります。

```text
I2C scan:
  Found device at 0x3C
  Found device at 0x77
Initializing BSEC at I2C address 0x77 ...
BME680/BSEC: OK
```

`DEBUG_MODE = true` 設定時は、より詳細なパラメータ群（不快指数 DI や CO2, bVOC, 各種精度フラグなど）がシリアルへ出力されます。

## トラブルシューティング

### BME680が見つからない

シリアルモニターに`0x77`が表示されない場合は、次を確認します。

- SDAがGPIO5、SCLがGPIO6になっているか
- VCCが3.3Vになっているか
- GNDが共通になっているか
- BME680モジュールのI2Cアドレス設定が`0x77`か
- 配線の接触不良がないか

アドレスが`0x76`の場合は、`config.h`の`BME680_I2C_ADDRESS`も一致させます。

### `BSEC warning code: 14`

サンプルレートとBSEC設定の不一致です。現在のコードではモード選択に応じて3.3V用設定とレートを自動選択します。ライブラリ内の設定ファイルを直接編集しないでください。

### `BME680/BSEC: NG`

初期化失敗時の直前に表示される`BSEC error code`、`BSEC warning code`、`BME68X error code`を確認します。

### COMポートが使用中

```text
Could not open COMx, the port is busy or doesn't exist.
```

シリアルモニター、Arduino IDE、別のターミナルなどCOMポートを使用しているアプリを閉じてから、Uploadを再実行します。

## ライセンス

このプロジェクトでTake-pachi-pachiが作成したソースコードは、ルートの
[LICENSE](LICENSE)に記載した独自の非商用ライセンスで公開します。私的利用、改変、
無償での共有は許可しますが、販売、広告収益を伴う利用、有償サービスや商用製品への
組み込みなどの商用利用は許可しません。商用利用にはTake-pachi-pachiの事前の書面に
よる許可が必要です。再配布時はライセンス文と著作権表示を保持してください。

PlatformIOで取得するU8g2、BME68x Sensor library、BSEC2 Arduino libraryは、
それぞれの著作権表示とライセンス条件に従います。依存ライブラリの一覧と注意事項は
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)にまとめています。

BSEC 2.xのバイナリには、Bosch Sensortecの別途の使用許諾条件が適用されます。
このプロジェクトをビルドしたファームウェアを第三者へ配布する場合は、BSECの
ライセンス条件を確認してください。非商用であっても、この条件が自動的に免除される
わけではありません。

## セキュリティ上の注意

- Wi-Fiパスワードを公開リポジトリへ登録しないでください。
- GAS WebアプリURLを公開しないでください。
- `config.h`をGit管理する場合は、公開用のサンプル設定と実機用設定を分けてください。
- GASのアクセス権を[全員]にする場合、URLを知っている第三者からアクセスされる可能性があります。