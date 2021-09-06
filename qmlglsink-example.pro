PKGCONFIG += gstreamer-1.0
CONFIG += qt c++14 link_pkgconfig moc
QT += core qml quick

TARGET = qmlglsink-example

SOURCES += src/main.cpp
HEADERS += src/ScopeGuard.hpp
OTHER_FILES += src/main.qml
RESOURCES += src/main.qrc

INCLUDEPATH += src

QMAKE_CXXFLAGS += -Wextra -Wall -std=c++14 -pedantic -fPIC -DPIC -O0 -g3 -ggdb
QMAKE_LFLAGS += -fPIC -DPIC

isEmpty(PREFIX) {
	PREFIX = /usr/local
}
target.path = $$PREFIX/bin
INSTALLS += target
