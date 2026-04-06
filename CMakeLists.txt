# SPDX-FileCopyrightText: 2026 Alysson Souza e Silva <alysson@ll9.com.br>
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.16)

set(PROJECT_VERSION "0.1.0")
project(plasma-ion-meteireann VERSION ${PROJECT_VERSION})

set(QT_MIN_VERSION "6.10.0")
set(KF6_MIN_VERSION "6.22.0")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQtDeclareLoggingCategory)
include(FeatureSummary)

find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Core Qml)
find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS
    CoreAddons
    KIO
    I18n
    UnitConversion
)

find_library(PLASMAWEATHERION_LIBRARY NAMES plasmaweatherion REQUIRED)
find_library(PLASMAWEATHERDATA_LIBRARY NAMES plasmaweatherdata REQUIRED)

add_subdirectory(plugin)

install(FILES
    NOTICE.met-eireann.txt
    DESTINATION ${KDE_INSTALL_DATADIR}/doc/${PROJECT_NAME}
)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
