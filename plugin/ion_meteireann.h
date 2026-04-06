/*
    SPDX-FileCopyrightText: 2026 Alysson Souza e Silva <alysson@ll9.com.br>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ion.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QTimeZone>

#include <forecast.h>
#include <locations.h>

class KJob;
namespace KIO
{
class TransferJob;
}

class MetEireannIon : public Ion
{
    Q_OBJECT

public:
    explicit MetEireannIon(QObject *parent, const QVariantList &args);
    ~MetEireannIon() override;

    void findPlaces(std::shared_ptr<QPromise<std::shared_ptr<Locations>>> promise, const QString &searchString) override;
    void fetchForecast(std::shared_ptr<QPromise<std::shared_ptr<Forecast>>> promise, const QString &placeInfo) override;

private:
    struct LocationInfo {
        QString name;
        QString county;
        QString province;
        QString slug;
        double latitude = qQNaN();
        double longitude = qQNaN();
    };

    struct InstantForecast {
        QDateTime time;
        double latitude = qQNaN();
        double longitude = qQNaN();
        double temperature = qQNaN();
        double humidity = qQNaN();
        double pressure = qQNaN();
        double dewpoint = qQNaN();
        double windSpeed = qQNaN();
        double windGust = qQNaN();
        double windDirectionDegrees = qQNaN();
        QString windDirectionName;
    };

    struct IntervalForecast {
        QDateTime from;
        QDateTime to;
        QString symbolId;
        int symbolNumber = -1;
        double precipitation = qQNaN();
        double probability = qQNaN();
    };

    struct DayPartSummary {
        const IntervalForecast *interval = nullptr;
        double highTemp = qQNaN();
        double lowTemp = qQNaN();
        double probability = qQNaN();
    };

    struct LocationRequestState {
        QByteArray data;
        QString searchString;
        std::shared_ptr<QPromise<std::shared_ptr<Locations>>> promise;
    };

    struct ForecastRequestState {
        QByteArray data;
        LocationInfo locationInfo;
        QList<InstantForecast> instantForecasts;
        QList<IntervalForecast> intervalForecasts;
        std::shared_ptr<QPromise<std::shared_ptr<Forecast>>> promise;
    };

    KIO::TransferJob *requestApiJob(const QUrl &url, bool needsAjaxHeaders = false);

    void cancelLocationRequest();
    void cancelForecastRequest();

    void parseLocationsData(const QByteArray &data);
    bool parseForecastData(const QByteArray &data, QList<InstantForecast> &instantForecasts, QList<IntervalForecast> &intervalForecasts);
    void updateWeather(const ForecastRequestState &request);

    LocationInfo parsePlaceInfo(const QString &placeInfo) const;
    QString buildPlaceInfo(const LocationInfo &location) const;

    const InstantForecast *currentObservation(const QList<InstantForecast> &instantForecasts) const;
    const IntervalForecast *matchingInterval(const QList<IntervalForecast> &intervalForecasts, const QDateTime &time) const;
    DayPartSummary summarizeDayPart(const QList<InstantForecast> &instantForecasts, const QList<IntervalForecast> &intervalForecasts, const QDate &date, bool isNight) const;

    QDate dayPartDate(const QDateTime &dateTime, bool *isNight = nullptr) const;
    QDateTime dayPartTarget(const QDate &date, bool isNight) const;

    QString conditionText(const QString &symbolId) const;
    ConditionIcons conditionIcon(const QString &symbolId, bool isNight) const;
    QString weatherIconName(const QString &symbolId, bool isNight) const;
    QString windDirectionFromDegrees(double degrees) const;

private Q_SLOTS:
    void locationsJobFinished(KJob *job);
    void forecastJobFinished(KJob *job);

private:
    QHash<KJob *, std::shared_ptr<LocationRequestState>> m_locationRequests;
    QHash<KJob *, std::shared_ptr<ForecastRequestState>> m_forecastRequests;
    KJob *m_activeLocationJob = nullptr;
    KJob *m_activeForecastJob = nullptr;
};
