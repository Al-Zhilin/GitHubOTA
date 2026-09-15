#include <GitHubOTA.h>
#include <ArduinoJson.h>

void GitHubOTA::onStateChange(void (*callback)(State newState)) {
    _stateCallback = callback;
}

void GitHubOTA::onProgress(void (*callback)(size_t written, size_t total)) {
    _progressCallback = callback;
}

GitHubOTA::Status GitHubOTA::begin(const GitHubOTA::Config& cfg, GitHubOTAClient& networkClient) {
    _config = cfg;
    _client = &networkClient;
    
    _lastCheckMillis = 0 - _config.checkIntervalMs;

    // Валидация введенных полей
    if (!strlen(_config.repoName) || !strlen(_config.repoOwner) || !strlen(_config.assetName) || !strlen(_config.currentVersion)) {
        setFailStatus(Status::INCORRECT_CONFIG);
        return _lastError;
    }

    EEPROM.begin(sizeof(PersistedState));
    loadPersistedState();                       // выгружаем данные из памяти

    if (!_persisted.pendingValidation) {                  // Обычный старт
        _initialized = true;
        _state = State::IDLE;
        return Status::SUCCESS;
    }

    else {                                      // "Испытательный срок" для новой версии прошивки
        _initialized = true;
        _persisted.bootAttempts++;
        savePersistedState();

        if (_persisted.bootAttempts > _config.maxBootAttempts) {
            return performRollback();
        }

        else {
            _bootTimestamp = millis();
            return Status::SUCCESS;
        }
    }
}

GitHubOTA::Status GitHubOTA::checkUpdates() {
    if (!_initialized)  return Status::NOT_INITIALIZED;
    _state = State::CHECKING;
    if (_stateCallback) _stateCallback(_state);

    char url[256] = {0};
    snprintf(url, 256, "https://api.github.com/repos/%s/%s/releases/latest", _config.repoOwner, _config.repoName);

    HTTPClient http;
    http.begin(*_client, url);

    http.setTimeout(_config.httpTimeoutMs);
    int returned_code = http.GET();

    if (returned_code != 200) {
        http.end();
        if (returned_code < 0)  return setFailStatus(Status::NETWORK_ERROR);           // ошибка сети (подключения)
        else    return setFailStatus(Status::HTTP_ERROR);                       // ошибка в процессе запроса
    }
    // создаем фильтр: из всего ответа github API будем парсить только те поля, которые нам нужны и не тратить память на остальные
    JsonDocument filter;
    filter["tag_name"] = true;              // на данном этапе нужен только тэг


    // парсим Json
    JsonDocument doc;
    manualStreamReader reader(_client, _config.httpTimeoutMs);
    DeserializationError err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    http.end();

    if (err)    return setFailStatus(Status::JSON_PARSE_ERROR);

    ComparisonResult result = versionComparison(doc["tag_name"].as<const char*>());

    switch (result) {
        case ComparisonResult::ERROR_GET_SEMVER:
            return setFailStatus(Status::JSON_PARSE_ERROR);

        case ComparisonResult::EQUALLY:
        case ComparisonResult::LESS:
            _state = State::UP_TO_DATE;
            if (_stateCallback) _stateCallback(_state);
            return Status::ALREADY_UP_TO_DATE;
        
        case ComparisonResult::MORE:
            snprintf(_availableVersion, sizeof(_availableVersion), "%s" , doc["tag_name"].as<const char*>());
            _state = State::UPDATE_AVAILABLE;
            if (_stateCallback) _stateCallback(_state);
            return Status::UPDATE_AVAILABLE;

        default:
            return setFailStatus(Status::JSON_PARSE_ERROR);
        
    }
}

GitHubOTA::ComparisonResult GitHubOTA::versionComparison(const char* parsedVersion) {
    // v255.255.255 или 255.255.255 - 2 варианта поддерживаемого тега

    int major1, major2, minor1, minor2, patch1, patch2;
    if (sscanf(parsedVersion + (parsedVersion[0] == 'v' ? 1 : 0), "%i.%i.%i", &major1, &minor1, &patch1) == 3 && sscanf(_config.currentVersion + (_config.currentVersion[0] =='v' ? 1 : 0), "%i.%i.%i", &major2, &minor2, &patch2) == 3) {
        if (major1 != major2)   return (major1 > major2) ? ComparisonResult::MORE : ComparisonResult::LESS;
        if (minor1 != minor2)   return (minor1 > minor2) ? ComparisonResult::MORE : ComparisonResult::LESS;
        if (patch1 == patch2)   return ComparisonResult::EQUALLY;
        else return (patch1 > patch2) ? ComparisonResult::MORE : ComparisonResult::LESS;
    }
    
    else return ComparisonResult::ERROR_GET_SEMVER;

}

GitHubOTA::Status GitHubOTA::update() {
    if (!_initialized)   return Status::NOT_INITIALIZED;

    char url[256] = {0};
    snprintf(url, 256, "https://api.github.com/repos/%s/%s/releases/latest", _config.repoOwner, _config.repoName);

    HTTPClient http;
    http.begin(*_client, url);

    http.setTimeout(_config.httpTimeoutMs);
    int http_code = http.GET();

    if (http_code != 200) {
        http.end();
        if (http_code < 0)  return setFailStatus(Status::NETWORK_ERROR);               // ошибка сети (подключения)
        else    return setFailStatus(Status::HTTP_ERROR);                              // ошибка в процессе запроса
    }
    // создаем фильтр: из всего ответа github API будем парсить только те поля, которые нам нужны и не тратить память на остальные
    JsonDocument filter;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;                     // имя файлов
    filter["assets"][0]["browser_download_url"] = true;     // ссылка на скачивание файла

    // парсим Json
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));

    http.end();

    char assetNameMD5[64] = "";  // ожидаемое имя файла с MD5 хэшем файла прошивки
    snprintf(assetNameMD5, 64, "%s.md5", _config.assetName);

    char download_urls[2][255] = {"", ""};                      // сохраняем 2 ссцылки: на файл прошивки и на его md5 хэш для проверки целостности скачивания

    for (JsonObject asset : doc["assets"].as<JsonArray>()) {
        if (!strcmp(asset["name"].as<const char*>(), _config.assetName)) {                              // если есть файл прошивки (название совпадает с конфигом) - сохраняем ссылку
            snprintf(download_urls[0], 255, "%s", asset["browser_download_url"].as<const char*>());
        }

        else if (!strcmp(asset["name"].as<const char*>(), assetNameMD5)) {                              // если есть файл md5 хэша прошивки - сохраняем
            snprintf(download_urls[1], 255, "%s", asset["browser_download_url"].as<const char*>());
        }
    }   

    if (strlen(download_urls[0]) == 0 || strlen(download_urls[1]) == 0) return setFailStatus(Status::NO_ASSET_FOUND);

    if (versionComparison(doc["tag_name"].as<const char*>()) != ComparisonResult::MORE) {
        _state = State::UP_TO_DATE;
        if (_stateCallback) _stateCallback(_state);
        return Status::ALREADY_UP_TO_DATE;
    }

    http.begin(*_client, download_urls[1]);
    http.setTimeout(_config.httpTimeoutMs);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    _state = State::DOWNLOADING;
    
    http_code = http.GET();                     // запрос 1: ХЭШ обновления
    if (http_code != 200) {
        http.end();
        if (http_code < 0)  return setFailStatus(Status::NETWORK_ERROR);               // ошибка сети (подключения)
        else    return setFailStatus(Status::HTTP_ERROR);                              // ошибка в процессе запроса
    }

    String MD5Hash = http.getString();
    MD5Hash.trim();
    http.end();

    http.begin(*_client, download_urls[0]);
    http.setTimeout(_config.httpTimeoutMs);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    http_code = http.GET();                     // запрос 2: сам файл обновленнной прошивки

    if (http_code != 200) {
        http.end();
        if (http_code < 0)  return setFailStatus(Status::NETWORK_ERROR);               // ошибка сети (подключения)
        else    return setFailStatus(Status::HTTP_ERROR);                              // ошибка в процессе запроса
    }
    int contentLength = http.getSize();
    http.end();

    if (!Update.setMD5(MD5Hash.c_str())) {
        return setFailStatus(Status::CHECKSUM_MISMATCH);
    }
    if (!Update.begin(contentLength)) {
        return setFailStatus(mapUpdateError());
    }

    if (_progressCallback) Update.onProgress(_progressCallback);

    Update.writeStream(http.getStream());           // читаем файл как стрим поток

    _state = State::VERIFYING;

    if (!Update.end(true)) {
        return setFailStatus(mapUpdateError());                    // пробразываем разшифрованную ошибку и выходим
    }

    // если все получилось - переходим к перезагрузке и применению ошибки
    _state = State::REBOOTING;
    _persisted.pendingValidation = true;
    _persisted.bootAttempts = 0;

    #if defined(ESP32)
        snprintf(_persisted.prevLabel, sizeof(_persisted.prevLabel), "%s", esp_ota_get_running_partition()->label);
    #endif

    savePersistedState();
    ESP.restart();
    
    return Status::SUCCESS;
}

void GitHubOTA::getAvailableVersion(char* vers, size_t buf_size) const {
    snprintf(vers, buf_size, "%s", _availableVersion);
}

GitHubOTA::Status GitHubOTA::mapUpdateError() {
    switch (Update.getError()) {
        case UPDATE_ERROR_WRITE:
        case UPDATE_ERROR_ERASE:
        case UPDATE_ERROR_READ:
            return Status::FLASH_ERROR;
        case UPDATE_ERROR_SPACE:
            return Status::INSUFFICIENT_SPACE;
        case UPDATE_ERROR_MD5:
#ifdef UPDATE_ERROR_SIGN
        case UPDATE_ERROR_SIGN:                // константа есть не во всех версиях Update.h — не завязываемся жёстко
#endif
            return Status::CHECKSUM_MISMATCH;
        default:
            return Status::ANOTHER_UPDATE_ERROR;
    }
}

GitHubOTA::Status GitHubOTA::setFailStatus(Status status) {
    _lastError = status;
    _state = State::CAUGHT_ERROR;
    if (_stateCallback) _stateCallback(_state);
    return status;
}

GitHubOTA::Status GitHubOTA::confirmValid() {
    if (!_initialized)  return Status::NOT_INITIALIZED;
    if (!_persisted.pendingValidation)    return Status::SUCCESS;

    _persisted.pendingValidation = false;
    _persisted.bootAttempts = 0;
    savePersistedState();

    return Status::SUCCESS;
}

GitHubOTA::Status GitHubOTA::rejectAndRollback() {
    if (!_initialized)  return Status::NOT_INITIALIZED;
    if (!_persisted.pendingValidation)    return Status::SUCCESS;

    return performRollback();  
}

GitHubOTA::Status GitHubOTA::performRollback() {
    _persisted.bootAttempts = 0;
    _persisted.pendingValidation = false;
    savePersistedState();

    #if defined(ESP32)  // выискиваем нужный раздел для отката, только для ESP32
        const esp_partition_t* target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, _persisted.prevLabel);
        if (target) esp_ota_set_boot_partition(target);
        _state = State::ROLLING_BACK;
        if (_stateCallback) _stateCallback(_state);
        ESP.restart();
        return Status::SUCCESS;                 // формальность компилятора
    #else
        return Status::ROLLBACK_UNSUPPORTED;
    #endif
}

GitHubOTA::Status GitHubOTA::handle() {
    if (!_initialized) return Status::NOT_INITIALIZED;

    if (_persisted.pendingValidation && _config.autoConfirmTimeoutMs != 0) {                 // включена функция автоподтверждения и обновление требует подтверждения
        if (millis() - _bootTimestamp >= _config.autoConfirmTimeoutMs) {           // прошивка без падения отработала испытательный срок - считаем ее успешной
            confirmValid();
        }
    }

    if (_config.checkIntervalMs != 0 && millis() - _lastCheckMillis >= _config.checkIntervalMs) {         //  автопроверка обновлений
        _lastCheckMillis = millis();
        Status checkStatus = checkUpdates(), updateStatus = Status::SUCCESS;

        if (checkStatus == Status::UPDATE_AVAILABLE && _config.policy == Config::Policy::AutoInstall)   updateStatus = update();   // если доступно обновление и выбранная политика работы разрешает - автоматически обновляемся
        else if (checkStatus != Status::ALREADY_UP_TO_DATE) return checkStatus;     // возникла ошибка при проверке обновлений
        if (updateStatus != Status::SUCCESS)    return updateStatus;
    }

    return Status::SUCCESS;
}

void GitHubOTA::loadPersistedState() {
    EEPROM.get(0, _persisted);
}

void GitHubOTA::savePersistedState() {
    PersistedState current;
    EEPROM.get(0, current);

    if (memcmp(&current, &_persisted, sizeof(PersistedState)) != 0) {              // пишем только если есть различия
        EEPROM.put(0, _persisted);
        EEPROM.commit();
    }
}