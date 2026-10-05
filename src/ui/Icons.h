#pragma once

// Font Awesome icon choices that depend on data rather than on a fixed
// control. Header-only so they can be unit tested without a UI.

#include <string_view>

#include <IconsFontAwesome6.h>

namespace em::icons {

// Battery glyph for a charge level in percent. Negative means unknown.
[[nodiscard]] inline const char* battery(double levelPct) noexcept {
    if (levelPct < 0.0) return ICON_FA_BATTERY_EMPTY;
    if (levelPct >= 90.0) return ICON_FA_BATTERY_FULL;
    if (levelPct >= 60.0) return ICON_FA_BATTERY_THREE_QUARTERS;
    if (levelPct >= 35.0) return ICON_FA_BATTERY_HALF;
    if (levelPct >= 10.0) return ICON_FA_BATTERY_QUARTER;
    return ICON_FA_BATTERY_EMPTY;
}

// By Android sensor type constant (android.hardware.Sensor.TYPE_*).
[[nodiscard]] inline const char* sensor(int typeId) noexcept {
    switch (typeId) {
        case 1:   // ACCELEROMETER
        case 10:  // LINEAR_ACCELERATION
        case 35:  // ACCELEROMETER_UNCALIBRATED
            return ICON_FA_UP_DOWN_LEFT_RIGHT;
        case 2:   // MAGNETIC_FIELD
        case 14:  // MAGNETIC_FIELD_UNCALIBRATED
            return ICON_FA_MAGNET;
        case 4:   // GYROSCOPE
        case 16:  // GYROSCOPE_UNCALIBRATED
            return ICON_FA_ROTATE;
        case 5:   return ICON_FA_LIGHTBULB;          // LIGHT
        case 6:   return ICON_FA_GAUGE;              // PRESSURE
        case 8:   return ICON_FA_HAND;               // PROXIMITY
        case 9:   return ICON_FA_ARROW_DOWN;         // GRAVITY
        case 3:                                      // ORIENTATION
        case 11:                                     // ROTATION_VECTOR
        case 15:                                     // GAME_ROTATION_VECTOR
        case 20:                                     // GEOMAGNETIC_ROTATION_VECTOR
            return ICON_FA_COMPASS;
        case 12:  return ICON_FA_DROPLET;            // RELATIVE_HUMIDITY
        case 13:  return ICON_FA_TEMPERATURE_HALF;   // AMBIENT_TEMPERATURE
        case 17:                                     // SIGNIFICANT_MOTION
        case 30:  return ICON_FA_PERSON_RUNNING;     // MOTION_DETECT
        case 18:                                     // STEP_DETECTOR
        case 19:  return ICON_FA_SHOE_PRINTS;        // STEP_COUNTER
        case 21:  return ICON_FA_HEART_PULSE;        // HEART_RATE
        default:  return ICON_FA_SATELLITE_DISH;
    }
}

// For the section titles DeviceInfo produces. Matched on the title text, which
// is what DeviceInfo.cpp emits; an unknown title gets a neutral icon.
[[nodiscard]] inline const char* deviceInfoSection(std::string_view title) noexcept {
    if (title == "Identity") return ICON_FA_ID_CARD;
    if (title == "OS and build") return ICON_FA_CODE_BRANCH;
    if (title == "SoC and CPU") return ICON_FA_MICROCHIP;
    if (title == "Display and graphics") return ICON_FA_DISPLAY;
    if (title == "Memory and storage") return ICON_FA_HARD_DRIVE;
    if (title == "Network") return ICON_FA_WIFI;
    if (title == "Debug and tracing") return ICON_FA_BUG;
    return ICON_FA_CIRCLE_INFO;
}

}  // namespace em::icons
