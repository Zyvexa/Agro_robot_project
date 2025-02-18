import sensor, image, time, pyb

# Настройка камеры
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=2000)

# Порог для зеленого цвета (параметры могут потребовать корректировки)
green_threshold = (30, 100, -64, -8, -32, 32)

# Инициализация I2C в режиме SLAVE на шине 2 с адресом 0x12
i2c = pyb.I2C(2, pyb.I2C.SLAVE, addr=0x12)
print("Ожидаем данные по I²C...")

while True:
    try:
        # Принимаем 1 байт данных (ожидаем сообщение от Arduino)
        data = i2c.recv(1, timeout=500)
        if data:
            print("Получено сообщение:", data)

            # Делаем снимок
            img = sensor.snapshot()
            plant = ""
            # Ищем зеленые объекты с объединением соседних областей
            blobs = img.find_blobs([green_threshold], merge=True)
            if blobs:
                plant = "plant"
                # Выбираем самый крупный объект
                largest_blob = max(blobs, key=lambda b: b.pixels())
                # Рисуем красный прямоугольник вокруг объекта
                img.draw_rectangle(largest_blob.rect(), color=(255, 0, 0))

            # Формируем уникальное имя файла на основе времени (миллисекунд)
            filename = "/sd/img_%d_" % pyb.millis() + plant + ".jpg"
            # Сохраняем изображение на SD-карту
            img.save(filename)
            print("Изображение сохранено:", filename)

    except Exception as e:
        # Если ошибка связана с отсутствием данных (EIO), просто игнорируем её
        if "EIO" not in str(e):
            print("Ошибка:", e)
    time.sleep(0.1)
