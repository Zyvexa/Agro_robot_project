#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Wire.h>

// ===== BLE настройки =====
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // уведомления (ESP -> клиент)
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // запись (клиент -> ESP)

// ===== I²C настройки =====
#define I2C_DEV_ADDR 0x55  // ESP будет I²C slave с этим адресом

// ===== Глобальные переменные =====
// Для управляющих команд (MOVE/TURN)
String currentAction = "";
int currentValue = 0;

// Флаг обращения по I²C (хоть раз)
bool i2cAccessed = false;

// Для сенсорных данных от мастера (например, "LUX: X Temperature: X Humidity: X")
String sensorData = "";
// Флаг, управляющий отправкой сенсорных данных по BLE
bool sendSensorData = false;

// ===== BLE объекты =====
BLECharacteristic* pTxCharacteristic;
bool deviceConnected = false;

// ===== BLE callbacks =====
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("BLE клиент подключился");
  }
  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    Serial.println("BLE клиент отключился");
  }
};

class MyRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      String commandStr = String(rxValue.c_str());
      commandStr.trim();
      Serial.print("Получено по BLE: ");
      Serial.println(commandStr);
      
      // Обработка команды "status"
      if (commandStr.equalsIgnoreCase("status")) {
        String status = i2cAccessed ? "готов" : "не готов";
        pTxCharacteristic->setValue(status.c_str());
        pTxCharacteristic->notify();
        Serial.print("Отправлен статус: ");
        Serial.println(status);
      }
      // Обработка команды "MOVE X"
      else if (commandStr.startsWith("MOVE")) {
        int spaceIndex = commandStr.indexOf(' ');
        if (spaceIndex > 0) {
          String valueStr = commandStr.substring(spaceIndex + 1);
          currentValue = valueStr.toInt();
          currentAction = "MOVE";
          Serial.print("Команда получена: MOVE ");
          Serial.println(currentValue);
          pTxCharacteristic->setValue("Команда принята: MOVE");
          pTxCharacteristic->notify();
        }
      }
      // Обработка команды "TURN X"
      else if (commandStr.startsWith("TURN")) {
        int spaceIndex = commandStr.indexOf(' ');
        if (spaceIndex > 0) {
          String valueStr = commandStr.substring(spaceIndex + 1);
          currentValue = valueStr.toInt();
          currentAction = "TURN";
          Serial.print("Команда получена: TURN ");
          Serial.println(currentValue);
          pTxCharacteristic->setValue("Команда принята: TURN");
          pTxCharacteristic->notify();
        }
      }
      // Команда GET: включить передачу сенсорных данных
      else if (commandStr.equalsIgnoreCase("GET")) {
        sendSensorData = true;
        pTxCharacteristic->setValue("Передача сенсорных данных включена");
        pTxCharacteristic->notify();
        Serial.println("Получена команда GET: включение передачи сенсорных данных");
      }
      // Команда STOP: выключить передачу сенсорных данных
      else if (commandStr.equalsIgnoreCase("STOP")) {
        sendSensorData = false;
        pTxCharacteristic->setValue("Передача сенсорных данных отключена");
        pTxCharacteristic->notify();
        Serial.println("Получена команда STOP: отключение передачи сенсорных данных");
      }
      else {
        Serial.println("Неизвестная команда");
        pTxCharacteristic->setValue("Ошибка: неизвестная команда");
        pTxCharacteristic->notify();
      }
    }
  }
};

// ===== I²C callbacks =====
// При запросе мастером (для управляющих команд)
void onI2CRequest() {
  i2cAccessed = true;
  if (currentAction != "") {
    String outStr = currentAction + " " + String(currentValue);
    outStr += '\0';
    Wire.write((const uint8_t*)outStr.c_str(), outStr.length());
    Serial.print("I2C запрос: отправлена команда: ");
    Serial.println(outStr);
    // Сбрасываем команду после отправки
    currentAction = "";
    currentValue = 0;
  } else {
    String zeroStr = "0 Packets.";
    zeroStr += '\0';
    Wire.write((const uint8_t*)zeroStr.c_str(), zeroStr.length());
    Serial.println("I2C запрос: отправлено: 0 Packets.");
  }
}

// При записи мастером – получаем сенсорные данные
void onI2CReceive(int len) {
  sensorData = "";
  while (Wire.available()) {
    char c = Wire.read();
    if (c == '\0') break;
    sensorData += c;
  }
  Serial.print("I2C получено: сенсорные данные: ");
  Serial.println(sensorData);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Инициализация ESP (BLE + I2C slave)...");
  
  // Инициализация BLE
  BLEDevice::init("ESP32_BLE_Controller");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_TX,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());
  
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE
  );
  pRxCharacteristic->setCallbacks(new MyRxCallbacks());
  
  pService->start();
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("Ожидание BLE клиента...");
  
  // Инициализация I²C в режиме slave
  Wire.begin(I2C_DEV_ADDR);
  Wire.onReceive(onI2CReceive);
  Wire.onRequest(onI2CRequest);
}

void loop() {
  // Если активирован режим передачи сенсорных данных и данные получены, отправляем их по BLE
  if (sendSensorData && sensorData.length() > 0) {
    pTxCharacteristic->setValue(sensorData.c_str());
    pTxCharacteristic->notify();
    Serial.print("BLE: отправлены сенсорные данные: ");
    Serial.println(sensorData);
    // По желанию можно очистить sensorData после отправки
    sensorData = "";
  }
  delay(1000);
}
