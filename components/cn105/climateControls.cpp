#include "cn105.h"
#include "Globals.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>
#include <utility>

using namespace esphome;


void CN105Climate::checkPendingWantedSettings() {
    long now = CUSTOM_MILLIS;
    if (!(this->wantedSettings.hasChanged) || (now - this->wantedSettings.lastChange < this->debounce_delay_)) {
        return;
    }

    ESP_LOGI(LOG_ACTION_EVT_TAG, "checkPendingWantedSettings - wanted settings have changed, sending them to the heatpump...");
    this->sendWantedSettings();
}

void logCheckWantedSettingsMutex(wantedHeatpumpSettings& settings) {

    if (settings.hasBeenSent) {
        ESP_LOGE("control", "Mutex lock faillure: wantedSettings should be locked while sending.");
        ESP_LOGD("control", "-- This is an assertion test on wantedSettings.hasBeenSent");
        ESP_LOGD("control", "-- wantedSettings.hasBeenSent = true is unexpected");
        ESP_LOGD("control", "-- should be false because mutex should prevent running this while sending");
        ESP_LOGD("control", "-- and mutex should be released only when hasBeenSent is false");
    }

}
void CN105Climate::controlDelegate(const esphome::climate::ClimateCall& call) {
    ESP_LOGD("control", "espHome control() interface method called...");
    bool updated = false;

    logCheckWantedSettingsMutex(this->wantedSettings);

    // Traiter les commandes de climatisation ici
    if (call.get_mode().has_value()) {
        ESP_LOGD("control", "Mode change asked");
        // Changer le mode de climatisation
        this->mode = *call.get_mode();
        updated = true;
        controlMode();
    }

    if (call.get_target_temperature().has_value()) {
        // Changer la température cible
        ESP_LOGI("control", "Setting heatpump setpoint : %.1f", *call.get_target_temperature());

        // As call.get_target_temperature() already returns a UX normalized value, we should
        // assign to target_temperature directly instead of using set_target_temperature, which
        // would cause normalization to be applied a second time.
        this->target_temperature = *call.get_target_temperature();
        updated = true;
        controlTemperature();
    }

    if (call.get_fan_mode().has_value()) {
        ESP_LOGD("control", "Fan change asked");
        // Changer le mode de ventilation
        this->fan_mode = *call.get_fan_mode();
        updated = true;
        this->controlFan();
    }
    if (call.get_swing_mode().has_value()) {
        ESP_LOGD("control", "Swing change asked");
        // Changer le mode de balancement
        this->swing_mode = *call.get_swing_mode();
        updated = true;
        this->controlSwing();
    }

    if (updated) {
        ESP_LOGD(LOG_ACTION_EVT_TAG, "clim.control() -> User changed something...");
        logCheckWantedSettingsMutex(this->wantedSettings);
        this->wantedSettings.hasChanged = true;
        this->wantedSettings.hasBeenSent = false;
        this->wantedSettings.lastChange = CUSTOM_MILLIS;
        this->debugSettings("control (wantedSettings)", this->wantedSettings);
    }

}

void CN105Climate::control(const esphome::climate::ClimateCall& call) {

#ifdef USE_ESP32
    std::lock_guard<std::mutex> guard(wantedSettingsMutex);
    this->controlDelegate(call);
#else    
    this->emulateMutex("CONTROL_WANTED_SETTINGS", std::bind(&CN105Climate::controlDelegate, this, call));
#endif    

}


/**
 * @brief Controls the swing modes based on user selection.
 *
 * This function handles the logic for CLIMATE_SWING_OFF, VERTICAL, HORIZONTAL, and BOTH.
 * It is designed to be safe for units that do not support horizontal swing (wideVane)
 * and provides an intuitive user experience by preserving static vane settings when possible.
 */
void CN105Climate::controlSwing() {
    // Check if horizontal vane (wideVane) is supported by this unit at the beginning.
    bool wideVaneSupported = this->traits_.supports_swing_mode(climate::CLIMATE_SWING_HORIZONTAL);

    switch (this->swing_mode) {
    case climate::CLIMATE_SWING_OFF:
        // When swing is turned OFF, unconditionally set vanes to a default static position.
        // This ensures that any and all swinging motion stops, regardless of the previous state.
        this->setVaneSetting("AUTO");
        if (wideVaneSupported) {
            this->setWideVaneSetting("|");
        }
        break;

    case climate::CLIMATE_SWING_VERTICAL:
        // Turn on vertical swing.
        this->setVaneSetting("SWING");
        // If horizontal swing was also on AND is supported, turn it off to a default static position.
        // This correctly handles switching from BOTH to VERTICAL, while preserving any user's
        // static horizontal setting if it wasn't swinging.
        if (wideVaneSupported && strcmp(currentSettings.wideVane, "SWING") == 0) {
            this->setWideVaneSetting("|");
        }
        break;

    case climate::CLIMATE_SWING_HORIZONTAL:
        // If vertical swing was on, turn it off to a default static position.
        // This correctly handles switching from BOTH to HORIZONTAL, while preserving any user's
        // static vertical setting if it wasn't swinging.
        if (strcmp(currentSettings.vane, "SWING") == 0) {
            this->setVaneSetting("AUTO");
        }
        // Turn on horizontal swing, but only if the unit supports it.
        if (wideVaneSupported) {
            this->setWideVaneSetting("SWING");
        }
        break;

    case climate::CLIMATE_SWING_BOTH:
        // Turn on vertical swing.
        this->setVaneSetting("SWING");
        // Turn on horizontal swing, but only if the unit supports it.
        if (wideVaneSupported) {
            this->setWideVaneSetting("SWING");
        }
        break;

    default:
        ESP_LOGW(TAG, "control - received unsupported swing mode request.");
        break;
    }
}
void CN105Climate::controlFan() {

    switch (this->fan_mode.value()) {
    case climate::CLIMATE_FAN_OFF:
        this->setPowerSetting("OFF");
        break;
    case climate::CLIMATE_FAN_QUIET:
        this->setFanSpeed("QUIET");
        break;
    case climate::CLIMATE_FAN_DIFFUSE:
        this->setFanSpeed("QUIET");
        break;
    case climate::CLIMATE_FAN_LOW:
        this->setFanSpeed("1");
        break;
    case climate::CLIMATE_FAN_MEDIUM:
        this->setFanSpeed("2");
        break;
    case climate::CLIMATE_FAN_MIDDLE:
        this->setFanSpeed("3");
        break;
    case climate::CLIMATE_FAN_HIGH:
        this->setFanSpeed("4");
        break;
    case climate::CLIMATE_FAN_ON:
    case climate::CLIMATE_FAN_AUTO:
    default:
        this->setFanSpeed("AUTO");
        break;
    }
}

void CN105Climate::controlTemperature() {
    float setting = this->get_target_temperature();
    if (!this->tempMode) {
        this->wantedSettings.temperature = this->lookupByteMapIndex(TEMP_MAP, 16, (int)(setting + 0.5)) > -1 ? setting : TEMP_MAP[0];
    } else {
        setting = std::round(2.0f * setting) / 2.0f;  // Round to the nearest half-degree.
        this->wantedSettings.temperature = setting < 10 ? 10 : (setting > 31 ? 31 : setting);
    }
}


void CN105Climate::controlMode() {
    switch (this->mode) {
    case climate::CLIMATE_MODE_COOL:
        ESP_LOGI("control", "changing mode to COOL");
        this->setModeSetting("COOL");
        this->setPowerSetting("ON");
        break;
    case climate::CLIMATE_MODE_HEAT:
        ESP_LOGI("control", "changing mode to HEAT");
        this->setModeSetting("HEAT");
        this->setPowerSetting("ON");
        break;
    case climate::CLIMATE_MODE_DRY:
        ESP_LOGI("control", "changing mode to DRY");
        this->setModeSetting("DRY");
        this->setPowerSetting("ON");
        break;
    case climate::CLIMATE_MODE_AUTO:
        ESP_LOGI("control", "changing mode to AUTO");
        this->setModeSetting("AUTO");
        this->setPowerSetting("ON");
        break;
    case climate::CLIMATE_MODE_FAN_ONLY:
        ESP_LOGI("control", "changing mode to FAN_ONLY");
        this->setModeSetting("FAN");
        this->setPowerSetting("ON");
        break;
    case climate::CLIMATE_MODE_OFF:
        ESP_LOGI("control", "changing mode to OFF");
        this->setPowerSetting("OFF");
        break;
    default:
        ESP_LOGW("control", "unsupported mode");
    }
}


void CN105Climate::setActionIfOperatingTo(climate::ClimateAction action_if_operating) {
    bool effective_operating_status = this->currentStatus.operating; // Valeur par défaut depuis paquet 0x06

    if (this->use_stage_for_operating_status_) {
        bool stage_is_active = false;
        // Accéder à l'état actuel du stage_sensor
        // this->currentSettings.stage est mis à jour dans getPowerFromResponsePacket
        // lorsque le stage_sensor_ (s'il est configuré) publie son état.
        if (this->currentSettings.stage != nullptr &&
            strcmp(this->currentSettings.stage, STAGE_MAP[0 /*IDLE*/]) != 0) {
            stage_is_active = true;
        }

        // for fwump38 issue #277 (where paquet 0x06 does not give a reliable state for 'operating'),
        // on se base principalement sur 'stage_is_active'.
        // Si le paquet 0x06 *donne* un 'operating = true', on le garde.
        // Sinon (0x06 dit false OU 0x06 n'est pas fiable/reçu), on regarde stage.
        // Une logique possible: si 0x06 dit "operating", c'est "operating". Sinon, si fallback activé, stage décide.
        if (!effective_operating_status) { // Si 0x06 n'a pas dit "operating"
            effective_operating_status = stage_is_active;
        }
        // Autre logique plus directe pour fwump38:
        // effective_operating_status = stage_is_active; // Si on veut que stage ait la priorité ou soit la seule source quand fallback est true.
        // Choisissons pour l'instant: le stage peut rendre "operating" true si 0x06 ne l'a pas déjà fait,
        // mais ne peut pas le rendre false si 0x06 l'a mis à true (sauf si stage est IDLE).
        // Pour fwump38, son 0x06 ne renvoyait rien, donc effective_operating_status serait false au départ.
        // Sa logique était: effective_operating_status = stage_is_active;
        // Adoptons cela pour le fallback:
        effective_operating_status = stage_is_active; // Si fallback est activé, stage dicte.
        // Attention: cela ignore complètement le data[4] de 0x06 si fallback est true.
        // C'est ce que fwump38 a fait pour son cas.
    }

    if (effective_operating_status) {
        this->action = action_if_operating;
    } else {
        this->action = climate::CLIMATE_ACTION_IDLE;
    }
    ESP_LOGD(TAG, "Setting action to %d (effective_operating: %s, use_stage_fallback: %s, current_stage: %s)",
        static_cast<int>(this->action),
        effective_operating_status ? "true" : "false",
        this->use_stage_for_operating_status_ ? "yes" : "no",
        this->currentSettings.stage ? this->currentSettings.stage : "N/A");
}

/**
 * Thanks to Bascht74 on issu #9 we know that the compressor frequency is not a good indicator of the heatpump being in operation
 * Because one can have multiple inside module for a single compressor.
 * Also, some heatpump does not support compressor frequency.
 * SO usage is deprecated.
*/
void CN105Climate::setActionIfOperatingAndCompressorIsActiveTo(climate::ClimateAction action) {

    ESP_LOGW(TAG, "Warning: the use of compressor frequency as an active indicator is deprecated. Please use operating status instead.");

    if (currentStatus.compressorFrequency <= 0) {
        this->action = climate::CLIMATE_ACTION_IDLE;
    } else {
        this->setActionIfOperatingTo(action);
    }
}

//inside the below we could implement an internal only HEAT_COOL doing the math with an offset or something
void CN105Climate::updateAction() {
    ESP_LOGV(TAG, "updating action back to espHome...");
    switch (this->mode) {
    case climate::CLIMATE_MODE_HEAT:
        //this->setActionIfOperatingAndCompressorIsActiveTo(climate::CLIMATE_ACTION_HEATING);       
        this->setActionIfOperatingTo(climate::CLIMATE_ACTION_HEATING);
        break;
    case climate::CLIMATE_MODE_COOL:
        //this->setActionIfOperatingAndCompressorIsActiveTo(climate::CLIMATE_ACTION_COOLING);
        this->setActionIfOperatingTo(climate::CLIMATE_ACTION_COOLING);
        break;
    case climate::CLIMATE_MODE_AUTO:
        if (this->traits().supports_mode(climate::CLIMATE_MODE_HEAT) &&
            this->traits().supports_mode(climate::CLIMATE_MODE_COOL)) {
            // If the unit supports both heating and cooling
            this->setActionIfOperatingTo(
                (this->current_temperature > this->target_temperature ?
                    climate::CLIMATE_ACTION_COOLING :
                    climate::CLIMATE_ACTION_HEATING));
        } else if (this->traits().supports_mode(climate::CLIMATE_MODE_COOL)) {
            // If the unit only supports cooling
            if (this->current_temperature <= this->target_temperature) {
                // If the temperature meets or exceeds the target, switch to fan-only mode
                this->setActionIfOperatingTo(climate::CLIMATE_ACTION_FAN);
            } else {
                // Otherwise, continue cooling
                this->setActionIfOperatingTo(climate::CLIMATE_ACTION_COOLING);
            }
        } else if (this->traits().supports_mode(climate::CLIMATE_MODE_HEAT)) {
            // If the unit only supports heating
            if (this->current_temperature >= this->target_temperature) {
                // If the temperature meets or exceeds the target, switch to fan-only mode
                this->setActionIfOperatingTo(climate::CLIMATE_ACTION_FAN);
            } else {
                // Otherwise, continue heating
                this->setActionIfOperatingTo(climate::CLIMATE_ACTION_HEATING);
            }
        } else {
            ESP_LOGE(TAG, "AUTO mode is not supported by this unit");
        }
        break;

    case climate::CLIMATE_MODE_DRY:
        //this->setActionIfOperatingAndCompressorIsActiveTo(climate::CLIMATE_ACTION_DRYING);
        this->setActionIfOperatingTo(climate::CLIMATE_ACTION_DRYING);
        break;
    case climate::CLIMATE_MODE_FAN_ONLY:
        this->action = climate::CLIMATE_ACTION_FAN;
        break;
    default:
        this->action = climate::CLIMATE_ACTION_OFF;
    }

    ESP_LOGD(TAG, "Climate mode is: %i", this->mode);
    ESP_LOGD(TAG, "Climate action is: %i", this->action);
}

climate::ClimateTraits CN105Climate::traits() {
    return traits_;
}


/**
 * Modify our supported traits.
 *
 * Returns:
 *   A reference to this class' supported climate::ClimateTraits.
 */
climate::ClimateTraits& CN105Climate::config_traits() {
    return traits_;
}


void CN105Climate::setModeSetting(const char* setting) {
    int index = lookupByteMapIndex(MODE_MAP, 5, setting);
    if (index > -1) {
        wantedSettings.mode = MODE_MAP[index];
    } else {
        wantedSettings.mode = MODE_MAP[0];
    }
}

void CN105Climate::setPowerSetting(const char* setting) {
    int index = lookupByteMapIndex(POWER_MAP, 2, setting);
    if (index > -1) {
        wantedSettings.power = POWER_MAP[index];
    } else {
        wantedSettings.power = POWER_MAP[0];
    }
}

void CN105Climate::setFanSpeed(const char* setting) {
    int index = lookupByteMapIndex(FAN_MAP, 6, setting);
    if (index > -1) {
        wantedSettings.fan = FAN_MAP[index];
    } else {
        wantedSettings.fan = FAN_MAP[0];
    }
}

void CN105Climate::setVaneSetting(const char* setting) {
    int index = lookupByteMapIndex(VANE_MAP, 7, setting);
    if (index > -1) {
        wantedSettings.vane = VANE_MAP[index];
    } else {
        wantedSettings.vane = VANE_MAP[0];
    }
}

void CN105Climate::setWideVaneSetting(const char* setting) {
    int index = lookupByteMapIndex(WIDEVANE_MAP, 11, setting);
    if (index > -1) {
        wantedSettings.wideVane = WIDEVANE_MAP[index];
    } else {
        wantedSettings.wideVane = WIDEVANE_MAP[0];
    }
}

void CN105Climate::set_current_temperature(float temperature) {
    this->current_temperature = this->fahrenheitSupport_.normalizeCelsiusForConversionToFahrenheit(temperature);
}

void CN105Climate::set_remote_temperature(float setting) {
    this->shouldSendExternalTemperature_ = true;
    this->remoteTemperature_ = this->fahrenheitSupport_.normalizeCelsiusForConversionFromFahrenheit(setting);
    ESP_LOGD(LOG_REMOTE_TEMP, "setting remote temperature to %f", this->remoteTemperature_);
}

void CN105Climate::set_target_temperature(float temperature) {
    this->target_temperature = this->fahrenheitSupport_.normalizeCelsiusForConversionToFahrenheit(temperature);
}

float CN105Climate::get_target_temperature() {
    return this->fahrenheitSupport_.normalizeCelsiusForConversionFromFahrenheit(this->target_temperature);
}