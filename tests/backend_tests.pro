QT += core gui qml quick quickcontrols2 network concurrent testlib
CONFIG += c++17 testcase
TARGET = backend_tests
TEMPLATE = app
INCLUDEPATH += ../src
HEADERS += ../src/backend.h ../src/usage.h ../src/history.h
SOURCES += backend_tests.cpp ../src/backend.cpp ../src/usage.cpp ../src/history.cpp
RESOURCES += ../src/resources.qrc
