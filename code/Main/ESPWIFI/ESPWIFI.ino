 #include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>

const char* ssid     = "Agricultural Robot";
const char* password = "nenavishyZaklVsoshRT25";

WebServer server(80);

// Текущая команда и её значение
String currentAction = "";
String currentValue  = "";
  
// ---------------------------------------------------
// Фильтрация строки: оставляет только англ. буквы, цифры и пробел
// ---------------------------------------------------
String filterMessage(const String &msg) {
  String filtered;
  for (int i = 0; i < msg.length(); i++) {
    char c = msg.charAt(i);
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        (c == ' ') || (c== '-')) {
      filtered += c;
    }
  }
  return filtered;
}

// ---------------------------------------------------
// Обработчик корневого запроса
// ---------------------------------------------------
void handleRoot() {
  String response = "Команда не распознана";

  // MOVE
  if (server.hasArg("move")) {
    currentAction = "MOVE";
    currentValue  = server.arg("move");
    response      = "Команда MOVE получена";
  }
  // TURN
  else if (server.hasArg("turn")) {
    currentAction = "TURN";
    currentValue  = server.arg("turn");
    response      = "Команда TURN получена";
  }
  // SETSPEED
  else if (server.hasArg("speed")) {
    currentAction = "SETSPEED";
    currentValue  = server.arg("speed");
    response      = "Команда SETSPEED получена";
  }
  // STOP
  else if (server.hasArg("stop")) {
    currentAction = "STOP";
    currentValue  = "";
    response      = "Команда STOP получена";
  }

  server.send(200, "text/plain", response);
}

// ---------------------------------------------------
// I2C: Когда мастер запрашивает данные
// ---------------------------------------------------
void onI2CRequest() {
  // Локальный буфер на 32 байта (максимум для Wire)
  const int I2C_BUFFER_SIZE = 32;
  static char buffer[I2C_BUFFER_SIZE]; 
  memset(buffer, 0, I2C_BUFFER_SIZE); // заполнить нулями

  // Если пришла команда - формируем строку
  if (currentAction != "") {
    // Например: MOVE 100
    String outStr = currentAction;
    if (currentAction != "STOP") {
      outStr += " ";
      outStr += currentValue;
    }

    // Фильтруем, чтобы убрать все лишние символы
    outStr = filterMessage(outStr);

    // Кладем результат в buffer (превращаем в с-строку)
    // toCharArray автоматически добавляет '\0' в конце
    // но обрежет, если длиннее 31 символа (т.к. 1 байт нужен под '\0')
    outStr.toCharArray(buffer, I2C_BUFFER_SIZE);

    // Отправляем ровно 32 байта
    Wire.write((uint8_t*)buffer, I2C_BUFFER_SIZE);

    Serial.print("I2C: отправлена команда: ");
    Serial.println(buffer);

    // Сбросим команду
    currentAction = "";
    currentValue  = "";
  }
  else {
    // Если команды нет - шлём 32 байта нулей
    Wire.write((uint8_t*)buffer, I2C_BUFFER_SIZE);
    Serial.println("I2C: пустая команда (в буфере нули)");
  }
}

// ---------------------------------------------------
// Настройка
// ---------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Подключение к Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Подключение к Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi подключено, IP: " + WiFi.localIP().toString());

  // Запуск веб-сервера
  server.on("/", handleRoot);
  server.begin();
  Serial.println("HTTP-сервер запущен");

  // Настройка I2C в режиме slave на адресе 0x55
  Wire.begin(0x55);
  Wire.onRequest(onI2CRequest);
}

// ---------------------------------------------------
// Основной цикл
// ---------------------------------------------------
void loop() {
  // Обработка входящих HTTP-запросов
  server.handleClient();
}
