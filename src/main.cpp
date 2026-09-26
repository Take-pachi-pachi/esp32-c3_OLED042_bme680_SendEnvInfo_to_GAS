#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <U8g2lib.h>
#include <bsec2.h>
#include "config.h"

#ifndef ARRAY_LEN
#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))
#endif

// 0.42インチ OLED ピン設定 (SDA: GPIO5, SCL: GPIO6)
#define OLED_SDA 5
#define OLED_SCL 6
#define OLED_RESET U8X8_PIN_NONE

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RESET, OLED_SCL, OLED_SDA);
Bsec2 envSensor;
bool bme680Ready = false;

// BSEC2 測定データ用変数
float bme680Iaq = NAN;
float bme680IaqAccuracy = 0.0F;
float bme680StaticIaq = NAN;
float bme680StaticIaqAccuracy = 0.0F;
float bme680Co2Equivalent = NAN;
float bme680Co2Accuracy = 0.0F;
float bme680BvocEquivalent = NAN;
float bme680BvocAccuracy = 0.0F;
float bme680GasResistance = NAN;
float bme680GasPercentage = NAN;
float bme680Stabilization = NAN;
float bme680RunIn = NAN;
float bme680CompTemp = NAN;
float bme680CompHum = NAN;
float bme680RawTemp = NAN;
float bme680RawHum = NAN;
float bme680RawPressure = NAN;

enum class BsecOperationMode {
  ULP,
  LP
};

constexpr BsecOperationMode BSEC_OPERATION_MODE =
  BSEC_USE_ULP ? BsecOperationMode::ULP : BsecOperationMode::LP;

constexpr float BSEC_SAMPLE_RATE =
    (BSEC_OPERATION_MODE == BsecOperationMode::ULP)
      ? BSEC_SAMPLE_RATE_ULP
      : BSEC_SAMPLE_RATE_LP;

constexpr uint32_t SENSOR_DISPLAY_INTERVAL_MS =
    (BSEC_OPERATION_MODE == BsecOperationMode::ULP)
      ? 300000UL // ULP: 5分 (300,000ms)
      : 3000UL;  // LP:  3秒 (3,000ms)

const uint8_t BSEC_CONFIG_33V_ULP[] = {
  #include <config/bme680/bme680_iaq_33v_300s_4d/bsec_iaq.txt>
};
const uint8_t BSEC_CONFIG_33V_LP[] = {
  #include <config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt>
};

const int xOffset = 28;
const int yOffset = OLED_ROTATED ? 0 : 24;

long lastScheduledHourKey = -1;
int lastScheduledMinute = -1;

void checkBsecStatus(Bsec2 &bsec) {
  if (bsec.status < BSEC_OK) {
    Serial.printf("BSEC error code: %d\n", bsec.status);
  } else if (bsec.status > BSEC_OK) {
    Serial.printf("BSEC warning code: %d\n", bsec.status);
  }

  if (bsec.sensor.status < BME68X_OK) {
    Serial.printf("BME68X error code: %d\n", bsec.sensor.status);
  } else if (bsec.sensor.status > BME68X_OK) {
    Serial.printf("BME68X warning code: %d\n", bsec.sensor.status);
  }
}

int getDueScheduledMinute(int currentMinute) {
  int dueMinute = -1;
  const size_t sendMinuteCount = ARRAY_LEN(SEND_MINUTES);
  for (size_t index = 0; index < sendMinuteCount; index++) {
    const int scheduledMinute = SEND_MINUTES[index];
    if (scheduledMinute >= 0 && scheduledMinute <= currentMinute && scheduledMinute > dueMinute) {
      dueMinute = scheduledMinute;
    }
  }
  return dueMinute;
}

void scanI2cDevices() {
  Serial.println("I2C scan:");
  const uint8_t addresses[] = {0x38, 0x3C, 0x76, 0x77};
  bool deviceFound = false;
  for (uint8_t address : addresses) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Found device at 0x%02X\n", address);
      deviceFound = true;
    }
  }
  if (!deviceFound) {
    Serial.println("  No I2C devices found");
  }
}

bool synchronizeNtp() {
  configTzTime(TIME_ZONE, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 10000)) {
    Serial.println("NTP time synchronization failed");
    return false;
  }

  Serial.printf("NTP synchronized: %04d-%02d-%02d %02d:%02d:%02d\n",
                timeInfo.tm_year + 1900,
                timeInfo.tm_mon + 1,
                timeInfo.tm_mday,
                timeInfo.tm_hour,
                timeInfo.tm_min,
                timeInfo.tm_sec);
  return true;
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi already connected: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");

  unsigned long startMillis = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMillis < 30000UL) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected: ");
    Serial.println(WiFi.localIP());
    return synchronizeNtp();
  } else {
    Serial.println("Wi-Fi connection failed");
    return false;
  }
}

void disconnectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("Wi-Fi disconnected");
}

bool getInternalTimestamp(char* timestamp, size_t timestampSize) {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 10000)) {
    return false;
  }

  strftime(timestamp, timestampSize, "%Y-%m-%d_%H:%M", &timeInfo);
  return true;
}

// URL送信関数 (p14: 不快指数 DI を追加)
bool sendMeasurement(float temperature, float humidity, float pressure) {
  char timestamp[20];
  if (!getInternalTimestamp(timestamp, sizeof(timestamp))) {
    Serial.println("ESP32 internal time is not available; measurement was not sent");
    return false;
  }

  // 不快指数 (Discomfort Index) の計算
  float discomfortIndex = 0.81f * temperature + 0.01f * humidity * (0.99f * temperature - 14.3f) + 46.3f;

  // Accuracyが0の時は空文字列をセット
  String normalIaqStr = (!isnan(bme680Iaq) && bme680IaqAccuracy > 0.0F) ? String(bme680Iaq, 2) : "";
  String staticIaqStr = (!isnan(bme680StaticIaq) && bme680StaticIaqAccuracy > 0.0F) ? String(bme680StaticIaq, 2) : "";
  String co2Str       = (!isnan(bme680Co2Equivalent) && bme680Co2Accuracy > 0.0F) ? String(bme680Co2Equivalent, 2) : "";
  String bvocStr      = (!isnan(bme680BvocEquivalent) && bme680BvocAccuracy > 0.0F) ? String(bme680BvocEquivalent, 2) : "";

  // 整数項目
  String gasResStr    = !isnan(bme680GasResistance) ? String(static_cast<uint32_t>(bme680GasResistance)) : "";
  String gasPctStr    = !isnan(bme680GasPercentage) ? String(bme680GasPercentage, 2) : "";
  String stabStr      = !isnan(bme680Stabilization) ? String(static_cast<int>(bme680Stabilization)) : "";
  String runInStr     = !isnan(bme680RunIn) ? String(static_cast<int>(bme680RunIn)) : "";

  String url = String(SHEET_URL)
               + "?p1="  + SHEET_NAME
               + "&p2="  + timestamp
               + "&p3="  + String(temperature, 2)
               + "&p4="  + String(humidity, 2)
               + "&p5="  + String(pressure, 2)
               + "&p6="  + normalIaqStr
               + "&p7="  + staticIaqStr
               + "&p8="  + co2Str
               + "&p9="  + bvocStr
               + "&p10=" + gasResStr
               + "&p11=" + gasPctStr
               + "&p12=" + stabStr
               + "&p13=" + runInStr
               + "&p14=" + String(discomfortIndex, 2);

  Serial.println("Sending URL:");
  Serial.println(url);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000); // タイムアウトを10秒に設定

  if (!http.begin(url)) {
    Serial.println("Could not start HTTP request");
    return false;
  }

  int httpCode = http.GET();
  Serial.printf("Measurement sent: HTTP %d\n", httpCode);
  bool sent = (httpCode >= 200 && httpCode < 300);
  if (httpCode > 0) {
    Serial.println(http.getString());
  }
  http.end();
  return sent;
}

bool initBsec() {
  bsecSensor sensorList[] = {
    BSEC_OUTPUT_IAQ,
    BSEC_OUTPUT_STATIC_IAQ,
    BSEC_OUTPUT_CO2_EQUIVALENT,
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
    BSEC_OUTPUT_RAW_TEMPERATURE,
    BSEC_OUTPUT_RAW_HUMIDITY,
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_RAW_GAS,
    BSEC_OUTPUT_GAS_PERCENTAGE,
    BSEC_OUTPUT_STABILIZATION_STATUS,
    BSEC_OUTPUT_RUN_IN_STATUS,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY
  };

  Serial.printf("Initializing BSEC at I2C address 0x%02X ...\n", BME680_I2C_ADDRESS);
  if (!envSensor.begin(BME680_I2C_ADDRESS, Wire)) {
    checkBsecStatus(envSensor);
    return false;
  }

  const uint8_t *bsecConfig = (BSEC_OPERATION_MODE == BsecOperationMode::ULP)
                                ? BSEC_CONFIG_33V_ULP
                                : BSEC_CONFIG_33V_LP;

  if (!envSensor.setConfig(bsecConfig)) {
    checkBsecStatus(envSensor);
    return false;
  }

  if (BSEC_OPERATION_MODE == BsecOperationMode::ULP) {
    envSensor.setTemperatureOffset(BSEC_INTERNAL_TEMP_OFFSET_ULP);
  } else {
    envSensor.setTemperatureOffset(BSEC_INTERNAL_TEMP_OFFSET_LP);
  }

  if (!envSensor.updateSubscription(sensorList, ARRAY_LEN(sensorList), BSEC_SAMPLE_RATE)) {
    checkBsecStatus(envSensor);
    return false;
  }

  return true;
}

void processBsecOutputs() {
  if (!envSensor.run()) return;

  const bsecOutputs *outputs = envSensor.getOutputs();
  if (outputs == nullptr) return;

  for (uint8_t index = 0; index < outputs->nOutputs; index++) {
    const bsecData output = outputs->output[index];
    switch (output.sensor_id) {
      case BSEC_OUTPUT_IAQ:
        bme680Iaq = output.signal;
        bme680IaqAccuracy = static_cast<float>(output.accuracy);
        break;
      case BSEC_OUTPUT_STATIC_IAQ:
        bme680StaticIaq = output.signal;
        bme680StaticIaqAccuracy = static_cast<float>(output.accuracy);
        break;
      case BSEC_OUTPUT_CO2_EQUIVALENT:
        bme680Co2Equivalent = output.signal;
        bme680Co2Accuracy = static_cast<float>(output.accuracy);
        break;
      case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
        bme680BvocEquivalent = output.signal;
        bme680BvocAccuracy = static_cast<float>(output.accuracy);
        break;
      case BSEC_OUTPUT_RAW_TEMPERATURE:
        bme680RawTemp = output.signal;
        break;
      case BSEC_OUTPUT_RAW_HUMIDITY:
        bme680RawHum = output.signal;
        break;
      case BSEC_OUTPUT_RAW_PRESSURE:
        bme680RawPressure = output.signal;
        break;
      case BSEC_OUTPUT_RAW_GAS:
        bme680GasResistance = output.signal;
        break;
      case BSEC_OUTPUT_GAS_PERCENTAGE:
        bme680GasPercentage = output.signal;
        break;
      case BSEC_OUTPUT_STABILIZATION_STATUS:
        bme680Stabilization = output.signal;
        break;
      case BSEC_OUTPUT_RUN_IN_STATUS:
        bme680RunIn = output.signal;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
        bme680CompTemp = output.signal;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
        bme680CompHum = output.signal;
        break;
      default:
        break;
    }
  }
}

void printDebugInfo(bool hasCurrentTime, const struct tm &currentTime,
                    float tempDisplay, float humDisplay, float pressDisplay) {
  float di = 0.81f * tempDisplay + 0.01f * humDisplay * (0.99f * tempDisplay - 14.3f) + 46.3f;

  Serial.println("\n--- Sensor data (BME680 + BSEC2 Full Outputs) ---");
  if (hasCurrentTime) {
    Serial.printf("ESP32 internal time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  currentTime.tm_year + 1900, currentTime.tm_mon + 1, currentTime.tm_mday,
                  currentTime.tm_hour, currentTime.tm_min, currentTime.tm_sec);
  } else {
    Serial.println("ESP32 internal time: unavailable");
  }
  Serial.println("Temperature:");
  Serial.printf("  Raw: %.2f C\n", bme680RawTemp);
  Serial.printf("  BSEC Compensated: %.2f C\n", bme680CompTemp);
  Serial.printf("  Display Offsetted: %.2f C\n", tempDisplay);
  Serial.println("Humidity:");
  Serial.printf("  Raw: %.2f %%\n", bme680RawHum);
  Serial.printf("  BSEC Compensated: %.2f %%\n", bme680CompHum);
  Serial.printf("  Display Offsetted: %.2f %%\n", humDisplay);
  Serial.println("Pressure:");
  Serial.printf("  Raw: %.2f hPa\n", bme680RawPressure);
  Serial.printf("  Corrected: %.2f hPa\n", pressDisplay);
  Serial.println("Calculated Index:");
  Serial.printf("  Discomfort Index (DI): %.2f\n", di);
  Serial.println("Air Quality & Gas:");
  Serial.printf("  Normal IAQ: %.2f (Accuracy: %.0f)\n", bme680Iaq, bme680IaqAccuracy);
  Serial.printf("  Static IAQ: %.2f (Accuracy: %.0f)\n", bme680StaticIaq, bme680StaticIaqAccuracy);
  Serial.printf("  CO2 Equivalent: %.2f ppm (Accuracy: %.0f)\n", bme680Co2Equivalent, bme680Co2Accuracy);
  Serial.printf("  bVOC Equivalent: %.2f ppm (Accuracy: %.0f)\n", bme680BvocEquivalent, bme680BvocAccuracy);
  Serial.printf("  Gas Resistance: %.0f Ohm\n", bme680GasResistance);
  Serial.printf("  Gas Percentage: %.2f %%\n", bme680GasPercentage);
  Serial.println("Status:");
  Serial.printf("  Stabilization: %.0f\n", bme680Stabilization);
  Serial.printf("  Run-in: %.0f\n", bme680RunIn);
  Serial.println("-------------------------------------------------");
}

void updateOledDisplay(float tempDisplay, float humDisplay, float pressDisplay) {
  static bool isFirstUpdate = true;

  // 2回目以降の更新時、ENABLE_OLED が false なら画面を消灯して処理をスキップ
  if (!isFirstUpdate && !ENABLE_OLED) {
    u8g2.clearBuffer();
    u8g2.sendBuffer();
    return;
  }

  isFirstUpdate = false;

  char line[24];
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.clearBuffer();

  if (!isnan(tempDisplay)) {
    snprintf(line, sizeof(line), "%.1f C", tempDisplay);
    u8g2.drawStr(xOffset + 2, yOffset + 8, line);
  } else {
    u8g2.drawStr(xOffset + 2, yOffset + 8, "NG");
  }

  if (bme680Ready) {
    snprintf(line, sizeof(line), "%.1f %%", humDisplay);
    u8g2.drawStr(xOffset + 2, yOffset + 18, line);
    snprintf(line, sizeof(line), "%.0f hPa", pressDisplay);
    u8g2.drawStr(xOffset + 2, yOffset + 28, line);
  } else {
    u8g2.drawStr(xOffset + 2, yOffset + 18, "NG");
    u8g2.drawStr(xOffset + 2, yOffset + 28, "NG");
  }

  float currentIaq = USE_STATIC_IAQ ? bme680StaticIaq : bme680Iaq;
  if (!isnan(currentIaq)) {
    snprintf(line, sizeof(line), "%sIAQ %.0f", USE_STATIC_IAQ ? "s" : "", currentIaq);
    u8g2.drawStr(xOffset + 2, yOffset + 38, line);
  } else {
    u8g2.drawStr(xOffset + 2, yOffset + 38, "IAQ NG");
  }

  u8g2.sendBuffer();
}

void handleScheduledSend(const struct tm &currentTime, float tempDisplay, float humDisplay, float pressDisplay) {
  const int scheduledMinute = getDueScheduledMinute(currentTime.tm_min);
  if (scheduledMinute < 0) return;

  long currentHourKey = (currentTime.tm_year + 1900L) * 1000000L
                        + (currentTime.tm_mon + 1L) * 10004L
                        + currentTime.tm_mday * 100L
                        + currentTime.tm_hour;

  if (currentHourKey != lastScheduledHourKey || scheduledMinute != lastScheduledMinute) {
    lastScheduledHourKey = currentHourKey;
    lastScheduledMinute = scheduledMinute;
    Serial.println("Scheduled measurement started");

    if (!isnan(tempDisplay) && !isnan(humDisplay) && !isnan(pressDisplay)) {
      if (!connectWiFi()) {
        Serial.println("Measurement skipped: Wi-Fi connection failed");
      } else if (!sendMeasurement(tempDisplay, humDisplay, pressDisplay)) {
        Serial.println("Measurement failed");
      }
      disconnectWiFi();
    } else {
      Serial.printf("Measurement skipped: incomplete sensor data (temperature=%.2f, humidity=%.2f, pressure=%.2f)\n",
                    tempDisplay, humDisplay, pressDisplay);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Initial Wi-Fi connection");
  connectWiFi();
  disconnectWiFi();

  const size_t sendMinuteCount = ARRAY_LEN(SEND_MINUTES);
  Serial.print("Configured send minutes: ");
  for (size_t index = 0; index < sendMinuteCount; index++) {
    if (index > 0) Serial.print(", ");
    Serial.print(SEND_MINUTES[index]);
    if (SEND_MINUTES[index] < 0 || SEND_MINUTES[index] >= 60) {
      Serial.printf("SEND_MINUTES[%u] must be between 0 and 59\n", static_cast<unsigned int>(index));
    }
  }
  Serial.println();

  u8g2.setDisplayRotation(OLED_ROTATED ? U8G2_R2 : U8G2_R0);
  u8g2.begin();
  u8g2.setContrast(255);

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(100);
  scanI2cDevices();

  bme680Ready = initBsec();
  Serial.printf("BME680/BSEC: %s\n", bme680Ready ? "OK" : "NG");

  if (!bme680Ready) {
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.clearBuffer();
    u8g2.drawStr(xOffset + 2, yOffset + 14, "Sensor Error");
    u8g2.sendBuffer();
  }
}

void loop() {
  // 1. BSEC2の処理は毎ループ呼び出す
  if (bme680Ready) {
    processBsecOutputs();
  }

  // 2. 表示・送信処理（初回即時実行、以降は動作モード指定の間隔）
  static unsigned long lastDisplayUpdateMs = 0;
  if (lastDisplayUpdateMs == 0 || millis() - lastDisplayUpdateMs >= SENSOR_DISPLAY_INTERVAL_MS) {
    lastDisplayUpdateMs = millis();

    float bmeTemperatureDisplay = NAN;
    float humidityValue = NAN;
    float pressure = NAN;

    if (bme680Ready && !isnan(bme680CompTemp)) {
      bmeTemperatureDisplay = bme680CompTemp + ((BSEC_OPERATION_MODE == BsecOperationMode::ULP)
                                                 ? DISPLAY_TEMP_OFFSET_ULP
                                                 : DISPLAY_TEMP_OFFSET_LP);
      float baseHum = !isnan(bme680CompHum) ? bme680CompHum : bme680RawHum;
      humidityValue = baseHum * (1.0F + HUM_OFFSET_RATE);
      pressure = bme680RawPressure + PRESS_OFFSET;
    }

    struct tm currentTime;
    const bool hasCurrentTime = getLocalTime(&currentTime, 10);

    if (DEBUG_MODE) {
      printDebugInfo(hasCurrentTime, currentTime, bmeTemperatureDisplay, humidityValue, pressure);
    }

    updateOledDisplay(bmeTemperatureDisplay, humidityValue, pressure);

    if (hasCurrentTime) {
      handleScheduledSend(currentTime, bmeTemperatureDisplay, humidityValue, pressure);
    }
  }
}