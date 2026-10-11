LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE     := leaderboardredirect
LOCAL_LDLIBS     := -llog
LOCAL_SRC_FILES  := src/main.c \
    ../../AoQ-ModLoader-For-Quest/shared/aoqcore/aoq.c \
    ../../AoQ-ModLoader-For-Quest/shared/inline-hook/inlineHook.c \
    ../../AoQ-ModLoader-For-Quest/shared/inline-hook/relocate.c \
    ../../AoQ-ModLoader-For-Quest/shared/utils/utils.c \
    ../../AoQ-ModLoader-For-Quest/modmanager/modconfig.c \
    ../../AoQ-ModLoader-For-Quest/modmanager/cJSON.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../AoQ-ModLoader-For-Quest/modmanager
include $(BUILD_SHARED_LIBRARY)