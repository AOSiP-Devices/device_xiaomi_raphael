/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#define LOG_TAG "light"
 
#define DIM_ALPHA_PATH "sys/class/drm/card0-DSI-1/dim_alpha"
 
 
#define MAXIMUM_DISPLAY_BRIGHTNESS 2047
#define MINIMUM_DISPLAY_BRIGHTNESS 1
#define MAXIMUM_DISPLAY_SCALED_BRIGHTNESS 255
#define MINIMUM_DISPLAY_SCALED_BRIGHTNESS 1
 
#include <log/log.h>
 
#include <cmath>
#include <fstream>
#include <stdio.h>
 
#include "Light.h"
 
namespace android {
namespace hardware {
namespace light {
namespace V2_0 {
namespace implementation {
 
template <typename T>
static void set(const std::string& path, const T& value) {
    std::ofstream file(path);
    file << value;
}
 
static_assert(LIGHT_FLASH_NONE == static_cast<int>(Flash::NONE),
    "Flash::NONE must match legacy value.");
static_assert(LIGHT_FLASH_TIMED == static_cast<int>(Flash::TIMED),
    "Flash::TIMED must match legacy value.");
static_assert(LIGHT_FLASH_HARDWARE == static_cast<int>(Flash::HARDWARE),
    "Flash::HARDWARE must match legacy value.");
 
static_assert(BRIGHTNESS_MODE_USER == static_cast<int>(Brightness::USER),
    "Brightness::USER must match legacy value.");
static_assert(BRIGHTNESS_MODE_SENSOR == static_cast<int>(Brightness::SENSOR),
    "Brightness::SENSOR must match legacy value.");
static_assert(BRIGHTNESS_MODE_LOW_PERSISTENCE ==
    static_cast<int>(Brightness::LOW_PERSISTENCE),
    "Brightness::LOW_PERSISTENCE must match legacy value.");
 
Light::Light(std::map<Type, light_device_t*> &&lights)
  : mLights(std::move(lights)) {}
 
// Methods from ::android::hardware::light::V2_0::ILight follow.
Return<Status> Light::setLight(Type type, const LightState& state)  {
    auto it = mLights.find(type);
 
    if (it == mLights.end()) {
        return Status::LIGHT_NOT_SUPPORTED;
    }
 
    light_device_t* hwLight = it->second;
 
    light_state_t legacyState {
        .color = state.color,
        .flashMode = static_cast<int>(state.flashMode),
        .flashOnMS = state.flashOnMs,
        .flashOffMS = state.flashOffMs,
        .brightnessMode = static_cast<int>(state.brightnessMode),
    };
 
    // Scale display brightness.
    if (type == Type::BACKLIGHT) {
        legacyState.color = (MAXIMUM_DISPLAY_BRIGHTNESS - MINIMUM_DISPLAY_BRIGHTNESS) * ((state.color & 0xFF) - 1) / (MAXIMUM_DISPLAY_SCALED_BRIGHTNESS - MINIMUM_DISPLAY_SCALED_BRIGHTNESS) + 1;
        float alpha = 1.0 - pow(legacyState.color / 2047.0 * 430.0 / 600.0, 0.455);
        int dim_alpha = alpha * 255;
        set(DIM_ALPHA_PATH, dim_alpha);
    }
 
    int ret = hwLight->set_light(hwLight, &legacyState);
 
    switch (ret) {
        case -ENOSYS:
            return Status::BRIGHTNESS_NOT_SUPPORTED;
        case 0:
            return Status::SUCCESS;
        default:
            return Status::UNKNOWN;
    }
}
 
Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb)  {
    Type *types = new Type[mLights.size()];
 
    int idx = 0;
    for(auto const &pair : mLights) {
        Type type = pair.first;
 
        types[idx++] = type;
    }
 
    {
        hidl_vec<Type> hidl_types{};
        hidl_types.setToExternal(types, mLights.size());
 
        _hidl_cb(hidl_types);
    }
 
    delete[] types;
 
    return Void();
}
 
const static std::map<Type, const char*> kLogicalLights = {
    {Type::BACKLIGHT,     LIGHT_ID_BACKLIGHT},
    {Type::KEYBOARD,      LIGHT_ID_KEYBOARD},
    {Type::BUTTONS,       LIGHT_ID_BUTTONS},
    {Type::BATTERY,       LIGHT_ID_BATTERY},
    {Type::NOTIFICATIONS, LIGHT_ID_NOTIFICATIONS},
    {Type::ATTENTION,     LIGHT_ID_ATTENTION},
    {Type::BLUETOOTH,     LIGHT_ID_BLUETOOTH},
    {Type::WIFI,          LIGHT_ID_WIFI}
};
 
Return<void> Light::debug(const hidl_handle& handle, const hidl_vec<hidl_string>& /* options */) {
    if (handle == nullptr || handle->numFds < 1) {
        ALOGE("debug called with no handle\n");
        return Void();
    }
 
    int fd = handle->data[0];
    if (fd < 0) {
        ALOGE("invalid FD: %d\n", handle->data[0]);
        return Void();
    }
 
    dprintf(fd, "The following lights are registered: ");
    for (auto const& pair : mLights) {
        const Type type = pair.first;
        dprintf(fd, "%s,", kLogicalLights.at(type));
    }
    dprintf(fd, ".\n");
    fsync(fd);
    return Void();
}
 
light_device_t* getLightDevice(const char* name) {
    light_device_t* lightDevice;
    const hw_module_t* hwModule = NULL;
 
    int ret = hw_get_module (LIGHTS_HARDWARE_MODULE_ID, &hwModule);
    if (ret == 0) {
        ret = hwModule->methods->open(hwModule, name,
            reinterpret_cast<hw_device_t**>(&lightDevice));
        if (ret != 0) {
            ALOGE("light_open %s %s failed: %d", LIGHTS_HARDWARE_MODULE_ID, name, ret);
        }
    } else {
        ALOGE("hw_get_module %s %s failed: %d", LIGHTS_HARDWARE_MODULE_ID, name, ret);
    }
 
    if (ret == 0) {
        return lightDevice;
    } else {
        ALOGE("Light passthrough failed to load legacy HAL.");
        return nullptr;
    }
}
 
ILight* HIDL_FETCH_ILight(const char* /* name */) {
    std::map<Type, light_device_t*> lights;
 
    for(auto const &pair : kLogicalLights) {
        Type type = pair.first;
        const char* name = pair.second;
 
        light_device_t* light = getLightDevice(name);
 
        if (light != nullptr) {
            lights[type] = light;
        }
    }
 
    if (lights.size() == 0) {
        // Log information, but still return new Light.
        // Some devices may not have any lights.
        ALOGI("Could not open any lights.");
    }
 
    return new Light(std::move(lights));
}
 
} // namespace implementation
}  // namespace V2_0
}  // namespace light
}  // namespace hardware
}  // namespace android