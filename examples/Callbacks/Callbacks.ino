/*
    - Пример использования коллбэков

    onStateChange - при смене состояния
    onProgress - при скачивании прошивки
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

void onStateChanged(GitHubOTA::State state);                 // функция с подходящей сигнатурой
void onDownloading(size_t written, size_t total);            // и для второго коллбэка

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

    updater.onStateChange(onStateChanged);                   // регистрируем коллбэк смены состояния
    updater.onProgress(onDownloading);                       // и коллбэк прогресса скачивания
    // Важно! Регистрацию нужно проводить до begin(), т.к. уже в этой функции могут произойти фундаментальные действия, которые бывает критически важно регистрировать

    updater.begin(cfg, client);                              // передаем конфиг настроек и клиент защищенного соединения

    // Проверка обновлений гарантированно вызовет коллбэк onStateChange, а при наличии обновления - второй коллбэк будет вызываться методом update()
    if (updater.checkUpdates() == GitHubOTA::Status::UPDATE_AVAILABLE) {
        updater.update();
    }

}

void loop() {

}

void onDownloading(size_t written, size_t total) {
    Serial.printf("Скачано %zu из %zu байт (%f%%)\n", written, total, (float)written/total*100.0f);
}

void onStateChanged(GitHubOTA::State state) {
    switch (state) {
        case GitHubOTA::State::CAUGHT_ERROR:
            Serial.println("Произошла какая-то ошибка, см. updater.getLastError()");
            break;

        case GitHubOTA::State::CHECKING:
            Serial.println("Проверка налличия обновлений...");
            break;

        case GitHubOTA::State::DOWNLOADING:
            Serial.println("Скачиваем найденное обновление...");
            break;

        case GitHubOTA::State::IDLE:
            Serial.println("Холостая работа");
            break;

        case GitHubOTA::State::REBOOTING:
            Serial.println("Перезагрузка при обновлении");
            break;

        case GitHubOTA::State::ROLLING_BACK:
            Serial.println("Откат обновления");
            break;

        case GitHubOTA::State::UP_TO_DATE:
            Serial.println("Установлена последняя версия");
            break;

        case GitHubOTA::State::UPDATE_AVAILABLE:
            Serial.println("Доступно обновления");
            break;

        case GitHubOTA::State::VERIFYING:
            Serial.println("Проверка целостности файла обновления перед применением...");
            break;
    }
}