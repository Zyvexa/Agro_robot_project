import threading
import time
import cv2
import requests

from flask import Flask, render_template_string, request, Response, jsonify
from pyzbar.pyzbar import decode

# --- I2C и датчики ---
import smbus2
import bme280

import board
import busio
from adafruit_apds9960.apds9960 import APDS9960

app = Flask(__name__)

########################################
# Глобальные переменные
########################################

# Глобальные переменные датчиков
temperature_c = 0.0
temperature_f = 32.0
pressure = 0.0
humidity = 0.0
light_level = 0  # c (clear) с датчика APDS9960

# Глобальная переменная для распознанного QR-кода
recognized_qr = ""

# Параметры камеры (USB-камера или CSI)
camera = cv2.VideoCapture(0)
camera.set(cv2.CAP_PROP_FRAME_WIDTH, 320)
camera.set(cv2.CAP_PROP_FRAME_HEIGHT, 240)

# IP адрес ESP (замените на свой)
esp_ip = '192.168.50.47'

########################################
# Функции для датчиков
########################################

def celsius_to_fahrenheit(c):
    return (c * 9.0 / 5.0) + 32.0

def sensor_thread():
    """
    Поток для постоянного чтения данных с BME280 и APDS9960,
    чтобы не блокировать основной поток Flask.
    """
    global temperature_c, temperature_f, pressure, humidity, light_level

    # Инициализация датчика BME280
    address_bme = 0x76  # Проверьте адрес в вашей конфигурации
    bus = smbus2.SMBus(1)
    calibration_params = bme280.load_calibration_params(bus, address_bme)

    # Инициализация датчика APDS9960
    i2c = board.I2C()
    apds = APDS9960(i2c)
    apds.enable_color = True  # Включаем режим считывания цвета (для освещённости)

    while True:
        try:
            # --- Считывание BME280 ---
            data = bme280.sample(bus, address_bme, calibration_params)
            temperature_c = data.temperature
            temperature_f = celsius_to_fahrenheit(temperature_c)
            pressure = data.pressure
            humidity = data.humidity

            # --- Считывание APDS9960 ---
            r, g, b, c = apds.color_data
            light_level = c  # Обычно для "уровня освещённости" берут канал c (clear)

        except Exception as e:
            # Здесь можно вывести в лог или отладить ошибку
            pass

        # Делаем паузу, чтобы не перегружать шину I2C
        time.sleep(2)

########################################
# Генерация кадров видеопотока
########################################

def gen_frames():
    """
    Генератор кадров для видеопотока (MJPEG). 
    Распознаёт QR-коды и при обнаружении обновляет recognized_qr.
    Если кода нет - оставляем последнее распознанное значение.
    """
    global recognized_qr

    while True:
        success, frame = camera.read()
        if not success:
            break

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        barcodes = decode(gray)

        # Если нашли новый код – обновим recognized_qr
        for barcode in barcodes:
            x, y, w, h = barcode.rect
            cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 255, 0), 2)
            recognized_qr = barcode.data.decode('utf-8')

        # Кодируем кадр в JPEG
        ret, buffer = cv2.imencode('.jpg', frame)
        if not ret:
            continue

        frame = buffer.tobytes()
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')

########################################
# HTML-шаблон
########################################

page_template = """
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Управление роботом и модулем</title>
    <style>
      body {
          margin: 0; 
          padding: 0;
          font-family: Arial, sans-serif;
          background-color: #0B0B3B; /* Тёмно-синий фон */
          color: #FFFFFF; /* Белый текст */
      }
      .container {
          display: flex;
          flex-direction: column;
          width: 90%;
          margin: 0 auto;
          padding: 20px;
      }
      .control-section, .module-section {
          background-color: #1C1C5A; /* Темнее, но близко к тёмно-синему */
          margin: 10px 0;
          padding: 20px;
          border-radius: 12px; /* Скруглённые углы */
      }
      h2 {
          margin-top: 0;
          text-align: center;
      }
      .form-group {
          margin: 10px 0;
          display: flex;
          align-items: center;
      }
      .form-group label {
          margin-right: 10px;
      }
      .form-group input[type="number"],
      .form-group input[type="text"] {
          flex: 1;
          padding: 8px;
          border-radius: 8px;
          border: 1px solid #ccc;
      }
      .button {
          background-color: #2E2EFE;
          color: #fff;
          padding: 10px 20px;
          border: none;
          border-radius: 10px;
          cursor: pointer;
          font-size: 14px;
      }
      .button:hover {
          background-color: #5858FA;
      }
      .video-container {
          text-align: center;
      }
      .sensor-data {
          margin-top: 10px;
          text-align: center;
      }
      .sensor-item {
          margin: 5px 0;
      }
      #qr_text {
          font-weight: bold;
      }
      /* Скрываем элементы по умолчанию при необходимости */
    </style>
</head>
<body>
  <div class="container">

    <!-- ВЕРХНЯЯ ЧАСТЬ: Управление роботом -->
    <div class="control-section">
      <h2>Управление роботом</h2>

      <div class="form-group">
        <button class="button" onclick="sendStop()">STOP</button>
      </div>

      <div class="form-group">
        <label for="speedInput">Скорость (0-100):</label>
        <input type="number" id="speedInput" value="50" min="0" max="100" oninput="changeSpeed()">
      </div>

      <!-- Радио-кнопки для выбора режима действия -->
      <div class="form-group">
        <label style="margin-right:10px;">Режим:</label>
        <input type="radio" name="movementMode" value="move" id="moveRadio" checked onclick="toggleMode('move')"> Движение
        <input type="radio" name="movementMode" value="turn" id="turnRadio" style="margin-left:20px;" onclick="toggleMode('turn')"> Поворот
      </div>

      <!-- Поле ввода для движения -->
      <div class="form-group" id="distanceGroup">
        <label for="distanceInput">Дистанция (см):</label>
        <input type="number" id="distanceInput" placeholder="0" min="0">
      </div>

      <!-- Поле ввода для поворота (изначально скрыто) -->
      <div class="form-group" id="angleGroup" style="display:none;">
        <label for="angleInput">Угол поворота (°):</label>
        <input type="number" id="angleInput" placeholder="0" min="0" max="360">
      </div>

      <div class="form-group">
        <button class="button" onclick="sendMove()">Отправить команду</button>
      </div>
    </div>

    <!-- НИЖНЯЯ ЧАСТЬ: Видео, распознание QR, датчики -->
    <div class="module-section">
      <h2>Модуль (камера и датчики)</h2>

      <div class="video-container">
        <img id="videoStream" src="{{ url_for('video_feed') }}" alt="Видеотрансляция" />
      </div>
      <p style="text-align:center;">
        Распознанный QR-код: <span id="qr_text">нет</span>
      </p>

      <div class="sensor-data">
        <div class="sensor-item" id="tempData">Температура: --</div>
        <div class="sensor-item" id="pressureData">Давление: --</div>
        <div class="sensor-item" id="humidityData">Влажность: --</div>
        <div class="sensor-item" id="lightData">Освещённость: --</div>
      </div>
    </div>

  </div>

  <script>
    // Функция переключения режима (движение или поворот)
    function toggleMode(mode) {
      if (mode === 'move') {
        document.getElementById('distanceGroup').style.display = 'flex';
        document.getElementById('angleGroup').style.display = 'none';
      } else {
        document.getElementById('distanceGroup').style.display = 'none';
        document.getElementById('angleGroup').style.display = 'flex';
      }
    }

    // Функция отправки запроса "STOP"
    function sendStop() {
      fetch("/robot_control?stop=1")
        .then(response => response.json())
        .then(data => {
          console.log("Stop response:", data);
        })
        .catch(err => console.error(err));
    }

    // Функция отправки запроса изменения скорости
    let lastSentSpeed = 50;  // начальное значение
    function changeSpeed() {
      const speedVal = document.getElementById("speedInput").value;
      // Отправляем только если скорость реально поменялась:
      if (speedVal !== lastSentSpeed.toString()) {
        lastSentSpeed = speedVal;
        fetch("/robot_control?speed=" + speedVal)
          .then(response => response.json())
          .then(data => {
            console.log("Speed response:", data);
          })
          .catch(err => console.error(err));
      }
    }

    // Функция отправки команды движения или поворота
    // - Если выбран режим "move" => отправляем ?move=XX
    // - Если выбран режим "turn" => отправляем ?turn=YY
    function sendMove() {
      const moveRadio = document.getElementById("moveRadio");
      const turnRadio = document.getElementById("turnRadio");

      if (moveRadio.checked) {
        const distanceVal = document.getElementById("distanceInput").value;
        const params = new URLSearchParams({ move: distanceVal });
        fetch("/move?" + params.toString())
          .then(response => response.json())
          .then(data => {
            console.log("Move response:", data);
          })
          .catch(err => console.error(err));
      } 
      else if (turnRadio.checked) {
        const angleVal = document.getElementById("angleInput").value;
        const params = new URLSearchParams({ turn: angleVal });
        fetch("/move?" + params.toString())
          .then(response => response.json())
          .then(data => {
            console.log("Turn response:", data);
          })
          .catch(err => console.error(err));
      }
    }

    // Периодический опрос сервера на предмет последнего QR-кода
    function updateQR() {
      fetch("/get_qr")
        .then(response => response.json())
        .then(data => {
          const qrElem = document.getElementById("qr_text");
          if (data.qr && data.qr !== "") {
            qrElem.textContent = data.qr;
          }
        })
        .catch(err => console.error("Ошибка при получении QR:", err));
    }

    // Периодический опрос сервера для показаний датчиков
    function updateSensors() {
      fetch("/get_sensors")
        .then(response => response.json())
        .then(data => {
          document.getElementById("tempData").innerText = "Температура: " + data.temp_c.toFixed(1) + " °C / " + data.temp_f.toFixed(1) + " °F";
          document.getElementById("pressureData").innerText = "Давление: " + data.pressure.toFixed(1) + " hPa";
          document.getElementById("humidityData").innerText = "Влажность: " + data.humidity.toFixed(1) + " %";
          document.getElementById("lightData").innerText = "Освещённость: " + data.light;
        })
        .catch(err => console.error("Ошибка при получении данных датчиков:", err));
    }

    // Запуск интервалов
    setInterval(updateQR, 1000);       // QR – раз в секунду
    setInterval(updateSensors, 2000);  // Датчики – раз в 2 секунды
  </script>
</body>
</html>
"""

########################################
# Flask-маршруты
########################################

@app.route("/")
def index():
    """
    Главная страница с управлением роботом и видеотрансляцией + датчики.
    """
    return render_template_string(page_template)

@app.route("/video_feed")
def video_feed():
    """
    Видеопоток MJPEG.
    """
    return Response(gen_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route("/get_qr")
def get_qr():
    """
    Возвращает последний распознанный QR-код (или пустую строку).
    """
    return jsonify({"qr": recognized_qr})

@app.route("/get_sensors")
def get_sensors():
    """
    Возвращает текущие показания датчиков в JSON.
    """
    return jsonify({
        "temp_c": temperature_c,
        "temp_f": temperature_f,
        "pressure": pressure,
        "humidity": humidity,
        "light": light_level
    })

@app.route("/robot_control")
def robot_control():
    """
    Принимает GET-параметры:
      - stop=1  => отправить /?stop=1 на ESP
      - speed=XX => отправить /?speed=XX на ESP
    Возвращает JSON-результат.
    """
    stop = request.args.get("stop")
    speed = request.args.get("speed")

    try:
        if stop == "1":
            url = f"http://{esp_ip}/?stop=1"
            resp = requests.get(url, timeout=5)
            return jsonify({"status": "ok", "action": "stop", "esp_response": resp.status_code})

        if speed is not None:
            url = f"http://{esp_ip}/?speed={speed}"
            resp = requests.get(url, timeout=5)
            return jsonify({"status": "ok", "action": "speed", "value": speed, "esp_response": resp.status_code})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)})

    return jsonify({"status": "no_action"})

@app.route("/move")
def move():
    """
    Принимает GET-параметры:
      move=XX (дистанция)
      turn=YY (угол)
    Отправляет их на ESP в зависимости от того, что задано:
      - Если пришёл move => http://<esp_ip>/?move=XX
      - Если пришёл turn => http://<esp_ip>/?turn=YY
    """
    distance = request.args.get("move")
    angle = request.args.get("turn")

    try:
        if distance is not None:
            url = f"http://{esp_ip}/?move={distance}"
            resp = requests.get(url, timeout=5)
            return jsonify({"status": "ok", "move": distance, "esp_response": resp.status_code})

        if angle is not None:
            url = f"http://{esp_ip}/?turn={angle}"
            resp = requests.get(url, timeout=5)
            return jsonify({"status": "ok", "turn": angle, "esp_response": resp.status_code})

        return jsonify({"status": "no_action"})

    except Exception as e:
        return jsonify({"status": "error", "message": str(e)})

########################################
# Точка входа
########################################

if __name__ == "__main__":
    # Запускаем поток для чтения датчиков
    t = threading.Thread(target=sensor_thread, daemon=True)
    t.start()

    # Запускаем Flask-сервер
    app.run(host='0.0.0.0', port=5000, debug=False)
