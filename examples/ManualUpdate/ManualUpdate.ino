/*
    - Пример ручного обновления

    Скетч проверяет наличие обновлений "вручную" - вызовом checkUpdates() и, если обновление найдено, запускает
    процесс обновления вызовом update()
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

    GitHubOTA::Status stat = updater.checkUpdates();         // метод проверки наличия обновления

    if (stat == GitHubOTA::Status::UPDATE_AVAILABLE) {
        char buf[16] = {0};
        updater.getAvailableVersion(buf, sizeof(buf));       // получить идентификатор доступной версии в буфер
        Serial.printf("Доступно обновление, версия: %s\nОбновляемся!", buf);

        GitHubOTA::Status update_status = updater.update();  // в нормальной работе этот метод под капотом перезагрузит контроллер, но код возврата ->
                                                                        // -> нужно проверять все равно на предмет возникновения сетевых и иных ошибок

        Serial.printf("Попытка обновления завершилась неудачно, код возврата: %d", update_status);
    }

    else if (stat == GitHubOTA::Status::ALREADY_UP_TO_DATE) {
        Serial.println("Установлена последняя версия");
    }

    else {
        Serial.printf("Выполнена проверка наличия обновления, код возврата: %d", stat);
    }
}

void loop() {

}