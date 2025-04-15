#include <Wire.h>

// ===== Адреса устройств =====
#define SLAVE_055 0x55  // ESP32 (Slave), откуда мастер получает команды
#define SLAVE_08  0x08  // Второе устройство, куда мастер пересылает команду

// ===== Период опроса (мс) =====
const unsigned long POLL_INTERVAL = 500;  // как часто (в мс) опрашиваем 0x55 и 0x08

// ===== Машина состояний для примера =====
enum MasterState {
  MS_POLL_055,        // 1) спрашиваем у 0x55: «есть ли у тебя команда?»
  MS_PROCESS_COMMAND, // 2) разбираем команду, проверяем формат
  MS_SEND_TO_08,      // 3) отправляем полученную команду на 0x08
  MS_WAIT_08,         // 4) читаем короткий ответ от 0x08 (до 4 байт, например "TRUE")
  MS_IDLE             // 5) «ожидание»/ничего не делаем (можно не использовать)
};

MasterState masterState = MS_POLL_055; 
unsigned long lastPollTime = 0;

// Буфер для принятой команды и команда к отправке
String commandFrom055 = "";
String commandToSend  = "";

void setup() {
  Serial.begin(115200);
  Wire.begin();  // Мастер по I2C
  Serial.println("I2C Master started (no sensors).");
}

// Функция чтения строки из 0x55 (до 32 байт или до '\0')
String readStringFromSlave(uint8_t slaveAddr, size_t maxBytes = 32) {
  Wire.requestFrom(slaveAddr, (uint8_t)maxBytes);

  char buffer[33]; // +1 для завершающего нуля
  int i = 0;
  while (Wire.available() && i < (int)maxBytes) {
    char c = Wire.read();
    if (c == '\0') { 
      // Если встретили нуль-терминатор, завершаем строку
      break;
    }
    buffer[i++] = c;
  }
  buffer[i] = '\0';
  return String(buffer);
}

void loop() {
  switch (masterState) {

    // --------------------------------------------------
    // 1) Пытаемся получить команду от 0x55
    // --------------------------------------------------
    case MS_POLL_055: {
      if (millis() - lastPollTime >= POLL_INTERVAL) {
        commandFrom055 = readStringFromSlave(SLAVE_055, 32);
        Serial.print("Received from 0x55: ");
        Serial.println(commandFrom055);

        // Если получили что-то осмысленное, переходим к разбору
        if (commandFrom055.length() > 0) {
          masterState = MS_PROCESS_COMMAND;
        }
        lastPollTime = millis();
      }
      break;
    }

    // --------------------------------------------------
    // 2) Разбираем команду (MOVE X, TURN X, SETSPEED X, STOP)
    // --------------------------------------------------
    case MS_PROCESS_COMMAND: {
      // Приведём команду к верхнему регистру (для сравнения)
      // Но аккуратно, чтобы не стереть числовую часть.
      // Проще сначала найти пробел, если есть.
      int spaceIndex = commandFrom055.indexOf(' ');
      if (spaceIndex > 0) {
        // Есть пробел: формат "COMMAND VALUE"
        String cmdType  = commandFrom055.substring(0, spaceIndex);
        String cmdValue = commandFrom055.substring(spaceIndex + 1);

        cmdType.toUpperCase();
        // Проверяем валидные типы
        if (cmdType == "MOVE" || cmdType == "TURN" || cmdType == "SETSPEED") {
          commandToSend = cmdType + " " + cmdValue; 
          Serial.print("Parsed command for 0x08: ");
          Serial.println(commandToSend);
          masterState = MS_SEND_TO_08;
        } else {
          Serial.println("Unrecognized command: " + cmdType);
          // Возвращаемся к опросу
          masterState = MS_POLL_055;
        }
      } else {
        // Нет пробела: может быть просто STOP
        String cmdUp = commandFrom055;
        cmdUp.toUpperCase();
        if (cmdUp == "STOP") {
          commandToSend = "STOP";
          Serial.println("Parsed command STOP for 0x08.");
          masterState = MS_SEND_TO_08;
        } else {
          Serial.println("Unknown/empty command: " + commandFrom055);
          masterState = MS_POLL_055;
        }
      }
      break;
    }

    // --------------------------------------------------
    // 3) Отправляем команду на 0x08
    // --------------------------------------------------
    case MS_SEND_TO_08: {
      Serial.print("Sending to 0x08: ");
      Serial.println(commandToSend);

      Wire.beginTransmission(SLAVE_08);
      Wire.write(commandToSend.c_str());
      byte error = Wire.endTransmission();

      if (error == 0) {
        Serial.println("Command successfully sent to 0x08.");
        masterState = MS_WAIT_08;
        lastPollTime = millis();
      } else {
        Serial.print("Error sending command to 0x08: ");
        Serial.println(error);
        // Возвращаемся к опросу команд
        masterState = MS_POLL_055;
      }
      break;
    }

    // --------------------------------------------------
    // 4) Читаем ответ от 0x08 (до 4 байт, например "TRUE")
    // --------------------------------------------------
    case MS_WAIT_08: {
      if (millis() - lastPollTime >= POLL_INTERVAL) {
        // Запросим 4 байта (или меньше)
        Wire.requestFrom(SLAVE_08, 4U);
        char resp[5];
        int i = 0;
        while (Wire.available() && i < 4) {
          resp[i++] = Wire.read();
        }
        resp[i] = '\0';

        String status = String(resp);
        status.trim();

        Serial.print("Response from 0x08: ");
        Serial.println(status);

        if (status.equalsIgnoreCase("TRUE")) {
          Serial.println("0x08 confirmed the command.");
        } else {
          Serial.println("0x08 did NOT confirm the command (or no response).");
        }
        // Снова ждём следующую команду
        masterState = MS_POLL_055;
      }
      break;
    }

    // --------------------------------------------------
    // 5) Просто состояние «простаиваем» (можно не использовать)
    // --------------------------------------------------
    case MS_IDLE:
    default:
      // Можно что-то делать по желанию
      break;
  }
}
