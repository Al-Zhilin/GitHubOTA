// библиотека, реализующая OTA обновление из GitHub releases
#pragma once
#include <Arduino.h>
#include <EEPROM.h>

#if defined(ESP32)
    #include <NetworkClient.h>
    #include <HTTPClient.h>
    #include <Update.h>
    #include <esp_ota_ops.h>
    #include <esp_partition.h>
    using GitHubOTAClient = NetworkClient;

#elif defined(ESP8266)
    #include <WiFiClient.h>
    #include <ESP8266HTTPClient.h>
    #include <Updater.h>
    using GitHubOTAClient = WiFiClient;
    
#else
    #error "GitHubOTA: unsupported platform"

#endif

class GitHubOTA {
    public:

        struct Config {
            const char* repoOwner = "";                             // владелец репозитория
            const char* repoName = "";                              // имя репозитория
            const char* assetName = "firmware.bin";                 // имя файла ассетов
            const char* currentVersion = "";                        // текущая версия
            uint32_t checkIntervalMs = 0;                           // период между проверками наличия обновления (мс). Ноль для отключения
            uint32_t autoConfirmTimeoutMs = 60000;                  // сколько мс новая прошивка должна проработать после перезагрузки, прежде чем handle() САМ вызовет confirmValid(), если хост не сделал этого явно (0 = автоподтверждение выкл, ждать только явного вызова)
            uint32_t httpTimeoutMs = 15000;                         // таймаут на выполнение http запроса
            uint8_t maxBootAttempts = 3;                            // сколько раз подряд можно перезагрузиться с неподтверждённой (pendingValidation) прошивкой, прежде чем begin() сам откатит устройство на предыдущий OTA-раздел

            enum class Policy {                                     // выбранная "политика" работы
                NotifyOnly,                                         // только уведомлять
                AutoInstall,                                        // автоматически обновляться при наличии возможности
            } policy = Policy::NotifyOnly;                          // по умолчанию: только уведомление
        };

        enum class State {                                          // состояния работы
            IDLE,                                                   // простой
            CHECKING,                                               // проверка наличия обновлений
            UP_TO_DATE,                                             // установлена последняя версия
            UPDATE_AVAILABLE,                                       // доступно обновление
            DOWNLOADING,                                            // обновление скачивается
            VERIFYING,                                              // обновление проверяется
            REBOOTING,                                              // перезагрузка для применения обновления
            CAUGHT_ERROR,                                           // произошла какая-то ошибка
            ROLLING_BACK,                                           // откатываемся назад: обновление сломало нормальную работу
        };

        enum class Status {                                         // статус (возвращается многими методами)
            SUCCESS,                                                // успех
            NOT_INITIALIZED,                                        // не инициализировано: вызывайте begin() перед использованием методов
            WIFI_NOT_CONNECTED,                                     // WiFi не подключен
            BUSY,                                                   // выполняется
            NETWORK_ERROR,                                          // ошибка сети
            HTTP_ERROR,                                             // ошибка http
            JSON_PARSE_ERROR,                                       // ошибка парсинга JSON
            NO_ASSET_FOUND,                                         // не найден файл ассетов
            INSUFFICIENT_SPACE,                                     // недостаточно свободного места              
            CHECKSUM_MISMATCH,                                      // ошибка проверки контрольной суммы
            FLASH_ERROR,                                            // ошибка записи/чтения/очистки flash
            ANOTHER_UPDATE_ERROR,                                   // другая ошибка при работе с Update()
            ALREADY_UP_TO_DATE,                                     // обновлено до последней версии
            UPDATE_AVAILABLE,                                       // доступно новое обновление
            INCORRECT_CONFIG,                                       // передан некорректный конфиг
            ROLLBACK_UNSUPPORTED,                                   // откат не поддеживается платформой (ESP8266)
        };
        
        enum class ComparisonResult {
            EQUALLY,
            MORE,
            LESS,
            ERROR_GET_SEMVER,
        };

        Status begin(const Config& cfg, GitHubOTAClient& networkClient);     // начало работы: сохраняет конфиг И проверяет в NVS, не является ли этот запуск "неподтверждённым" после недавнего OTA (см. confirmValid)
        Status handle();                                            // тикер из loop(): 1) если пришло время — checkUpdates()/update() по таймеру и Policy; 2) если ждём подтверждения новой прошивки — считает autoConfirmTimeoutMs и сам вызывает confirmValid(), если хост не вызвал явно
        Status checkUpdates();                                      // ручная проверка обновлений
        Status update();                                            // ручной запуск процесса обновления
        Status confirmValid();                                      // хост явно подтверждает: новая прошивка (после OTA-перезагрузки) работает штатно — сбрасывает pendingValidation/bootAttempts в NVS, чтобы устройство НЕ откатилось на предыдущую версию при следующей перезагрузке
        Status rejectAndRollback();                                 // хост явно просит немедленный откат на предыдущий раздел (не дожидаясь исчерпания maxBootAttemps)
        
        // Геттеры
        State getState() const {                                    // текущее состояние работы
            return _state;
        }
        Status getLastError() const {                               // получить последнюю ошибку
            return _lastError;
        }
        bool isPendingValidation() const {                          // ожидает запроса валидации
            return _persisted.pendingValidation;
        }
        void getAvailableVersion(char* vers, size_t buf_size) const;// получить номер версии, доступной для оформления

        // Коллбеки
        void onStateChange(void (*callback)(State newState));       // при смене состояния
        void onProgress(void (*callback)(size_t written, size_t total));    // при скачивании обновления
        


    private:
        struct __attribute__((packed)) PersistedState {
            char prevLabel[17] = {0};
            uint8_t bootAttempts = 0;
            bool pendingValidation = false;
        };
        
        void (*_stateCallback) (State newState) = nullptr;          // указатель на функцию - коллбэк смены состояния
        void (*_progressCallback)(size_t written, size_t total) = nullptr; // указатель на функцию - коллбэк прогресса скачивания обновления
        ComparisonResult versionComparison(const char*);
        Status mapUpdateError();                                    // переводит ошибки Update() класса в статусы типа Status
        GitHubOTAClient* _client = nullptr;
        Status setFailStatus(Status status);
        Status performRollback();
        void loadPersistedState();
        void savePersistedState();
        Config _config;
        State _state = State::IDLE;
        Status _lastError = Status::SUCCESS;
        bool _initialized = false;
        uint32_t _bootTimestamp = 0;
        char _availableVersion[16] = {0};
        uint32_t _lastCheckMillis = 0;
        PersistedState _persisted;
};