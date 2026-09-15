/*
    - Работа с новой прошивкой: подтверждаем работоспособность или откатываемся
*/

#include <Arduino.h>
#include <GitHubOTA.h>

#if defined(ESP32)
    #include <WiFi.h>
    #include <WiFiClientSecure.h>
    WiFiClientSecure client;                                 // клиент для защищенного соединения
#elif defined(ESP8266)
    #include <ESP8266WiFi.h>
    #include <WiFiClientSecureBearSSL.h>
    BearSSL::WiFiClientSecure client;                        // клиент для защищенного соединения
#endif

#define WiFi_SSID "ваше имя сети"                            // ssid (имя) WiFI сети
#define WiFi_PASS "ваш пароль от сети"                       // пароль от WiFi сети

GitHubOTA updater;                                           // объект библиотеки

bool selfCheckPassed();

void setup() {
    Serial.begin(9600);

    Serial.printf("Подключение к %s\n", WiFi_SSID);
    WiFi.begin(WiFi_SSID, WiFi_PASS);                        // подключаемся к сети
    while (WiFi.status() != WL_CONNECTED) {                  // с ожиданием
        delay(50);
        Serial.print(".");
    }
    Serial.println("Подключено к сети WiFi!");

    client.setInsecure();                                    // для тестов отключим проверку сертификата

    GitHubOTA::Config cfg;                                   // создаем конфиг - профиль настроек работы
    cfg.repoOwner = "Al-Zhilin";                             // владелец репозитория, в котором ловим обновления
    cfg.repoName = "GitHubOTA-TestBed";                      // имя репозитория, в котором будут мониториться релизы
    cfg.currentVersion = "v0.0.1";                           // намеренно устаревшая версия прошивки, чтобы проверка выдала наличие обновления

    updater.begin(cfg, client);                              // передаем конфиг настроек и клиент защищенного соединения

    if (updater.isPendingValidation()) {                     // если прошивка требует подтверждения работоспособности
        // Испытательный срок
        if (selfCheckPassed())  updater.confirmValid();      // подтверждаем: обновление работоспособно
        else updater.rejectAndRollback();                    // откатываемся к предыдущей версии. На ESP32 это приведет к немедленной перезагрузке, а на ESP8266 - возврат ROLLBACK_UNSUPPORTED без перезагрузки, ..
                                                                // -> т.к. механизма, который бы позволял делать откат к предыдущей версии, на этом контроллере нет
    }

    else {
        Serial.println("Прошивка не требует подтверждения работоспособности, штатно начинаем работу");
    }
}

void loop() {

}

bool selfCheckPassed() {
    // Здесь должна быть проверка, доказывающая, что обновление работает
    // например проверка, что датчики отвечают, сервер доступен, выход в сеть имеется и т.д.
    // в идеальном случае проверка должна однозначно идентифицировать: успешно ли работает устройство или что-то сломалось и требуется откат

    return true;
}