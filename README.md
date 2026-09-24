# ESP32-C3 BME680環境データロガー

ESP32-C3、BME680、0.42インチOLEDを使って、温度・湿度・気圧・IAQを測定する環境データロガーです。測定値はWi-Fi経由でGoogle Apps Script（GAS）のWeb APIへ送信し、Googleスプレッドシートに記録します。

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
- 3.3V電源
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
const char* WIFI_SSID     = "使用するWi-FiのSSID";
const char* WIFI_PASSWORD = "Wi-Fiパスワード";
const char* SHEET_URL     = "GAS WebアプリのURL";
const char* SHEET_NAME    = "Test";
const int SEND_MINUTES[]  = {10};

const bool DEBUG_MODE     = true;
const bool OLED_ROTATED   = true;
const bool BSEC_USE_ULP   = false;  // true: ULP（約5分）、false: LP（約3秒）
const uint8_t BME680_I2C_ADDRESS = 0x77;
const float TEMP_OFFSET_BME680_ULP = 0.0F;
const float TEMP_OFFSET_BME680_LP = 0.0F;
const float HUM_OFFSET_RATE = 0.0F;
const float PRESS_OFFSET = 0.0F;
```

`config.h`にはWi-FiパスワードやGAS URLが含まれるため、公開リポジトリへ実際の値を登録しないでください。

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
- LCD更新も約5分周期

### LP

```cpp
BsecOperationMode::LP
```

- ULPより消費電力が大きい
- 約3秒周期
- 温度・湿度・ガス抵抗の変化を短い間隔で確認可能
- 3.3V用LP BSEC設定を自動適用
- `BSEC_SAMPLE_RATE_LP`を自動選択
- LCD更新も約3秒周期

BSEC設定ファイルはBSEC2ライブラリ内の次のファイルを使用します。

```text
3.3V ULP: bme680_iaq_33v_300s_4d/bsec_iaq.txt
3.3V LP : bme680_iaq_33v_3s_4d/bsec_iaq.txt
```

モードを変更したときに、サンプルレートやBSEC設定ファイルを別途変更する必要はありません。

### 温度補正

補正値は動作モードごとに設定します。

```cpp
const float TEMP_OFFSET_BME680_ULP = 0.0F;
const float TEMP_OFFSET_BME680_LP = 0.0F;
```

`TEMP_OFFSET_BME680_ULP/LP`はBSEC標準の温度補正とは別に、表示・送信する温度へ最後に加える追加補正です。BSECには標準の温度補正値だけを渡します。

```text
BSECの温度出力 = BSEC標準の温度補正
表示・送信温度 = BSECの温度出力 + TEMP_OFFSET_BME680_ULP（ULP時）
表示・送信温度 = BSECの温度出力 + TEMP_OFFSET_BME680_LP  （LP時）
```

基準温度計より表示が高い場合は、該当する追加補正値をマイナスにします。例えばLP時に0.5℃下げる場合は、`TEMP_OFFSET_BME680_LP = -0.5F`とします。

### 湿度・気圧補正

```text
補正後湿度 = BSEC出力湿度 * (1 + HUM_OFFSET_RATE)
補正後気圧 = BSEC出力気圧 + PRESS_OFFSET
```

現在のコードはBSECの熱補償湿度出力がない場合にraw humidityを使用し、`HUM_OFFSET_RATE`を適用します。気圧はhPa単位で扱います。

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

BSEC はこれらの値を組み合わせて、室内の換気・におい・湿度・温度の変化を総合的に評価し、`BSEC_OUTPUT_IAQ` を出力します。つまり、IAQ は「ガス抵抗の大きさ」そのものではなく、BSEC が学習・補正した環境状態に基づく評価値です。

### 重要なポイント

1. ガス抵抗は生データであり、IAQ そのものではない
   - ばらつきが大きく、温度・湿度の変化に影響されやすい
   - 真の IAQ は、抵抗値だけでなく周囲環境との関係を含めて計算される

2. BSEC は run-in を必要とする
   - センサが一定の環境に慣れて、ベースラインが安定するまで IAQ は不安定です
   - `BSEC_OUTPUT_RUN_IN_STATUS` が完了するまでは、IAQ は信頼度が低くなります
   - プロジェクトでは `runIn <= 0` の場合は IAQ 送信を空欄にしています

3. IAQ は精度情報とセットで返る
   - `BSEC_OUTPUT_IAQ` の値だけでなく、`accuracy` も返されます
   - 精度が低い段階では、値が変動しやすく、意味のある判定には時間が必要です

4. 3.3V 用構成ファイルがモードごとに選ばれる
   - `bme680_iaq_33v_300s_4d` は ULP 向け
   - `bme680_iaq_33v_3s_4d` は LP 向け
   - どちらも BSEC の IAQ モデルに基づく設定ファイルです

このため、今回のコードは「ガス抵抗から手計算で IAQ を作る」のではなく、BSEC が最適化した IAQ アルゴリズムの出力を使う設計になっています。実際の運用では、測定後に数分〜数十分の run-in を経てから IAQ の値が安定し、ようやく室内の快適さや換気状態の判断に使えるようになります。

## GASの準備

詳細は https://github.com/Take-pachi-pachi/GAS_Raspi_Temp_Hum_press_IAQ/ を参照。

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

| パラメータ | 内容 |
| --- | --- |
| `p7` | `SHEET_NAME`のシート名 |
| `p1` | NTPで取得した時刻（`YYYY-MM-DD_HH:MM`） |
| `p2` | BSEC熱補償後の温度（℃） |
| `p3` | 補正後の湿度（%） |
| `p4` | 補正後の気圧（hPa） |
| `p5` | IAQ。`run-in`が完了するまでは空欄 |
| `p6` | 温度と湿度から計算した不快指数（DI） |

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

起動時にWi-Fiへ接続してNTP同期を行い、その後は通常Wi-Fiを切断します。指定した送信時刻を過ぎた最初のループでWi-Fiへ再接続し、NTP再同期後に直前に取得済みの値を送信します。そのためULPモードでは、指定時刻ぴったりではなく最大で約5分後に送信されます。送信時にセンサを取り直す処理はありません。

## 開発環境の導入

### 1. VS Codeをインストール

公式サイトからVisual Studio Codeをインストールします。

<https://code.visualstudio.com/>

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
- `boschsensortec/BSEC2`
- `BME68x Sensor library`
- ESP32 Arduino標準の`WiFi`、`HTTPClient`、`Wire`

BSEC2を手動でダウンロードしてプロジェクトへコピーする必要はありません。PlatformIOが`.pio/libdeps/`へ取得します。

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

`platformio`コマンドが見つからない場合は、PlatformIOの実行ファイルを直接指定します。

```powershell
C:\Users\<ユーザー名>\.platformio\penv\Scripts\platformio.exe run
C:\Users\<ユーザー名>\.platformio\penv\Scripts\platformio.exe run --target upload
```

初回ビルドでは依存ライブラリがダウンロードされます。成功時は次のように表示されます。

```text
Successfully created esp32c3 image.
========================= [SUCCESS] =========================
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

デバッグ有効時は、温度・湿度・気圧・IAQ・ガス抵抗・Stabilization・run-inを表示します。

```text
Gas sensor:
  IAQ: 50.00, accuracy: 0
  Gas resistance: 1000000 ohm
  Stabilization: 1, run-in: 0
```

IAQの`accuracy: 0`や`run-in: 0`は起動直後には正常です。BSECの学習が進むまで、IAQは初期値付近に留まる場合があります。

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

初期化失敗時の直前に表示される`BSEC error code`、`BSEC warning code`、`BME68X error code`を確認します。I2Cスキャンで`0x77`が見えていても、BSEC設定、電源電圧、配線が正しいとは限りません。

### COMポートが使用中

```text
Could not open COMx, the port is busy or doesn't exist.
```

シリアルモニター、Arduino IDE、別のターミナルなどCOMポートを使用しているアプリを閉じてから、Uploadを再実行します。

### 気圧が約1 hPaになる

BSEC2の出力はすでにhPaへ変換されています。コード側でさらに`/ 1000`しないでください。正常な気圧は通常、約`1000 hPa`前後です。

## セキュリティ上の注意

- Wi-Fiパスワードを公開リポジトリへ登録しないでください。
- GAS WebアプリURLを公開しないでください。
- `config.h`をGit管理する場合は、公開用のサンプル設定と実機用設定を分けてください。
- GASのアクセス権を[全員]にする場合、URLを知っている第三者からアクセスされる可能性があります。
