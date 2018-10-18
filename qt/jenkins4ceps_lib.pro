#-------------------------------------------------
#
# Project created by QtCreator 2015-11-24T15:53:23
#
#-------------------------------------------------

QMAKE_CXXFLAGS += -O3 -g3 -Wall -MD -fmessage-length=0 -std=c++14 -Wl,--no-as-needed -fPIC

INCLUDEPATH += $$_PRO_FILE_PWD_/../..
INCLUDEPATH += $$_PRO_FILE_PWD_/../../ceps/core/include
INCLUDEPATH += $$_PRO_FILE_PWD_/../../sm4ceps
INCLUDEPATH += $$_PRO_FILE_PWD_/../../pugixml/src
INCLUDEPATH += $$_PRO_FILE_PWD_/../../rapidjson/include

QT       -= core gui

TARGET = jenkins4ceps
TEMPLATE = lib

LIBS+=-lmysqlcppconn


SOURCES   += $$_PRO_FILE_PWD_/../jenkins4ceps.cpp
SOURCES   += $$_PRO_FILE_PWD_/../../pugixml/src/pugixml.cpp
CEPSLIBS = $$_PRO_FILE_PWD_/../../ceps/core/bin

LIBS+=  $$CEPSLIBS/ceps_ast.o
LIBS+=  $$CEPSLIBS/ceps.tab.o
LIBS+=  $$CEPSLIBS/ceps_interpreter.o
LIBS+=  $$CEPSLIBS/cepsparserdriver.o
LIBS+=  $$CEPSLIBS/cepsruntime.o
LIBS+=  $$CEPSLIBS/cepslexer.o
LIBS+=  $$CEPSLIBS/symtab.o
LIBS+=  $$CEPSLIBS/ceps_interpreter_loop.o
LIBS+=  $$CEPSLIBS/ceps_interpreter_nodeset.o
LIBS+=  $$CEPSLIBS/ceps_interpreter_macros.o
LIBS+=  $$CEPSLIBS/ceps_interpreter_functions.o

QMAKE_POST_LINK += $$quote(cp libjenkins4ceps.so.1.0.0 ../../bin/libjenkins4ceps.so)

