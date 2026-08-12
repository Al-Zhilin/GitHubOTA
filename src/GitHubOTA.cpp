#include <GitHubOTA.h>


GitHubOTA::Status GitHubOTA::begin(const GitHubOTA::Config& cfg) {
    _config = cfg;

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
            const esp_partition_t* rollback_targer = esp_ota_get_next_update_partition(NULL);
            esp_err_t err = esp_ota_set_boot_partition(rollback_targer);
            _prefs.putUChar("bootAttemps", 0);
            _prefs.putBool("pendingValidation", false);

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