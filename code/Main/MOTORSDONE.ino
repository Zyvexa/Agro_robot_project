#include <Wire.h>

// =================== КОНЕЧНЫЙ АВТОМАТ ====================
enum RobotState {
  STATE_IDLE,    // ожидание команды
  STATE_MOVING,  // выполняется команда MOVE
  STATE_TURNING  // выполняется команда TURN
};

volatile RobotState robotState = STATE_IDLE;  // начальное состояние – свободен
int currentSpeed = 15;      // текущая скорость (по умолчанию)
long targetCounts = 0;      // требуемое число импульсов (для MOVE или TURN)

// Буфер для получения I2C-команд
String commandBuffer = "";
volatile bool newCommandReceived = false; // флаг, что получена новая команда

// =================== КЛАСС MOTOR ====================
class Motor {
public:
  // Конструктор принимает:
  //  - dirPin: пин для задания направления (_IN1 TB6612FNG)
  //  - pwmPin: пин ШИМ (PWMA или PWAB)
  //  - encPin: пин энкодера
  //  - reversed: если true, то направление задано наоборот
  Motor(uint8_t dirPin, uint8_t pwmPin, uint8_t encPin, bool reversed = false)
    : _dirPin(dirPin), _pwmPin(pwmPin), _encPin(encPin), _encoderCount(0), _reversed(reversed)
  {
    pinMode(_dirPin, OUTPUT);
    pinMode(_pwmPin, OUTPUT);
    pinMode(_encPin, INPUT_PULLUP);
  }
  
  // Устанавливает скорость от -100 до 100 
  void setSpeed(int speed) {
    speed = constrain(speed, -100, 100);
    int pwmVal = map(abs(speed), 0, 100, 0, 1023);
    
    if (!_reversed) {
      digitalWrite(_dirPin, speed >= 0 ? HIGH : LOW);
    } else {
      digitalWrite(_dirPin, speed >= 0 ? LOW : HIGH);
    }
    
    analogWrite(_pwmPin, pwmVal);
  }
  
  // Возвращает значение энкодера
  long getEncoder() {
    noInterrupts();
    long count = _encoderCount;
    interrupts();
    return count;
  }
  
  // Сброс энкодера
  void resetEncoder() {
    noInterrupts();
    _encoderCount = 0;
    interrupts();
  }
  
  // Обновление счётчика энкодера (вызывается в прерывании)
  void updateEncoder() {
    _encoderCount++;
  }
  
private:
  uint8_t _dirPin;
  uint8_t _pwmPin;
  uint8_t _encPin;
  volatile long _encoderCount;
  bool _reversed;
};

// =================== ГЛОБАЛЬНЫЕ ОБЪЕКТЫ МОТОРОВ ====================
// Распиновка согласно вашему описанию:
// Мотор 1: dir=2, PWM=A0, энкодер: пин=5  
// Мотор 2: dir=3, PWM=A1, энкодер: пин=7  
// Мотор 3: dir=9, PWM=A2, энкодер: пин=10  
// Мотор 4: dir=A6, PWM=A3, энкодер: пин=12
Motor* motor1;
Motor* motor2;
Motor* motor3;
Motor* motor4;

// =================== ГЛОБАЛЫЕ КОНСТАНТЫ ====================
const float WHEEL_DIAMETER_CM = 11.5;  // диаметр колеса (см)
const int ENCODER_CPR = 500;           // импульсов за оборот
const float TRACK_WIDTH_CM = 21.5;     // расстояние между осями (см)

const int SLAVE_ADDRESS = 0x08;        // I2C адрес

// Для хранения предыдущих состояний портов для PCINT:
volatile uint8_t lastPIND;
volatile uint8_t lastPINB;

// =================== ФУНКЦИИ ЗАПУСКА ДВИЖЕНИЯ ===================

// Инициализация команды MOVE (проезд на distanceCm со скоростью speed)
void startMoveDistance(float distanceCm, int speed) {
  // Сброс энкодеров
  motor1->resetEncoder();
  motor2->resetEncoder();
  motor3->resetEncoder();
  motor4->resetEncoder();
  
  // Длина окружности колеса
  float wheelCircumference = PI * WHEEL_DIAMETER_CM;
  // Определяем требуемое число импульсов
  targetCounts = (long)((distanceCm / wheelCircumference) * ENCODER_CPR);
  
  // Запускаем все моторы вперёд
  motor1->setSpeed(speed);
  motor2->setSpeed(speed);
  motor3->setSpeed(speed);
  motor4->setSpeed(speed);
  
  robotState = STATE_MOVING;
  Serial.print("MOVE start: targetCounts=");
  Serial.println(targetCounts);
}

// Инициализация команды TURN (поворот на angle градусов со скоростью speed)
// По условию: моторы 1 и 2 (справа) и 3 и 4 (слева)
// Положительный угол – поворот вправо: левые моторы вперёд, правые назад.
// Отрицательный угол – поворот влево: левые моторы назад, правые вперёд.
void startTurnDegrees(float angle, int speed) {
  // Сброс энкодеров
  motor1->resetEncoder();
  motor2->resetEncoder();
  motor3->resetEncoder();
  motor4->resetEncoder();
  
  float turnRadius = TRACK_WIDTH_CM / 2.0;  
  float angleRadians = abs(angle) * PI / 180.0;
  float arcLength = turnRadius * angleRadians;
  float wheelCircumference = PI * WHEEL_DIAMETER_CM;
  targetCounts = (long)((arcLength / wheelCircumference) * ENCODER_CPR);
  
  // Устанавливаем скорости по конвенции
  if (angle > 0) {
    // Поворот вправо: левые моторы вперёд, правые назад
    motor1->setSpeed(-speed);
    motor2->setSpeed(-speed);
    motor3->setSpeed(speed);
    motor4->setSpeed(speed);
  } else {
    // Поворот влево: левые моторы назад, правые вперёд
    motor1->setSpeed(speed);
    motor2->setSpeed(speed);
    motor3->setSpeed(-speed);
    motor4->setSpeed(-speed);
  }
  
  robotState = STATE_TURNING;
  Serial.print("TURN start: targetCounts=");
  Serial.println(targetCounts);
}

// Остановка всех моторов и переход в состояние IDLE
void stopMotors() {
  motor1->setSpeed(0);
  motor2->setSpeed(0);
  motor3->setSpeed(0);
  motor4->setSpeed(0);
  robotState = STATE_IDLE;
  Serial.println("Command complete, motors stopped.");
}

// =================== I2C ОБРАБОТЧИКИ ===================

// Прием данных от мастера
void receiveEvent(int numBytes) {
  while (Wire.available()) {
    char c = Wire.read();
    commandBuffer += c;
  }
  newCommandReceived = true;
}

// Когда мастер запрашивает данные
// На команду DONE возвращаем "TRUE", если FSM в состоянии IDLE, иначе "FALSE"
void requestEvent() {
  String response = (robotState == STATE_IDLE) ? "TRUE" : "FALSE";
  Wire.write(response.c_str());
}

// =================== SETUP ===================
void setup() {
  Serial.begin(9600);
  
  // Пин STBY для драйверов моторов
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);
  
  // Создаем объекты моторов
  motor1 = new Motor(2, A0, 5, true);
  motor2 = new Motor(3, A1, 7, false);
  motor3 = new Motor(9, A2, 10, false);
  motor4 = new Motor(A6, A3, 12, false);
  
  // Сохраняем начальное состояние портов для PCINT
  lastPIND = PIND;
  lastPINB = PINB;
  
  // Настройка прерываний по изменению состояния (PCINT)
  PCICR |= (1 << PCIE2);    // Порт D (пины PD5 и PD7)
  PCMSK2 |= (1 << PD5) | (1 << PD7);
  
  PCICR |= (1 << PCIE0);    // Порт B (пины PB2 и PB4)
  PCMSK0 |= (1 << PB2) | (1 << PB4);
  
  // Инициализация I2C в режиме slave
  Wire.begin(SLAVE_ADDRESS);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
  
  Serial.println("I2C Slave ready.");
}

// =================== LOOP ===================
void loop() {
  // Если пришла новая I2C-команда, обрабатываем её (без блокирующих циклов)
  if (newCommandReceived) {
    newCommandReceived = false;
    commandBuffer.trim();
    commandBuffer.toUpperCase();
    Serial.print("Received command: ");
    Serial.println(commandBuffer);
    
    if (commandBuffer.startsWith("MOVE ")) {
      float distance = commandBuffer.substring(5).toFloat();
      startMoveDistance(distance, currentSpeed);
    }
    else if (commandBuffer.startsWith("TURN ")) {
      float angle = commandBuffer.substring(5).toFloat();
      startTurnDegrees(angle, currentSpeed);
    }
    else if (commandBuffer.startsWith("SETSPEED ")) {
      int newSpeed = commandBuffer.substring(9).toInt();
      currentSpeed = newSpeed;
      Serial.print("Speed set to ");
      Serial.println(currentSpeed);
    }
    // Команда DONE не требует обработки здесь – ответ отдается в requestEvent()
    
    commandBuffer = ""; // очистка буфера
  }
  
  // Обновление конечного автомата: если выполняется движение, проверяем, достигнута ли цель.
  if (robotState == STATE_MOVING || robotState == STATE_TURNING) {
    // Используем среднее значение всех 4 энкодеров
    long count1 = abs(motor1->getEncoder());
    long count2 = abs(motor2->getEncoder());
    long count3 = abs(motor3->getEncoder());
    long count4 = abs(motor4->getEncoder());
    long averageCount = (count1 + count2 + count3 + count4) / 4;
    
    // Если достигнуто требуемое число импульсов, остановить моторы и перейти в IDLE
    if (averageCount >= targetCounts) {
      stopMotors();
    }
  }
  
  // Здесь можно выполнять и другие периодические задачи, не блокируя I2C.
}

// =================== ОБРАБОТЧИКИ ПРЕРЫВАНИЙ PCINT ===================

ISR(PCINT2_vect) {
  uint8_t newPIND = PIND;
  uint8_t changed = newPIND ^ lastPIND;
  lastPIND = newPIND;
  
  // Motor 1: энкодер на PD5
  if (changed & (1 << PD5)) {
    motor1->updateEncoder();
  }
  // Motor 2: энкодер на PD7
  if (changed & (1 << PD7)) {
    motor2->updateEncoder();
  }
}

ISR(PCINT0_vect) {
  uint8_t newPINB = PINB;
  uint8_t changed = newPINB ^ lastPINB;
  lastPINB = newPINB;
  
  // Motor 3: энкодер на PB2
  if (changed & (1 << PB2)) {
    motor3->updateEncoder();
  }
  // Motor 4: энкодер на PB4
  if (changed & (1 << PB4)) {
    motor4->updateEncoder();
  }
}
