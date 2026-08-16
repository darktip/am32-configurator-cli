TEMPLATE = app
TARGET = am32-cli

CONFIG += console c++17
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += \
    main.cpp

win32 {
    DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX
}
