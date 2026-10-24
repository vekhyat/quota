QT += core gui quick quickcontrols2 testlib dbus
CONFIG += c++17 testcase
TARGET = quota_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/usage.h ../src/theme.h ../src/translator.h
SOURCES += quota_tests.cpp ../src/usage.cpp ../src/theme.cpp ../src/translator.cpp
