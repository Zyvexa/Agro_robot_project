#include <Arduino.h>
#include <Wire.h>

// =================== ПАРАМЕТРЫ РОБОТА ===================

// Диаметр колеса (см) и число импульсов энкодера за оборот:
const float WHEEL_DIAMETER_CM = 11.5;
const int   ENCODER_CPR       = 500;
const float WHEEL_CIRCUMFERENCE = PI * WHEEL_DIAMETER_CM;

// Расстояние между левыми и правыми колёсами (см):
const float TRACK_WIDTH_CM = 21.5;
const float ROBOT_LENGTH_CM = 13.5;
const float TURN_RADIUS = sqrt( pow(TRACK_WIDTH_CM/2.0, 2) + pow(ROBOT_LENGTH_CM/2.0, 2) );

// Пины управления:
const int PWM_PIN  = 11;  // общий ШИМ для всех моторов
const int STBY_PIN = 4;   // STBY (включение драйвера)

// Адрес I2C (slave):
const int SLAVE_ADDRESS = 0x08;

// =================== КОНЕЧНЫЙ АВТОМАТ ===================
enum RobotState {
  STATE_IDLE,    // ожидание команды
  STATE_MOVING,  // выполняется команда MOVE
  STATE_TURNING  // выполняется команда TURN
};

volatile RobotState robotState = STATE_IDLE;

// Текущая целевая «длина» в импульсах (для MOVE/TURN)
long targetCounts = 0;

// Текущая «базовая» скорость [0..100], задаётся командой SETSPEED
int currentSpeed = 50;

// Буфер для приёма I2C-команд
String commandBuffer = "";
volatile bool newCommandReceived = false; // флаг, что что-то пришло по I2C

// =================== КЛАСС Motor ===================
//
// В «новой» схеме у каждого мотора есть только 2 уникальных пина:
//   - dirPin  (задаём направление)
//   - encPin  (считываем энкодер)
//
// Скорость выставляем единую для всех через setGlobalSpeed(...).
//
class Motor {
public:
  Motor(uint8_t dirPin, uint8_t encPin, bool reversed = false)
    : _dirPin(dirPin), _encPin(encPin), _reversed(reversed), _encoderCount(0)
  {
    pinMode(_dirPin, OUTPUT);
    pinMode(_encPin, INPUT_PULLUP);
  }

  // Установить направление вращения в зависимости от знака speed
  // (speed > 0 – вперёд, < 0 – назад, 0 – не изменять направление)
  void setDirection(int speed) {
    if (speed > 0) {
      digitalWrite(_dirPin, _reversed ? LOW : HIGH);
    } else if (speed < 0) {
      digitalWrite(_dirPin, _reversed ? HIGH : LOW);
    }
    // Если speed == 0, направление менять не нужно (робот стопорится общим PWM=0).
  }

  // Получить текущее значение энкодера
  long getEncoder() {
    noInterrupts();
    long val = _encoderCount;
    interrupts();
    return val;
  }

  // Сбросить счётчик энкодера
  void resetEncoder() {
    noInterrupts();
    _encoderCount = 0;
    interrupts();
  }

  // Увеличить счётчик энкодера (прерывание по фронту)
  void updateEncoder() {
    _encoderCount++;
  }

private:
  uint8_t _dirPin;
  uint8_t _encPin;
  bool _reversed;
  volatile long _encoderCount;
};

// =================== СОЗДАНИЕ ЧЕТЫРЁХ МОТОРОВ ===================
//
// Из «нового» кода:
//   1) Motor1: dir=2,  enc=5,  reversed=true
//   2) Motor2: dir=3,  enc=7,  reversed=false
//   3) Motor3: dir=9,  enc=10, reversed=false
//   4) Motor4: dir=13, enc=12, reversed=true
//
Motor* motor1;
Motor* motor2;
Motor* motor3;
Motor* motor4;

// =================== ХЕЛПЕРЫ ДЛЯ МОТОРОВ ===================

// Установка одного общего PWM для всех моторов:
void setGlobalSpeed(int speed) {
  // Приводим speed (0..100) к 0..1023 для analogWrite
  int pwmVal = map(abs(speed), 0, 100, 0, 1023);
  analogWrite(PWM_PIN, pwmVal);
}

// Остановка моторов (принудительная)
void stopMotorsImmediately() {
  // PWM = 0
  setGlobalSpeed(0);
  // Отрубить драйвер (для TB6612FNG иногда достаточно держать STBY в HIGH с PWM=0,
  // но здесь по новой схеме отключим, чтобы точно «глушить»):
  digitalWrite(STBY_PIN, LOW);
  robotState = STATE_IDLE;
  Serial.println("Motors STOPPED.");
}

// Запуск движения «прямо» на distanceCm
void startMoveDistance(float distanceCm, int speed) {
  // Сброс энкодеров
  motor1->resetEncoder();
  motor2->resetEncoder();
  motor3->resetEncoder();
  motor4->resetEncoder();

  // Считаем, сколько импульсов надо
  targetCounts = (long)((abs(distanceCm) / WHEEL_CIRCUMFERENCE) * ENCODER_CPR);

  // Все моторы – вперёд/назад в зависимости от знака speed.
  if(distanceCm < 0){
    speed *= -1;
  }
  motor1->setDirection(speed);
  motor2->setDirection(speed);
  motor3->setDirection(speed);
  motor4->setDirection(speed);

  // Включаем драйвер, ставим общий PWM
  digitalWrite(STBY_PIN, HIGH);
  setGlobalSpeed(speed);

  robotState = STATE_MOVING;
  Serial.print("MOVE: distance=");
  Serial.print(distanceCm);
  Serial.print(" cm => targetCounts=");
  Serial.println(targetCounts);
}

// Запуск поворота на угол angleDegrees
// Положительный угол => поворот вправо, отрицательный => влево
void startTurnDegrees(float angleDegrees, int speed) {
  // Сброс энкодеров
  motor1->resetEncoder();
  motor2->resetEncoder();
  motor3->resetEncoder();
  motor4->resetEncoder();

  // Старый (более простой и надёжный) расчёт радиуса:
  float angleRad = fabs(angleDegrees) * PI / 180.0;
  float arcLength = TURN_RADIUS * angleRad;
  targetCounts = (long)((arcLength / WHEEL_CIRCUMFERENCE) * ENCODER_CPR);

  if (angleDegrees > 0) {
    // Поворот вправо: левые моторы вперёд, правые – назад
    motor1->setDirection(-speed); // правый
    motor2->setDirection(-speed); // правый
    motor3->setDirection(speed);  // левый
    motor4->setDirection(speed);  // левый
  } else {
    // Поворот влево: левые моторы назад, правые – вперёд
    motor1->setDirection(speed);   // правый
    motor2->setDirection(speed);   // правый
    motor3->setDirection(-speed);  // левый
    motor4->setDirection(-speed);  // левый
  }

  digitalWrite(STBY_PIN, HIGH);
  setGlobalSpeed(speed);

  robotState = STATE_TURNING;
  Serial.print("TURN: angle=");
  Serial.print(angleDegrees);
  Serial.print(" => targetCounts=");
  Serial.println(targetCounts);
}

// =================== ОБРАБОТКА ВХОДЯЩИХ КОМАНД ===================
void processCommand(const String& cmd) {
  String command = cmd;
  command.trim();        // убрать пробелы по краям
  command.toUpperCase(); // переводим к верхнему регистру для удобства

  Serial.print("I2C command: ");
  Serial.println(command);

  // Сначала проверяем STOP (неважно, есть ли аргументы)
  if (command.startsWith("STOP")) {
    stopMotorsImmediately();
    return; // всё, сразу останавливаем
  }

  // MOVE <число>
  if (command.startsWith("MOVE ")) {
    float distance = command.substring(5).toFloat();
    startMoveDistance(distance, currentSpeed);
  }
  // TURN <число>
  else if (command.startsWith("TURN ")) {
    float angle = command.substring(5).toFloat();
    startTurnDegrees(angle, currentSpeed);
  }
  // SETSPEED <число>
  else if (command.startsWith("SETSPEED ")) {
    int newSpeed = command.substring(9).toInt();
    newSpeed     = constrain(newSpeed, 0, 100);
    currentSpeed = newSpeed;
    Serial.print("Set speed to ");
    Serial.println(currentSpeed);
  }
  else {
    Serial.println("Unknown command (ignored).");
  }
}

// =================== I2C: ПРИЕМ/ОТВЕТ ===================

// Вызывается, когда мастер отправляет данные
void receiveEvent(int numBytes) {
  while (Wire.available()) {
    char c = Wire.read();
    commandBuffer += c;
  }
  newCommandReceived = true;
}

// Вызывается, когда мастер запрашивает ответ
// Отправляем "TRUE", если робот свободен (STATE_IDLE), иначе "FALSE".
void requestEvent() {
  String response = (robotState == STATE_IDLE) ? "TRUE" : "FALSE";
  Wire.write(response.c_str());
}

// =================== ПРЕРЫВАНИЯ PIN CHANGE ДЛЯ ЭНКОДЕРОВ ===================
// Чтобы считать энкодеры без задержек, используем PCINT на портах D и B.
// Следим за фронтами на соответствующих пинах.

volatile uint8_t lastPIND;
volatile uint8_t lastPINB;

ISR(PCINT2_vect) {
  uint8_t newPIND = PIND;
  uint8_t changed = newPIND ^ lastPIND;
  lastPIND = newPIND;

  // Мотор 1: энкодер на PD5 (Arduino pin 5)
  if (changed & (1 << PD5)) {
    motor1->updateEncoder();
  }
  // Мотор 2: энкодер на PD7 (Arduino pin 7)
  if (changed & (1 << PD7)) {
    motor2->updateEncoder();
  }
}

ISR(PCINT0_vect) {
  uint8_t newPINB = PINB;
  uint8_t changed = newPINB ^ lastPINB;
  lastPINB = newPINB;

  // Мотор 3: энкодер на PB2 (Arduino pin 10)
  if (changed & (1 << PB2)) {
    motor3->updateEncoder();
  }
  // Мотор 4: энкодер на PB4 (Arduino pin 12)
  if (changed & (1 << PB4)) {
    motor4->updateEncoder();
  }
}

// =================== SETUP ===================
void setup() {
  Serial.begin(9600);
  Serial.println("Robot @0x08 start...");

  // Настройка пинов STBY и PWM
  pinMode(STBY_PIN, OUTPUT);
  digitalWrite(STBY_PIN, LOW); // моторы изначально отключены
  
  pinMode(PWM_PIN, OUTPUT);
  analogWrite(PWM_PIN, 0);

  // Создаём объекты моторов (по новой схеме пинов)
  motor1 = new Motor(2, 5, true);
  motor2 = new Motor(3, 7, false);
  motor3 = new Motor(9, 10, false);
  motor4 = new Motor(13, 12, true);

  // Фиксируем начальное состояние портов D/B для PCINT
  lastPIND = PIND;
  lastPINB = PINB;

  // Разрешаем Pin Change Interrupt на портах D (PCIE2) и B (PCIE0)
  PCICR  |=  (1 << PCIE2) | (1 << PCIE0);
  // Включаем прерывания для нужных пинов:
  //   PD5 => bit PD5 (5), PD7 => bit PD7 (7)
  PCMSK2 |=  (1 << PD5) | (1 << PD7);
  //   PB2 => bit PB2 (2), PB4 => bit PB4 (4)
  PCMSK0 |=  (1 << PB2) | (1 << PB4);

  // Запуск I2C (slave)
  Wire.begin(SLAVE_ADDRESS);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);

  Serial.println("I2C Slave ready. Waiting for commands...");


//  startMoveDistance(distance, currentSpeed);
}

// =================== LOOP ===================
void loop() {
  // 1) Если пришла новая команда — разбираем её
  if (newCommandReceived) {
    newCommandReceived = false;
    processCommand(commandBuffer);
    commandBuffer = ""; // очистим после обработки
  }

  // 2) Если робот сейчас двигается (MOVE или TURN), проверяем энкодеры:
  if (robotState == STATE_MOVING || robotState == STATE_TURNING) {
    long c1 = labs(motor1->getEncoder());
    long c2 = labs(motor2->getEncoder());
    long c3 = labs(motor3->getEncoder());
    long c4 = labs(motor4->getEncoder());
    long averageCount = (c1 + c2 + c3 + c4) / 4;

    if (averageCount >= targetCounts) {
      stopMotorsImmediately(); // достигли цели — стоп
    }
  }

  // Остальные фоновые задачи, если нужны, здесь.
}
