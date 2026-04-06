# SPDX-FileCopyrightText: 2026 Alysson Souza e Silva <alysson@ll9.com.br>
# SPDX-License-Identifier: GPL-2.0-or-later

kcoreaddons_add_plugin(meteireann INSTALL_NAMESPACE "plasma/weather_ions")

target_sources(meteireann PRIVATE
    ion_meteireann.cpp
    ion_meteireann.h
)

target_include_directories(meteireann PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

ecm_qt_declare_logging_category(meteireann
    HEADER meteireann_debug.h
    IDENTIFIER WEATHER::ION::METEIREANN
    CATEGORY_NAME org.kde.weather.ion.meteireann
    DEFAULT_SEVERITY Info
    DESCRIPTION "Met Éireann ion plugin"
    EXPORT METEIREANN
)

target_link_libraries(meteireann PRIVATE
    ${PLASMAWEATHERION_LIBRARY}
    ${PLASMAWEATHERDATA_LIBRARY}
    KF6::KIOCore
    KF6::I18n
    KF6::UnitConversion
    Qt::Qml
)
