#include <GitHubOTA.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>


GitHubOTA::Status GitHubOTA::begin(const GitHubOTA::Config& cfg, NetworkClient& networkClient) {
    _config = cfg;
    _client = &networkClient;

    // Валидация введенных полей
    if (!strlen(_config.repoName) || !strlen(_config.repoOwner) || !strlen(_config.assetName) || !strlen(_config.currentVersion)) {
        _lastError = Status::INCORRECT_CONFIG;
        return _lastError;
    }

    _prefs.begin("gh_ota", false);
    _pendingValidation = _prefs.getBool("pendingValidation", false);

    if (!_pendingValidation) {                  // Обычный старт
        _initialized = true;
        _state = State::IDLE;
        return Status::SUCCESS;
    }

    else {                                      // "Испытательный срок" для новой версии прошивки
        _initialized = true;
        uint8_t boot_attempts = _prefs.getUChar("bootAttempts", 0);
        _prefs.putUChar("bootAttempts", ++boot_attempts);

        if (boot_attempts > _config.maxBootAttempts) {
            _prefs.putUChar("bootAttempts", 0);
            _prefs.putBool("pendingValidation", false);

            // выискиваем нужный раздел для отката
            char savedLabel[17] = {0};
            _prefs.getString("prevLabel", savedLabel, 17);
            const esp_partition_t* target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, savedLabel);
            if (target) esp_ota_set_boot_partition(target);


            _state = State::ROLLING_BACK;
            if (_stateCallback) _stateCallback(_state);
            ESP.restart();
            return Status::SUCCESS;                 // формальность компилятора
        }

        else {
            _pendingValidation = true;
            _bootTimestamp = millis();
            return Status::SUCCESS;
        }
    }
}

GitHubOTA::Status GitHubOTA::checkUpdates() {
    if (!_initialized)  return Status::NOT_INITIALIZED;
    _state = State::CHECKING;

    char url[256] = {0};
    snprintf(url, 256, "https://api.github.com/repos/%s/%s/releases/latest", _config.repoOwner, _config.repoName);

    HTTPClient http;
    http.begin(*_client, url);

    http.setTimeout(_config.httpTimeoutMs);
    int returned_code = http.GET();

    if (returned_code != 200) {
        http.end();
        if (returned_code < 0)  return Status::NETWORK_ERROR;           // ошибка сети (подключения)
        else    return Status::HTTP_ERROR;                              // ошибка в процессе запроса
    }
    // создаем фильтр: из всего ответа github API будем парсить только те поля, которые нам нужны и не тратить память на остальные
    JsonDocument filter;
    filter["tag_name"] = true;              // на данном этапе нужен только тэг


    // парсим Json
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    if (err)    return Status::JSON_PARSE_ERROR;

    ComparisonResult result = versionComparison(doc["tag_name"].as<const char*>());

    switch (result) {
        case ComparisonResult::ERROR_GET_SEMVER:
            return Status::JSON_PARSE_ERROR;

        case ComparisonResult::EQUALLY:
        case ComparisonResult::LESS:
            _state = State::UP_TO_DATE;
            return Status::ALREADY_UP_TO_DATE;
        
        case ComparisonResult::MORE:
            snprintf(availableVersion, sizeof(availableVersion), "%s" , doc["tag_name"].as<const char*>());
            _state = State::UPDATE_AVAILABLE;
            return Status::UPDATE_AVAILABLE;

        default:
            return Status::JSON_PARSE_ERROR;
        
    }
}

GitHubOTA::ComparisonResult GitHubOTA::versionComparison(const char* parsedVersion) {
    // v255.255.255

    int major1, major2, minor1, minor2, patch1, patch2;
    if (sscanf(parsedVersion + (parsedVersion[0] == 'v' ? 1 : 0), "%i.%i.%i", &major1, &minor1, &patch1) == 3 && sscanf(_config.currentVersion + (_config.currentVersion[0] =='v' ? 1 : 0), "%i.%i.%i", &major2, &minor2, &patch2) == 3) {
        if (major1 != major2)   return (major1 > major2) ? ComparisonResult::MORE : ComparisonResult::LESS;
        if (minor1 != minor2)   return (minor1 > minor2) ? ComparisonResult::MORE : ComparisonResult::LESS;
        if (patch1 == patch2)   return ComparisonResult::EQUALLY;
        else return (patch1 > patch2) ? ComparisonResult::MORE : ComparisonResult::LESS;
    }
    
    else return ComparisonResult::ERROR_GET_SEMVER;

}