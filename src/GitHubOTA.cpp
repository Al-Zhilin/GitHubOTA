#include <GitHubOTA.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <Update.h>


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
            snprintf(_availableVersion, sizeof(_availableVersion), "%s" , doc["tag_name"].as<const char*>());
            _state = State::UPDATE_AVAILABLE;
            return Status::UPDATE_AVAILABLE;

        default:
            return Status::JSON_PARSE_ERROR;
        
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
        if (http_code < 0)  return Status::NETWORK_ERROR;               // ошибка сети (подключения)
        else    return Status::HTTP_ERROR;                              // ошибка в процессе запроса
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

    if (strlen(download_urls[0]) == 0 || strlen(download_urls[1]) == 0) return Status::NO_ASSET_FOUND;

    if (versionComparison(doc["tag_name"].as<const char*>()) != ComparisonResult::MORE) return Status::ALREADY_UP_TO_DATE;

    http.begin(*_client, download_urls[1]);
    http.setTimeout(_config.httpTimeoutMs);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    _state = State::DOWNLOADING;
    
    http_code = http.GET();                     // запрос 1: ХЭШ обновления
    if (http_code != 200) {
        http.end();
        if (http_code < 0)  return Status::NETWORK_ERROR;               // ошибка сети (подключения)
        else    return Status::HTTP_ERROR;                              // ошибка в процессе запроса
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
        if (http_code < 0)  return Status::NETWORK_ERROR;               // ошибка сети (подключения)
        else    return Status::HTTP_ERROR;                              // ошибка в процессе запроса
    }
    int contentLength = http.getSize();
    http.end();

    if (!Update.setMD5(MD5Hash.c_str())) {
        return Status::CHECKSUM_MISMATCH;
    }
    if (!Update.begin(contentLength)) {
        return mapUpdateError();
    }

    // Здесь нужно настроить onProgress (помоги разобраться)

    Update.writeStream(http.getStream());           // читаем файл как стрим поток

    _state = State::VERIFYING;

    if (!Update.end(true)) {
        return mapUpdateError();                    // пробразываем разшифрованную ошибку и выходим
    }

    // если все получилось - переходим к перезагрузке и применению ошибки
    _state = State::REBOOTING;
    _prefs.putBool("pendingValidation", true);
    _prefs.putUChar("bootAttempts", 0);
    _prefs.putString("prevLabel", esp_ota_get_running_partition()->label);
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
        case UPDATE_ERROR_SIGN:
            return Status::CHECKSUM_MISMATCH;
        default:
            return Status::ANOTHER_UPDATE_ERROR;
    }
}