LOCAL_PATH := $(call my-dir)

# --- Define CURL Prebuilt ---
include $(CLEAR_VARS)
LOCAL_MODULE := curl_static
LOCAL_SRC_FILES := src/static/curl/$(TARGET_ARCH_ABI)/libcurl.a
include $(PREBUILT_STATIC_LIBRARY)

# --- Module: cnative (overlay client, runs inside the cheat app) ---
include $(CLEAR_VARS)

LOCAL_MODULE    := cnative

LOCAL_SRC_FILES := \
    src/Client.cpp \
    src/Namegun.cpp \
    src/Socket/client.cpp

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/src \
    $(LOCAL_PATH)/src/Includes \
    $(LOCAL_PATH)/src/Includes/curl \
    $(LOCAL_PATH)/src/Socket \
    $(LOCAL_PATH)/src/Unity

# Added -frtti to fix the dynamic_cast error
LOCAL_CFLAGS    := -Wno-error=format-security -fpermissive -fvisibility=hidden -fexceptions -frtti -DNDEBUG -Os -fdata-sections -ffunction-sections
LOCAL_CPPFLAGS  := -fvisibility=hidden -DNDEBUG -std=c++17 -fexceptions -frtti -Os -fdata-sections -ffunction-sections
LOCAL_LDFLAGS   := -Wl,--exclude-libs,ALL -s -latomic -Wl,--gc-sections -Wl,--icf=safe
LOCAL_LDLIBS    := -llog -landroid -lz

LOCAL_STATIC_LIBRARIES := curl_static
LOCAL_ARM_MODE  := arm

include $(BUILD_SHARED_LIBRARY)

# --- Module: bklkill (INJECTED payload, loaded inside com.dts.freefiremax) ---
# Same source as the legacy daemon, but built as a shared object: the constructor
# in Server.cpp starts the socket server + cheat threads in-process.
include $(CLEAR_VARS)
LOCAL_MODULE    := bklkill
LOCAL_SRC_FILES := src/Server.cpp src/Socket/server.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/src $(LOCAL_PATH)/src/Includes
LOCAL_CPPFLAGS  := -fvisibility=hidden -DNDEBUG -std=c++17 -fexceptions -frtti -Os -fdata-sections -ffunction-sections -DBKL_PAYLOAD
LOCAL_CFLAGS    := -Wno-error=format-security -fpermissive -fvisibility=hidden -DNDEBUG -fexceptions -frtti -Os
LOCAL_LDFLAGS   := -Wl,--exclude-libs,ALL -s -Wl,--gc-sections -Wl,--icf=safe
LOCAL_LDLIBS    := -llog -lz -ldl
LOCAL_ARM_MODE  := arm
include $(BUILD_SHARED_LIBRARY)

# --- Module: BKLKILL (legacy standalone root daemon, kept as fallback) ---
# This is now the primary external-mode binary: it attaches to the game through
# a memory backend (driver node / KPM / /proc/<pid>/mem) and never injects.
include $(CLEAR_VARS)
LOCAL_MODULE    := bklkilld
# ndk-build forbids an extension on LOCAL_MODULE_FILENAME for executables.
# Gradle stages this binary into jniLibs as libbklkilld.so before packaging.
LOCAL_MODULE_FILENAME := libbklkilld
LOCAL_SRC_FILES := \
    src/Server.cpp \
    src/Socket/server.cpp \
    src/External/MemBackend.cpp \
    src/External/Process.cpp \
    src/External/Scanner.cpp \
    src/External/Patcher.cpp \
    src/External/Engine.cpp \
    src/External/Ipc.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/src $(LOCAL_PATH)/src/Includes
LOCAL_CPPFLAGS  := -fvisibility=hidden -DNDEBUG -std=c++17 -fexceptions -frtti -Os -fdata-sections -ffunction-sections -DBKL_EXTERNAL_ENGINE
LOCAL_CFLAGS    := -Wno-error=format-security -fpermissive -fvisibility=hidden -fexceptions -frtti -DNDEBUG -Os
LOCAL_LDFLAGS   := -Wl,--exclude-libs,ALL -s -Wl,--gc-sections
LOCAL_LDLIBS    := -llog -lz
LOCAL_ARM_MODE  := arm
include $(BUILD_EXECUTABLE)

# --- Module: bkliject (root-side ptrace injector) ---
# Built as an executable, but named libbkliject.so so the APK packages it: the app
# extracts it to its private cache and runs it through `su`, then deletes it again.
include $(CLEAR_VARS)
LOCAL_MODULE    := bkliject
LOCAL_MODULE_FILENAME := libbkliject
LOCAL_SRC_FILES := src/Injector.cpp
LOCAL_CPPFLAGS  := -fvisibility=hidden -DNDEBUG -std=c++17 -Os -fdata-sections -ffunction-sections
LOCAL_CFLAGS    := -Wno-error=format-security -fpermissive -fvisibility=hidden -DNDEBUG -Os
LOCAL_LDFLAGS   := -Wl,--exclude-libs,ALL -s -Wl,--gc-sections
LOCAL_LDLIBS    := -llog
LOCAL_ARM_MODE  := arm
include $(BUILD_EXECUTABLE)
