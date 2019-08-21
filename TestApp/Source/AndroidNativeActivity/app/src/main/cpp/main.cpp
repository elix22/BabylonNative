/*
 * Copyright (C) 2010 The Android Open Source Project
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
 *
 */

//BEGIN_INCLUDE(all)
#include <initializer_list>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <jni.h>
#include <errno.h>
#include <cassert>

#include <EGL/egl.h>
#include <GLES/gl.h>

#include <android/sensor.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <Babylon/RuntimeAndroid.h>
#include <android/window.h>
#include <android/native_window.h>
#include <InputManager.h>

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "native-activity", __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, "native-activity", __VA_ARGS__))


std::unique_ptr<babylon::RuntimeAndroid> runtime{};
std::string androidPackagePath;
std::unique_ptr<InputManager::InputBuffer> inputBuffer{};


void AndroidLogMessage(const char* message)
{
    __android_log_write(ANDROID_LOG_INFO, "BabylonNative", message);
}

void AndroidWarnMessage(const char* message)
{
    __android_log_write(ANDROID_LOG_WARN, "BabylonNative", message);
}

void AndroidErrorMessage(const char* message)
{
    __android_log_write(ANDROID_LOG_ERROR, "BabylonNative", message);
}

/**
 * Shared state for our app.
 */
struct engine {
    struct android_app* app;
    ANativeWindow *m_window;
    ASensorManager* sensorManager;
    const ASensor* accelerometerSensor;
    ASensorEventQueue* sensorEventQueue;

    int32_t width;
    int32_t height;
};

/**
 * Process the next input event.
 */
static int32_t engine_handle_input(struct android_app* app, AInputEvent* event) {
    struct engine* engine = (struct engine*)app->userData;

    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        inputBuffer->SetPointerPosition(AMotionEvent_getX(event, 0),AMotionEvent_getY(event, 0));
        inputBuffer->SetPointerDown(AMotionEvent_getPressure(event, 0) >0.f);
        return 1;
    }
    return 0;
}

/**
 * Process the next main command.
 */
static void engine_handle_cmd(struct android_app* app, int32_t cmd) {
    struct engine* engine = (struct engine*)app->userData;
    switch (cmd) {
        case APP_CMD_SAVE_STATE:
            // The system has asked us to save our current state.
            break;
        case APP_CMD_INIT_WINDOW:
            // The window is being shown, get it ready.
            if (engine->m_window != app->window) {
                engine->m_window = app->window;
                int32_t width  = ANativeWindow_getWidth(engine->m_window);
                int32_t height = ANativeWindow_getHeight(engine->m_window);

                if (!runtime) {
                    // register console outputs
                    babylon::Runtime::RegisterLogOutput(AndroidLogMessage);
                    babylon::Runtime::RegisterWarnOutput(AndroidWarnMessage);
                    babylon::Runtime::RegisterErrorOutput(AndroidErrorMessage);

                    runtime = std::make_unique<babylon::RuntimeAndroid>(engine->m_window,
                                                                        "file:///data/local/tmp",
                                                                        width, height);

                    inputBuffer = std::make_unique<InputManager::InputBuffer>(*runtime);
                    InputManager::Initialize(*runtime, *inputBuffer);

                    runtime->LoadScript("Scripts/babylon.max.js");
                    runtime->LoadScript("Scripts/babylon.glTF2FileLoader.js");
                    runtime->LoadScript("Scripts/experience.js");
                } else {
                    runtime->SetWindow(engine->m_window);
                    runtime->UpdateSize(width, height);
                }
            }
            break;
        case APP_CMD_WINDOW_RESIZED:
            break;
        case APP_CMD_TERM_WINDOW:
            // The window is being hidden or closed, clean it up.
            //engine_term_display(engine);
            break;
        case APP_CMD_GAINED_FOCUS:
            // When our app gains focus, we start monitoring the accelerometer.
            if (engine->accelerometerSensor != NULL) {
                ASensorEventQueue_enableSensor(engine->sensorEventQueue,
                                               engine->accelerometerSensor);
                // We'd like to get 60 events per second (in us).
                ASensorEventQueue_setEventRate(engine->sensorEventQueue,
                                               engine->accelerometerSensor,
                                               (1000L/60)*1000);
            }
            break;
        case APP_CMD_LOST_FOCUS:
            // When our app loses focus, we stop monitoring the accelerometer.
            // This is to avoid consuming battery while not being used.
            if (engine->accelerometerSensor != NULL) {
                ASensorEventQueue_disableSensor(engine->sensorEventQueue,
                                                engine->accelerometerSensor);
            }
            //(engine);
            break;
    }
}

/*
 * AcquireASensorManagerInstance(void)
 *    Workaround ASensorManager_getInstance() deprecation false alarm
 *    for Android-N and before, when compiling with NDK-r15
 */
#include <dlfcn.h>
ASensorManager* AcquireASensorManagerInstance(android_app* app) {

  if(!app)
    return nullptr;

  typedef ASensorManager *(*PF_GETINSTANCEFORPACKAGE)(const char *name);
  void* androidHandle = dlopen("libandroid.so", RTLD_NOW);
  PF_GETINSTANCEFORPACKAGE getInstanceForPackageFunc = (PF_GETINSTANCEFORPACKAGE)
      dlsym(androidHandle, "ASensorManager_getInstanceForPackage");
  if (getInstanceForPackageFunc) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, NULL);

    jclass android_content_Context = env->GetObjectClass(app->activity->clazz);
    jmethodID midGetPackageName = env->GetMethodID(android_content_Context,
                                                   "getPackageName",
                                                   "()Ljava/lang/String;");
    jstring packageName= (jstring)env->CallObjectMethod(app->activity->clazz,
                                                        midGetPackageName);

    const char *nativePackageName = env->GetStringUTFChars(packageName, 0);
    ASensorManager* mgr = getInstanceForPackageFunc(nativePackageName);
    env->ReleaseStringUTFChars(packageName, nativePackageName);
    app->activity->vm->DetachCurrentThread();
    if (mgr) {
      dlclose(androidHandle);
      return mgr;
    }
  }

  typedef ASensorManager *(*PF_GETINSTANCE)();
  PF_GETINSTANCE getInstanceFunc = (PF_GETINSTANCE)
      dlsym(androidHandle, "ASensorManager_getInstance");
  // by all means at this point, ASensorManager_getInstance should be available
  assert(getInstanceFunc);
  dlclose(androidHandle);

  return getInstanceFunc();
}


/**
 * This is the main entry point of a native application that is using
 * android_native_app_glue.  It runs in its own thread, with its own
 * event loop for receiving input events and doing other things.
 */
void android_main(struct android_app* state) {
    struct engine engine;

    memset(&engine, 0, sizeof(engine));
    state->userData = &engine;
    state->onAppCmd = engine_handle_cmd;
    state->onInputEvent = engine_handle_input;
    ANativeActivity_setWindowFlags(state->activity, 0
                                                    | AWINDOW_FLAG_FULLSCREEN
                                                    | AWINDOW_FLAG_KEEP_SCREEN_ON
            , 0
    );
    engine.app = state;

    // Prepare to monitor accelerometer
    engine.sensorManager = AcquireASensorManagerInstance(state);
    engine.accelerometerSensor = ASensorManager_getDefaultSensor(
                                        engine.sensorManager,
                                        ASENSOR_TYPE_ACCELEROMETER);
    engine.sensorEventQueue = ASensorManager_createEventQueue(
                                    engine.sensorManager,
                                    state->looper, LOOPER_ID_USER,
                                    NULL, NULL);

    // loop waiting for stuff to do.
    while (0 == state->destroyRequested) {
        // Read all pending events.
        struct android_poll_source* source;

        int32_t num;
        ALooper_pollAll(-1, NULL, &num, (void**)&source);

        if (NULL != source)
        {
            source->process(state, source);
        }
    }
}
//END_INCLUDE(all)