#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <U8g2lib.h>
#include <bsec2.h>
#include "config.h"

// 0.42インチ OLED ピン設定 (SDA: GPIO5, SCL: GPIO6)
#define OLED_SDA 5
#define OLED_SCL 6
#define OLED_RESET U8X8_PIN_NONE

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RESET, OLED_SCL, OLED_SDA);
Bsec2 envSensor;
bool bme680Ready = false;
float bme680Iaq = NAN;
float bme680IaqAccuracy = NAN;
float bme680GasResistance = NAN;
float bme680Stabilization = NAN;
float bme680RunIn = NAN;

enum class BsecOperationMode {
  ULP,
  LP
};

// 動作モードはconfig.hのBSEC_USE_ULPで変更する。
constexpr BsecOperationMode BSEC_OPERATION_MODE =
  BSEC_USE_ULP ? BsecOperationMode::ULP : BsecOperationMode::LP;

// 下記行は手動変更不可
constexpr float BSEC_SAMPLE_RATE =
    BSEC_OPERATION_MODE == BsecOperationMode::ULP
      ? BSEC_SAMPLE_RATE_ULP
      : BSEC_SAMPLE_RATE_LP;

// BSEC実行とLCD更新の間隔。モードの測定周期に合わせて自動選択する。
constexpr uint32_t SENSOR_DISPLAY_INTERVAL_MS =
    BSEC_OPERATION_MODE == BsecOperationMode::ULP
      ? 300000UL
      : 3000UL;

// BME680を3.3 Vで使用するため、選択した動作モードに対応するBSEC設定を使用する。
const uint8_t BSEC_CONFIG_33V_ULP[] = {
  #include <config/bme680/bme680_iaq_33v_300s_4d/bsec_iaq.txt>
};
const uint8_t BSEC_CONFIG_33V_LP[] = {
  #include <config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt>
};

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

// 画面表示用キャリブレーション値 (72x40画面用)
const int xOffset = 28;
const int yOffset = OLED_ROTATED ? 0 : 24;

long lastScheduledHourKey = -1;
int lastScheduledMinute = -1;

int getDueScheduledMinute(int currentMinute) {
  int dueMinute = -1;
  const size_t sendMinuteCount = sizeof(SEND_MINUTES) / sizeof(SEND_MINUTES[0]);
  for (size_t index = 0; index < sendMinuteCount; index++) {
    const int scheduledMinute = SEND_MINUTES[index];
    if (scheduledMinute >= 0 && scheduledMinute <= currentMinute
        && scheduledMinute > dueMinute) {
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

bool synchronizeNtp();

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

bool getInternalTimestamp(char* timestamp, size_t timestampSize) {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 10000)) {
    return false;
  }

  strftime(timestamp, timestampSize, "%Y-%m-%d_%H:%M", &timeInfo);
  return true;
}

bool sendMeasurement(float temperature, float humidity, float pressure,
                     float iaq, float runIn) {
  char timestamp[20];
  if (!getInternalTimestamp(timestamp, sizeof(timestamp))) {
    Serial.println("ESP32 internal time is not available; measurement was not sent");
    return false;
  }

  float discomfortIndex = 0.81F * temperature
                          + 0.01F * humidity * (0.99F * temperature - 14.3F)
                          + 46.3F;
  String iaqValue = isnan(iaq) || isnan(runIn) || runIn <= 0.0F
                      ? ""
                      : String(iaq, 2);
  String url = String(SHEET_URL)
               + "?p7=" + SHEET_NAME
               + "&p1=" + timestamp
               + "&p2=" + String(temperature, 2)
               + "&p3=" + String(humidity, 2)
               + "&p4=" + String(pressure, 2)
               + "&p5=" + iaqValue
               + "&p6=" + String(discomfortIndex, 2);

  Serial.println("Sending URL:");
  Serial.println(url);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(url)) {
    Serial.println("Could not start HTTP request");
    return false;
  }

  int httpCode = http.GET();
  Serial.printf("Measurement sent: HTTP %d\n", httpCode);
  bool sent = httpCode >= 200 && httpCode < 300;
  if (httpCode > 0) {
    Serial.println(http.getString());
  }
  http.end();
  return sent;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Initial Wi-Fi connection");
  connectWiFi();
  disconnectWiFi();

  const size_t sendMinuteCount = sizeof(SEND_MINUTES) / sizeof(SEND_MINUTES[0]);
  Serial.print("Configured send minutes: ");
  for (size_t index = 0; index < sendMinuteCount; index++) {
    if (index > 0) {
      Serial.print(", ");
    }
    Serial.print(SEND_MINUTES[index]);
    if (SEND_MINUTES[index] < 0 || SEND_MINUTES[index] >= 60) {
      Serial.printf("SEND_MINUTES[%u] must be between 0 and 59\n",
                    static_cast<unsigned int>(index));
    }
  }
  Serial.println();

  // OLED初期化
  u8g2.setDisplayRotation(OLED_ROTATED ? U8G2_R2 : U8G2_R0);
  u8g2.begin();
  u8g2.setContrast(255);

  // OLED初期化後にI2Cバスを再設定し、センサーと同じピンを確実に使用する
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setTimeOut(100);
  scanI2cDevices();

  bsecSensor sensorList[] = {
    BSEC_OUTPUT_IAQ,
    BSEC_OUTPUT_RAW_TEMPERATURE,
    BSEC_OUTPUT_RAW_HUMIDITY,
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_RAW_GAS,
    BSEC_OUTPUT_STABILIZATION_STATUS,
    BSEC_OUTPUT_RUN_IN_STATUS,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY
  };

  Serial.printf("Initializing BSEC at I2C address 0x%02X ...\n", BME680_I2C_ADDRESS);
  if (!envSensor.begin(BME680_I2C_ADDRESS, Wire)) {
    checkBsecStatus(envSensor);
    bme680Ready = false;
  } else {
    const uint8_t *bsecConfig =
        BSEC_OPERATION_MODE == BsecOperationMode::ULP
          ? BSEC_CONFIG_33V_ULP
          : BSEC_CONFIG_33V_LP;

    if (!envSensor.setConfig(bsecConfig)) {
      checkBsecStatus(envSensor);
      bme680Ready = false;
    } else {
      if (BSEC_OPERATION_MODE == BsecOperationMode::ULP) {
        envSensor.setTemperatureOffset(TEMP_OFFSET_ULP);
      } else {
        envSensor.setTemperatureOffset(TEMP_OFFSET_LP);
      }

      if (!envSensor.updateSubscription(sensorList, ARRAY_LEN(sensorList), BSEC_SAMPLE_RATE)) {
        checkBsecStatus(envSensor);
        bme680Ready = false;
      } else {
        bme680Ready = true;
      }
    }
  }

  Serial.printf("BME680/BSEC: %s\n", bme680Ready ? "OK" : "NG");

  if (!bme680Ready) {
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.clearBuffer();
    u8g2.drawStr(xOffset + 2, yOffset + 14, "Sensor Error");
    u8g2.sendBuffer();
  }
}

void loop() {
  char line[24];
  float bmeTemperature = NAN;
  float bmeTemperatureCompensated = NAN;
  float bmeTemperatureDisplay = NAN;
  float rawHumidity = NAN;
  float humidityValue = NAN;
  float rawPressure = NAN;
  float pressure = NAN;

  if (bme680Ready) {
    if (envSensor.run()) {
      const bsecOutputs *outputs = envSensor.getOutputs();
      if (outputs != nullptr) {
        for (uint8_t index = 0; index < outputs->nOutputs; index++) {
          const bsecData output = outputs->output[index];
          switch (output.sensor_id) {
            case BSEC_OUTPUT_IAQ:
              bme680Iaq = output.signal;
              bme680IaqAccuracy = static_cast<float>(output.accuracy);
              break;
            case BSEC_OUTPUT_RAW_TEMPERATURE:
              bmeTemperature = output.signal;
              break;
            case BSEC_OUTPUT_RAW_HUMIDITY:
              rawHumidity = output.signal;
              break;
            case BSEC_OUTPUT_RAW_PRESSURE:
              // BSEC2 already converts pressure to hPa before exposing this output.
              rawPressure = output.signal;
              break;
            case BSEC_OUTPUT_RAW_GAS:
              bme680GasResistance = output.signal;
              break;
            case BSEC_OUTPUT_STABILIZATION_STATUS:
              bme680Stabilization = output.signal;
              break;
            case BSEC_OUTPUT_RUN_IN_STATUS:
              bme680RunIn = output.signal;
              break;
            case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
              bmeTemperatureCompensated = output.signal;
              break;
            case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
              if (isnan(rawHumidity)) {
                rawHumidity = output.signal;
              }
              break;
            default:
              break;
          }
        }
      }
    }

    if (!isnan(bmeTemperatureCompensated)) {
      bmeTemperatureDisplay = bmeTemperatureCompensated
                              + (BSEC_OPERATION_MODE == BsecOperationMode::ULP
                                   ? TEMP_OFFSET_BME680_ULP
                                   : TEMP_OFFSET_BME680_LP);
      humidityValue = rawHumidity * (1.0F + HUM_OFFSET_RATE);
      pressure = rawPressure + PRESS_OFFSET;
    }
  }

  struct tm currentTime;
  const bool hasCurrentTime = getLocalTime(&currentTime, 100);

  if (DEBUG_MODE) {
    Serial.println();
    Serial.println("--- Sensor data ---");
    if (hasCurrentTime) {
      Serial.printf("ESP32 internal time: %04d-%02d-%02d %02d:%02d:%02d\n",
                    currentTime.tm_year + 1900,
                    currentTime.tm_mon + 1,
                    currentTime.tm_mday,
                    currentTime.tm_hour,
                    currentTime.tm_min,
                    currentTime.tm_sec);
    } else {
      Serial.println("ESP32 internal time: unavailable");
    }
    Serial.println("Temperature:");
    Serial.printf("  BME680 raw: %.2f C\n", bmeTemperature);
    Serial.printf("  BME680 corrected: %.2f C\n",
                  bmeTemperatureCompensated);
    Serial.printf("  BME680 display: %.2f C\n", bmeTemperatureDisplay);
    Serial.println("Humidity:");
    Serial.printf("  BME680 raw: %.2f %%\n", rawHumidity);
    Serial.printf("  BME680 corrected: %.2f %%\n", humidityValue);
    Serial.println("Pressure:");
    Serial.printf("  BME680 raw: %.2f hPa\n", rawPressure);
    Serial.printf("  BME680 corrected: %.2f hPa\n", pressure);
    Serial.println("Gas sensor:");
    Serial.printf("  IAQ: %.2f, accuracy: %.0f\n", bme680Iaq, bme680IaqAccuracy);
    Serial.printf("  Gas resistance: %.0f ohm\n", bme680GasResistance);
    Serial.printf("  Stabilization: %.0f, run-in: %.0f\n",
            bme680Stabilization, bme680RunIn);
    Serial.println("-------------------");
  }

  u8g2.setFont(u8g2_font_6x10_tr);
  if (!isnan(bmeTemperatureDisplay)) {
    snprintf(line, sizeof(line), "%.1f C", bmeTemperatureDisplay);
    u8g2.drawStr(xOffset + 2, yOffset + 8, line);
  } else {
    u8g2.drawStr(xOffset + 2, yOffset + 8, "NG");
  }

  if (bme680Ready) {
    snprintf(line, sizeof(line), "%.1f %%", humidityValue);
    u8g2.drawStr(xOffset + 2, yOffset + 18, line);
  } else {
    u8g2.drawStr(xOffset + 2, yOffset + 18, "NG");
  }

  if (bme680Ready) {
    snprintf(line, sizeof(line), "%.0f hPa", pressure);
    u8g2.drawStr(xOffset + 2, yOffset + 28, line);
  } else {
    u8g2.drawStr(xOffset + 2, yOffset + 28, "NG");
  }

  if (!isnan(bme680Iaq)) {
    snprintf(line, sizeof(line), "IAQ %.0f", bme680Iaq);
    u8g2.drawStr(xOffset + 2, yOffset + 38, line);
  } else {
    u8g2.drawStr(xOffset + 2, yOffset + 38, "IAQ NG");
  }

  u8g2.sendBuffer();

  if (hasCurrentTime) {
    const int scheduledMinute = getDueScheduledMinute(currentTime.tm_min);
    if (scheduledMinute < 0) {
      delay(SENSOR_DISPLAY_INTERVAL_MS);
      return;
    }

    long currentHourKey = (currentTime.tm_year + 1900L) * 1000000L
                          + (currentTime.tm_mon + 1L) * 10000L
                          + currentTime.tm_mday * 100L
                          + currentTime.tm_hour;

    if (currentHourKey != lastScheduledHourKey
        || scheduledMinute != lastScheduledMinute) {
      lastScheduledHourKey = currentHourKey;
      lastScheduledMinute = scheduledMinute;
      Serial.println("Scheduled measurement started");
      if (!isnan(bmeTemperatureDisplay) && !isnan(humidityValue) && !isnan(pressure)) {
        if (!connectWiFi()) {
          Serial.println("Measurement skipped: Wi-Fi connection failed");
        } else if (!sendMeasurement(bmeTemperatureDisplay, humidityValue, pressure,
                  bme680Iaq, bme680RunIn)) {
          Serial.println("Measurement failed");
        }
        disconnectWiFi();
      } else {
        Serial.printf("Measurement skipped: incomplete sensor data (temperature=%.2f, humidity=%.2f, pressure=%.2f)\n",
                      bmeTemperatureDisplay, humidityValue, pressure);
      }
    }
  }

  delay(SENSOR_DISPLAY_INTERVAL_MS);
}