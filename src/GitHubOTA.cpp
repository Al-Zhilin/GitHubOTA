#include <GitHubOTA.h>


GitHubOTA::Status GitHubOTA::begin(const GitHubOTA::Config& cfg, Client& networkClient) {
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

