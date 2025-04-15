#include <Wire.h>
#include <Adafruit_BME280.h>
#include <SparkFun_APDS9960.h>
#include <math.h>  // Для isnan()

// ===== Адреса ведомых устройств =====
#define SLAVE_055 0x55  // Устройство, которому отправляем данные сенсоров и откуда получаем команду
#define SLAVE_08 0x08   // Устройство, которому пересылаем управляющую команду и откуда получаем статус

// ===== Периоды задержек (мс) =====
const unsigned long POLL_INTERVAL = 500;   // интервал опроса
const unsigned long COMMAND_DELAY = 1000;  // задержка между этапами

// ===== Перечисление состояний мастера =====
enum MasterState {
  MS_SEND_SENSOR,      // Отправляем данные сенсоров на 0x55
  MS_POLL_055,         // Опрашиваем 0x55 на наличие команды
  MS_PROCESS_COMMAND,  // Парсим полученную команду от 0x55
  MS_SEND_TO_08,       // Отправляем команду на устройство 0x08
  MS_WAIT_08,          // Опрашиваем 0x08 на получение статуса (ожидаем ровно 4 байта)
  MS_DONE              // Цикл завершён
};

MasterState masterState = MS_SEND_SENSOR;
unsigned long lastPollTime = 0;

String sensorData = "";
String commandFrom055 = "";
String commandToSend = "";

// ===== Объекты датчиков =====
Adafruit_BME280 bme;                           // BME280 (I²C; адрес 0x76 или 0x77)
SparkFun_APDS9960 apds = SparkFun_APDS9960();  // APDS-9960

void setup() {
  Serial.begin(115200);
  Serial.println("I2C Master started with sensors.");
  Wire.begin();  // Инициализация I2C в режиме мастера
  delay(COMMAND_DELAY);

  // Инициализация BME280
  if (!bme.begin(0x76)) {
    Serial.println("Ошибка инициализации BME280! Проверьте соединения.");
    // Если сенсоры не доступны, цикл не отправляет данные
  }

  // Инициализация APDS-9960
  if (apds.init()) {
    Serial.println("APDS-9960 успешно инициализирован.");
  } else {
    Serial.println("Ошибка инициализации APDS-9960!");
    // Если сенсор не инициализирован, дальнейшая отправка данных не будет производиться
  }
  apds.enableLightSensor(false);
}

void loop() {
  switch (masterState) {
    case MS_SEND_SENSOR:
      {
        // Считываем данные с датчиков
        float temperature = bme.readTemperature();
        float pressure = bme.readPressure() / 100.0F;  // в hPa
        float humidity = bme.readHumidity();
        uint16_t ambientLight = 0;

        // Проверяем корректность данных от BME280
        if (isnan(temperature) || isnan(pressure) || isnan(humidity)) {
          Serial.println("Сенсоры BME280 недоступны. Данные не отправляются.");
          // delay(2000);
          // Переходим к следующему циклу и пытаемся повторно прочитать данные
          sensorData = "Module is not connected";
          bme.begin(0x76);
        }

        else if (!apds.readAmbientLight(ambientLight)) {
          Serial.println("Сенсор APDS-9960 недоступен. Данные по освещенности не получены.");
          // delay(2000);
          sensorData = "Module is not connected";
          apds.init();

        } else {
          // Формируем строку с данными сенсоров
          sensorData = "Temp:" + String(temperature, 1) + "C,";
          sensorData += "Pressure:" + String(pressure, 1) + "hPa,";
          sensorData += "Humidity:" + String(humidity, 1) + "%,";
          sensorData += "Lux:" + String(ambientLight);
          sensorData += '\0';  // добавляем завершающий символ
        }

        Serial.print("Отправляем данные сенсоров на 0x55: ");
        Serial.println(sensorData);

        // Отправляем данные на устройство 0x55
        Wire.beginTransmission(SLAVE_055);
        Wire.write((const uint8_t*)sensorData.c_str(), sensorData.length());
        byte error = Wire.endTransmission();
        if (error == 0) {
          Serial.println("Данные сенсоров успешно отправлены.");
          masterState = MS_POLL_055;
          lastPollTime = millis();
        } else {
          Serial.print("Ошибка отправки данных сенсоров на 0x55: ");
          Serial.println(error);
          // Пытаемся повторить отправку
          masterState = MS_POLL_055;
        }
        delay(COMMAND_DELAY);
        break;
      }

    case MS_POLL_055:
      {
        // Опрашиваем устройство 0x55 на наличие управляющей команды
        if (millis() - lastPollTime >= POLL_INTERVAL) {
          Wire.requestFrom(SLAVE_055, 32);  // запрашиваем до 32 байт
          String response = "";
          while (Wire.available()) {
            char c = Wire.read();
            if (c == '\0') break;  // прекращаем, если встретили завершающий символ
            response += c;
          }
          response.trim();
          Serial.print("Получено от 0x55: ");
          Serial.println(response);
          if (response.length() > 0 && response != "0 Packets.") {
            commandFrom055 = response;
            masterState = MS_PROCESS_COMMAND;
          } else {
            Serial.println("Команда от 0x55 отсутствует. Повтор отправки данных сенсоров.");
            masterState = MS_SEND_SENSOR;
          }
          lastPollTime = millis();
        }
        break;
      }

    case MS_PROCESS_COMMAND:
      {
        // Ожидаем, что команда имеет формат "MOVE X" или "TURN X"
        int spaceIndex = commandFrom055.indexOf(' ');
        if (spaceIndex > 0) {
          String cmdType = commandFrom055.substring(0, spaceIndex);
          String cmdValue = commandFrom055.substring(spaceIndex + 1);
          if (cmdType.equalsIgnoreCase("MOVE") || cmdType.equalsIgnoreCase("TURN")) {
            commandToSend = cmdType + " " + cmdValue;
            Serial.print("Сформирована команда для 0x08: ");
            Serial.println(commandToSend);
            masterState = MS_SEND_TO_08;
          } else {
            Serial.println("Некорректный тип команды от 0x55. Пропускаем.");
            masterState = MS_SEND_SENSOR;
          }
        } else {
          Serial.println("Неверный формат команды от 0x55. Пропускаем.");
          masterState = MS_SEND_SENSOR;
        }
        break;
      }

    case MS_SEND_TO_08:
      {
        // Отправляем сформированную команду на устройство 0x08
        Serial.print("Отправляем команду на 0x08: ");
        Serial.println(commandToSend);

        Wire.beginTransmission(SLAVE_08);
        Wire.write(commandToSend.c_str());
        byte error = Wire.endTransmission();
        if (error == 0) {
          Serial.println("Команда успешно отправлена на 0x08.");
          masterState = MS_WAIT_08;
          lastPollTime = millis();
        } else {
          Serial.print("Ошибка отправки команды на 0x08: ");
          Serial.println(error);
          masterState = MS_SEND_SENSOR;
        }
        delay(COMMAND_DELAY);
        break;
      }

    case MS_WAIT_08:
      {
        // Опрашиваем устройство 0x08 на статус: ожидаем ровно 4 байта (например, "TRUE")
        if (millis() - lastPollTime >= POLL_INTERVAL) {
          Wire.requestFrom(SLAVE_08, 4);
          char statusResponse[5] = { 0 };  // массив для 4 символов + завершающий '\0'
          for (int i = 0; i < 4; i++) {
            if (Wire.available()) {
              statusResponse[i] = Wire.read();
            }
          }
          String response = String(statusResponse);
          response.trim();
          Serial.print("Получено от 0x08: ");
          Serial.println(response);
          if (response.equalsIgnoreCase("TRUE")) {
            Serial.println("Команда выполнена 0x08.");
            masterState = MS_DONE;
          } else {
            Serial.println("Команда не подтверждена 0x08. Продолжаем опрос.");
          }
          lastPollTime = millis();
        }
        break;
      }

    case MS_DONE:
      {
        Serial.println("Цикл команды завершён.");
        // Сброс переменных и обновление значений датчиков (например, для имитации)
        commandFrom055 = "";
        commandToSend = "";
        // Здесь можно обновлять реальные значения датчиков или продолжать считывать их в реальном времени
        delay(5000);  // пауза перед новым циклом
        masterState = MS_SEND_SENSOR;
        break;
      }
  }
}
