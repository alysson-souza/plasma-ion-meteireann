/*
    SPDX-FileCopyrightText: 2026 Alysson Souza e Silva <alysson@ll9.com.br>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ion_meteireann.h"

#include "meteireann_debug.h"

#include <KIO/TransferJob>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KUnitConversion/Converter>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTime>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <cmath>

K_PLUGIN_CLASS_WITH_JSON(MetEireannIon, "metadata.json")

using namespace KUnitConversion;
using namespace Qt::StringLiterals;

namespace
{
constexpr QLatin1StringView LOCATION_API_BASE("https://www.met.ie/api/location/");
constexpr QLatin1StringView FORECAST_API_BASE("http://openaccess.pf.api.met.ie/metno-wdb2ts/locationforecast");
constexpr int MAX_FORECAST_DAYS = 7;

QTimeZone irelandTimeZone()
{
    static const QTimeZone zone("Europe/Dublin");
    return zone.isValid() ? zone : QTimeZone::utc();
}

double parseDouble(QAnyStringView value)
{
    bool ok = false;
    const double parsed = value.toString().toDouble(&ok);
    return ok ? parsed : qQNaN();
}

int parseInt(QAnyStringView value)
{
    bool ok = false;
    const int parsed = value.toString().toInt(&ok);
    return ok ? parsed : -1;
}
}

MetEireannIon::MetEireannIon(QObject *parent, const QVariantList &args)
    : Ion(parent)
{
    Q_UNUSED(args);
}

MetEireannIon::~MetEireannIon()
{
    cancelLocationRequest();
    cancelForecastRequest();
}

KIO::TransferJob *MetEireannIon::requestApiJob(const QUrl &url, bool needsAjaxHeaders)
{
    auto *getJob = KIO::get(url, KIO::Reload, KIO::HideProgressInfo);
    getJob->addMetaData(u"cookies"_s, u"none"_s);

    if (needsAjaxHeaders) {
        getJob->addMetaData(u"customHTTPHeader"_s, u"Accept: */*\r\nReferer: https://www.met.ie/\r\nX-Requested-With: XMLHttpRequest\r\n"_s);
    }

    qCDebug(WEATHER::ION::METEIREANN) << "Requesting URL:" << url;

    return getJob;
}

void MetEireannIon::cancelLocationRequest()
{
    if (!m_activeLocationJob) {
        return;
    }

    const auto request = m_locationRequests.take(m_activeLocationJob);
    disconnect(m_activeLocationJob, nullptr, this, nullptr);

    if (request && request->promise) {
        request->promise->finish();
    }

    m_activeLocationJob->kill(KJob::Quietly);
    m_activeLocationJob = nullptr;
}

void MetEireannIon::cancelForecastRequest()
{
    if (!m_activeForecastJob) {
        return;
    }

    const auto request = m_forecastRequests.take(m_activeForecastJob);
    disconnect(m_activeForecastJob, nullptr, this, nullptr);

    if (request && request->promise) {
        request->promise->finish();
    }

    m_activeForecastJob->kill(KJob::Quietly);
    m_activeForecastJob = nullptr;
}

QString MetEireannIon::buildPlaceInfo(const LocationInfo &location) const
{
    const QJsonObject object{
        {u"name"_s, location.name},
        {u"county"_s, location.county},
        {u"province"_s, location.province},
        {u"slug"_s, location.slug},
        {u"lat"_s, location.latitude},
        {u"lon"_s, location.longitude},
    };

    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

MetEireannIon::LocationInfo MetEireannIon::parsePlaceInfo(const QString &placeInfo) const
{
    const auto document = QJsonDocument::fromJson(placeInfo.toUtf8());
    if (!document.isObject()) {
        return {};
    }

    const auto object = document.object();

    LocationInfo location;
    location.name = object.value(u"name"_s).toString();
    location.county = object.value(u"county"_s).toString();
    location.province = object.value(u"province"_s).toString();
    location.slug = object.value(u"slug"_s).toString();
    location.latitude = object.value(u"lat"_s).toDouble(qQNaN());
    location.longitude = object.value(u"lon"_s).toDouble(qQNaN());
    return location;
}

void MetEireannIon::findPlaces(std::shared_ptr<QPromise<std::shared_ptr<Locations>>> promise, const QString &searchString)
{
    cancelLocationRequest();

    auto request = std::make_shared<LocationRequestState>();
    request->promise = promise;
    request->promise->start();

    if (request->promise->isCanceled()) {
        request->promise->finish();
        return;
    }

    request->searchString = searchString.trimmed();

    const auto encodedSearch = QString::fromUtf8(QUrl::toPercentEncoding(request->searchString));
    QUrl url(QString::fromLatin1(LOCATION_API_BASE) + encodedSearch);

    QUrlQuery query;
    query.addQueryItem(u"term"_s, request->searchString);
    query.addQueryItem(u"_type"_s, u"query"_s);
    query.addQueryItem(u"q"_s, request->searchString);
    url.setQuery(query);

    auto *job = requestApiJob(url, true);
    m_activeLocationJob = job;
    m_locationRequests.insert(job, request);
    connect(job, &KIO::TransferJob::data, this, [request](KIO::Job *, const QByteArray &data) {
        if (!data.isEmpty()) {
            request->data.append(data);
        }
    });
    connect(job, &KJob::result, this, &MetEireannIon::locationsJobFinished);
}

void MetEireannIon::locationsJobFinished(KJob *job)
{
    const auto request = m_locationRequests.take(job);
    if (!request) {
        return;
    }

    if (job == m_activeLocationJob) {
        m_activeLocationJob = nullptr;
    }

    auto locations = std::make_shared<Locations>();

    if (job->error()) {
        qCWarning(WEATHER::ION::METEIREANN) << "Location search failed:" << job->errorText();
        locations->setError();
    } else {
        parseLocationsData(request->data);
        const auto document = QJsonDocument::fromJson(request->data);
        if (!document.isArray()) {
            qCWarning(WEATHER::ION::METEIREANN) << "Unexpected location search payload";
            locations->setError();
        } else {
            const auto results = document.array();
            for (const auto &value : results) {
                if (!value.isObject()) {
                    continue;
                }

                const auto object = value.toObject();
                LocationInfo info;
                const QString detailedName = object.value(u"name"_s).toString();
                info.name = !detailedName.isEmpty() ? detailedName : object.value(u"LocationName"_s).toString();
                info.county = object.value(u"County"_s).toString();
                info.province = object.value(u"province"_s).toString();
                info.slug = object.value(u"slug"_s).toString();
                info.latitude = object.value(u"lat"_s).toDouble(qQNaN());
                info.longitude = object.value(u"lon"_s).toDouble(qQNaN());

                if (info.name.isEmpty() || qIsNaN(info.latitude) || qIsNaN(info.longitude)) {
                    continue;
                }

                Location location;
                location.setDisplayName(info.name);
                location.setStation(!info.county.isEmpty() ? info.county : (!info.province.isEmpty() ? info.province : i18n("Ireland")));
                location.setCode(info.slug);
                location.setCoordinates(QPointF(info.latitude, info.longitude));
                location.setPlaceInfo(buildPlaceInfo(info));
                locations->addLocation(location);
            }
        }
    }

    if (request->promise && !request->promise->isCanceled()) {
        request->promise->addResult(locations);
    }
    if (request->promise) {
        request->promise->finish();
    }
}

void MetEireannIon::parseLocationsData(const QByteArray &data)
{
    qCDebug(WEATHER::ION::METEIREANN) << "Received location payload with" << data.size() << "bytes";
}

void MetEireannIon::fetchForecast(std::shared_ptr<QPromise<std::shared_ptr<Forecast>>> promise, const QString &placeInfo)
{
    cancelForecastRequest();

    auto request = std::make_shared<ForecastRequestState>();
    request->promise = promise;
    request->promise->start();

    if (request->promise->isCanceled()) {
        request->promise->finish();
        return;
    }

    request->locationInfo = parsePlaceInfo(placeInfo);
    if (request->locationInfo.name.isEmpty() || qIsNaN(request->locationInfo.latitude) || qIsNaN(request->locationInfo.longitude)) {
        qCWarning(WEATHER::ION::METEIREANN) << "Invalid placeInfo:" << placeInfo;
        request->promise->finish();
        return;
    }

    const QString urlString =
        QString::fromLatin1(FORECAST_API_BASE) + u"?lat=%1;long=%2"_s.arg(QString::number(request->locationInfo.latitude, 'f', 5),
                                                                          QString::number(request->locationInfo.longitude, 'f', 5));

    auto *job = requestApiJob(QUrl(urlString));
    m_activeForecastJob = job;
    m_forecastRequests.insert(job, request);
    connect(job, &KIO::TransferJob::data, this, [request](KIO::Job *, const QByteArray &data) {
        if (!data.isEmpty()) {
            request->data.append(data);
        }
    });
    connect(job, &KJob::result, this, &MetEireannIon::forecastJobFinished);
}

void MetEireannIon::forecastJobFinished(KJob *job)
{
    const auto request = m_forecastRequests.take(job);
    if (!request) {
        return;
    }

    if (job == m_activeForecastJob) {
        m_activeForecastJob = nullptr;
    }

    if (job->error()) {
        qCWarning(WEATHER::ION::METEIREANN) << "Forecast request failed:" << job->errorText();
        if (request->promise) {
            auto errorForecast = std::make_shared<Forecast>();
            errorForecast->setError();
            request->promise->addResult(errorForecast);
            request->promise->finish();
        }
        return;
    }

    if (!parseForecastData(request->data, request->instantForecasts, request->intervalForecasts)) {
        qCWarning(WEATHER::ION::METEIREANN) << "Failed to parse forecast payload";
        if (request->promise) {
            auto errorForecast = std::make_shared<Forecast>();
            errorForecast->setError();
            request->promise->addResult(errorForecast);
            request->promise->finish();
        }
        return;
    }

    updateWeather(*request);
}

bool MetEireannIon::parseForecastData(const QByteArray &data, QList<InstantForecast> &instantForecasts, QList<IntervalForecast> &intervalForecasts)
{
    QXmlStreamReader xml(data);

    while (!xml.atEnd()) {
        xml.readNext();

        if (!xml.isStartElement()) {
            continue;
        }

        if (xml.name() != u"time"_s) {
            continue;
        }

        const auto attributes = xml.attributes();
        const QDateTime from = QDateTime::fromString(attributes.value(u"from"_s).toString(), Qt::ISODate);
        const QDateTime to = QDateTime::fromString(attributes.value(u"to"_s).toString(), Qt::ISODate);
        const bool isInstant = from == to;

        InstantForecast instant;
        instant.time = from;

        IntervalForecast interval;
        interval.from = from;
        interval.to = to;

        while (xml.readNextStartElement()) {
            if (xml.name() != u"location"_s) {
                xml.skipCurrentElement();
                continue;
            }

            const auto locationAttributes = xml.attributes();
            if (isInstant) {
                instant.latitude = parseDouble(locationAttributes.value(u"latitude"_s));
                instant.longitude = parseDouble(locationAttributes.value(u"longitude"_s));
            }

            while (xml.readNextStartElement()) {
                const auto name = xml.name();
                const auto childAttributes = xml.attributes();

                if (isInstant) {
                    if (name == u"temperature"_s) {
                        instant.temperature = parseDouble(childAttributes.value(u"value"_s));
                    } else if (name == u"windDirection"_s) {
                        instant.windDirectionDegrees = parseDouble(childAttributes.value(u"deg"_s));
                        instant.windDirectionName = childAttributes.value(u"name"_s).toString();
                    } else if (name == u"windSpeed"_s) {
                        instant.windSpeed = parseDouble(childAttributes.value(u"mps"_s));
                    } else if (name == u"windGust"_s) {
                        instant.windGust = parseDouble(childAttributes.value(u"mps"_s));
                    } else if (name == u"humidity"_s) {
                        instant.humidity = parseDouble(childAttributes.value(u"value"_s));
                    } else if (name == u"pressure"_s) {
                        instant.pressure = parseDouble(childAttributes.value(u"value"_s));
                    } else if (name == u"dewpointTemperature"_s) {
                        instant.dewpoint = parseDouble(childAttributes.value(u"value"_s));
                    }
                } else {
                    if (name == u"precipitation"_s) {
                        interval.precipitation = parseDouble(childAttributes.value(u"value"_s));
                        interval.probability = parseDouble(childAttributes.value(u"probability"_s));
                    } else if (name == u"symbol"_s) {
                        interval.symbolId = childAttributes.value(u"id"_s).toString();
                        interval.symbolNumber = parseInt(childAttributes.value(u"number"_s));
                    }
                }

                xml.skipCurrentElement();
            }
        }

        if (isInstant) {
            if (instant.time.isValid()) {
                instantForecasts.append(instant);
            }
        } else if (interval.from.isValid() && interval.to.isValid() && !interval.symbolId.isEmpty()) {
            intervalForecasts.append(interval);
        }
    }

    if (xml.hasError()) {
        qCWarning(WEATHER::ION::METEIREANN) << "XML parse error:" << xml.errorString();
        return false;
    }

    qCDebug(WEATHER::ION::METEIREANN) << "Parsed" << instantForecasts.size() << "instant points and" << intervalForecasts.size() << "interval points";
    return !instantForecasts.isEmpty() && !intervalForecasts.isEmpty();
}

const MetEireannIon::InstantForecast *MetEireannIon::currentObservation(const QList<InstantForecast> &instantForecasts) const
{
    if (instantForecasts.isEmpty()) {
        return nullptr;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const InstantForecast *latestPast = nullptr;
    const InstantForecast *nearestFuture = nullptr;
    qint64 latestPastTime = std::numeric_limits<qint64>::min();
    qint64 nearestFutureDistance = std::numeric_limits<qint64>::max();

    for (const auto &forecast : instantForecasts) {
        const qint64 delta = forecast.time.msecsTo(now);
        if (delta >= 0) {
            const qint64 forecastTime = forecast.time.toMSecsSinceEpoch();
            if (!latestPast || forecastTime > latestPastTime) {
                latestPast = &forecast;
                latestPastTime = forecastTime;
            }
        } else {
            const qint64 futureDistance = -delta;
            if (!nearestFuture || futureDistance < nearestFutureDistance) {
                nearestFuture = &forecast;
                nearestFutureDistance = futureDistance;
            }
        }
    }

    return latestPast ? latestPast : nearestFuture;
}

const MetEireannIon::IntervalForecast *MetEireannIon::matchingInterval(const QList<IntervalForecast> &intervalForecasts, const QDateTime &time) const
{
    const IntervalForecast *best = nullptr;
    qint64 bestDistance = std::numeric_limits<qint64>::max();

    for (const auto &interval : intervalForecasts) {
        if (interval.from <= time && interval.to >= time) {
            return &interval;
        }

        const QDateTime midpoint = interval.from.addMSecs(interval.from.msecsTo(interval.to) / 2);
        const qint64 distance = std::llabs(time.msecsTo(midpoint));
        if (distance < bestDistance) {
            best = &interval;
            bestDistance = distance;
        }
    }

    return best;
}

QDate MetEireannIon::dayPartDate(const QDateTime &dateTime, bool *isNight) const
{
    const QDateTime localDateTime = dateTime.toTimeZone(irelandTimeZone());
    QDate date = localDateTime.date();
    const int hour = localDateTime.time().hour();

    bool night = false;
    if (hour < 6) {
        date = date.addDays(-1);
        night = true;
    } else if (hour >= 18) {
        night = true;
    }

    if (isNight) {
        *isNight = night;
    }

    return date;
}

QDateTime MetEireannIon::dayPartTarget(const QDate &date, bool isNight) const
{
    return QDateTime(date, isNight ? QTime(22, 0) : QTime(13, 0), irelandTimeZone()).toUTC();
}

MetEireannIon::DayPartSummary MetEireannIon::summarizeDayPart(const QList<InstantForecast> &instantForecasts,
                                                             const QList<IntervalForecast> &intervalForecasts,
                                                             const QDate &date,
                                                             bool isNight) const
{
    DayPartSummary summary;
    const QDateTime target = dayPartTarget(date, isNight);
    qint64 bestDistance = std::numeric_limits<qint64>::max();

    for (const auto &interval : intervalForecasts) {
        bool intervalIsNight = false;
        const QDate intervalDate = dayPartDate(interval.from.addMSecs(interval.from.msecsTo(interval.to) / 2), &intervalIsNight);
        if (intervalDate != date || intervalIsNight != isNight) {
            continue;
        }

        const QDateTime midpoint = interval.from.addMSecs(interval.from.msecsTo(interval.to) / 2);
        const qint64 distance = std::llabs(target.msecsTo(midpoint));
        if (distance < bestDistance) {
            summary.interval = &interval;
            bestDistance = distance;
        }

        if (!qIsNaN(interval.probability)) {
            summary.probability = qIsNaN(summary.probability) ? interval.probability : std::max(summary.probability, interval.probability);
        }
    }

    for (const auto &instant : instantForecasts) {
        bool instantIsNight = false;
        const QDate instantDate = dayPartDate(instant.time, &instantIsNight);
        if (instantDate != date || instantIsNight != isNight || qIsNaN(instant.temperature)) {
            continue;
        }

        summary.highTemp = qIsNaN(summary.highTemp) ? instant.temperature : std::max(summary.highTemp, instant.temperature);
        summary.lowTemp = qIsNaN(summary.lowTemp) ? instant.temperature : std::min(summary.lowTemp, instant.temperature);
    }

    return summary;
}

QString MetEireannIon::conditionText(const QString &symbolId) const
{
    const QString key = symbolId.toLower();

    if (key == u"sun"_s) {
        return i18nc("weather condition", "Clear");
    }
    if (key == u"lightcloud"_s) {
        return i18nc("weather condition", "Mostly clear");
    }
    if (key == u"partlycloud"_s) {
        return i18nc("weather condition", "Partly cloudy");
    }
    if (key == u"cloud"_s) {
        return i18nc("weather condition", "Overcast");
    }
    if (key.contains(u"thunder"_s)) {
        return i18nc("weather condition", "Thunderstorm");
    }
    if (key.contains(u"snow"_s)) {
        if (key.contains(u"sleet"_s) || key.contains(u"rain"_s)) {
            return i18nc("weather condition", "Rain and snow");
        }
        return key.contains(u"light"_s) ? i18nc("weather condition", "Light snow") : i18nc("weather condition", "Snow");
    }
    if (key.contains(u"sleet"_s)) {
        return i18nc("weather condition", "Sleet");
    }
    if (key.contains(u"hail"_s)) {
        return i18nc("weather condition", "Hail");
    }
    if (key.contains(u"drizzle"_s)) {
        return i18nc("weather condition", "Drizzle");
    }
    if (key.contains(u"rain"_s)) {
        return key.contains(u"light"_s) ? i18nc("weather condition", "Light rain") : i18nc("weather condition", "Rain");
    }
    if (key.contains(u"fog"_s)) {
        return i18nc("weather condition", "Fog");
    }

    return symbolId;
}

Ion::ConditionIcons MetEireannIon::conditionIcon(const QString &symbolId, bool isNight) const
{
    const QString key = symbolId.toLower();

    if (key == u"sun"_s) {
        return isNight ? ClearNight : ClearDay;
    }
    if (key == u"lightcloud"_s) {
        return isNight ? FewCloudsNight : FewCloudsDay;
    }
    if (key == u"partlycloud"_s) {
        return isNight ? PartlyCloudyNight : PartlyCloudyDay;
    }
    if (key == u"cloud"_s) {
        return Overcast;
    }
    if (key.contains(u"thunder"_s)) {
        if (key.contains(u"sun"_s)) {
            return isNight ? ChanceThunderstormNight : ChanceThunderstormDay;
        }
        return Thunderstorm;
    }
    if (key.contains(u"snow"_s)) {
        if (key.contains(u"sleet"_s) || key.contains(u"rain"_s)) {
            return RainSnow;
        }
        if (key.contains(u"sun"_s)) {
            return isNight ? ChanceSnowNight : ChanceSnowDay;
        }
        return key.contains(u"light"_s) ? LightSnow : Snow;
    }
    if (key.contains(u"sleet"_s)) {
        return RainSnow;
    }
    if (key.contains(u"hail"_s)) {
        return Hail;
    }
    if (key.contains(u"drizzle"_s) || key.contains(u"rain"_s)) {
        if (key.contains(u"sun"_s)) {
            return isNight ? ChanceShowersNight : ChanceShowersDay;
        }
        return key.contains(u"light"_s) || key.contains(u"drizzle"_s) ? LightRain : Rain;
    }
    if (key.contains(u"fog"_s)) {
        return Mist;
    }

    return NotAvailable;
}

QString MetEireannIon::weatherIconName(const QString &symbolId, bool isNight) const
{
    return getWeatherIcon(conditionIcon(symbolId, isNight));
}

QString MetEireannIon::windDirectionFromDegrees(double degrees) const
{
    if (qIsNaN(degrees)) {
        return {};
    }

    static constexpr std::array<const char *, 16> directions = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };

    const int index = static_cast<int>(std::round(degrees / 22.5)) % directions.size();
    return QString::fromLatin1(directions[index]);
}

void MetEireannIon::updateWeather(const ForecastRequestState &request)
{
    auto forecast = std::make_shared<Forecast>();

    Station station;
    station.setPlace(request.locationInfo.name);
    station.setStation(!request.locationInfo.slug.isEmpty() ? request.locationInfo.slug : request.locationInfo.name.toLower());
    if (!request.locationInfo.county.isEmpty()) {
        station.setRegion(request.locationInfo.county);
    }
    station.setCountry(i18n("Ireland"));
    station.setCoordinates(request.locationInfo.latitude, request.locationInfo.longitude);
    forecast->setStation(station);

    MetaData metaData;
    metaData.setCredit(i18nc("credit line, keep string short", "Copyright Met Éireann"));
    metaData.setCreditURL(u"https://www.met.ie/about-us/specialised-services/open-data"_s);
    metaData.setTemperatureUnit(Celsius);
    metaData.setWindSpeedUnit(MeterPerSecond);
    metaData.setHumidityUnit(Percent);
    metaData.setPressureUnit(Hectopascal);
    metaData.setPrecipUnit(Millimeter);
    forecast->setMetadata(metaData);

    const InstantForecast *observation = currentObservation(request.instantForecasts);
    if (observation) {
        LastObservation lastObservation;
        lastObservation.setObservationTimestamp(observation->time);

        const IntervalForecast *interval = matchingInterval(request.intervalForecasts, observation->time);
        if (interval) {
            const bool night = isNightTime(observation->time, request.locationInfo.latitude, request.locationInfo.longitude);
            lastObservation.setCurrentConditions(conditionText(interval->symbolId));
            lastObservation.setConditionIcon(weatherIconName(interval->symbolId, night));
        }

        if (!qIsNaN(observation->temperature)) {
            lastObservation.setTemperature(observation->temperature);
        }
        if (!qIsNaN(observation->humidity)) {
            lastObservation.setHumidity(observation->humidity);
        }
        if (!qIsNaN(observation->pressure)) {
            lastObservation.setPressure(observation->pressure);
        }
        if (!qIsNaN(observation->dewpoint)) {
            lastObservation.setDewpoint(observation->dewpoint);
        }
        if (!qIsNaN(observation->windSpeed)) {
            lastObservation.setWindSpeed(observation->windSpeed);
        }
        if (!qIsNaN(observation->windGust)) {
            lastObservation.setWindGust(observation->windGust);
        }

        const QString windDirection = !observation->windDirectionName.isEmpty() ? observation->windDirectionName : windDirectionFromDegrees(observation->windDirectionDegrees);
        if (!windDirection.isEmpty()) {
            lastObservation.setWindDirection(windDirection);
        }

        forecast->setLastObservation(lastObservation);
    }

    auto futureDays = std::make_shared<FutureDays>();
    const QDate today = QDateTime::currentDateTimeUtc().toTimeZone(irelandTimeZone()).date();
    QList<QDate> orderedDates;

    for (const auto &interval : request.intervalForecasts) {
        bool isNight = false;
        const QDate intervalDate = dayPartDate(interval.from.addMSecs(interval.from.msecsTo(interval.to) / 2), &isNight);
        if (intervalDate < today || orderedDates.contains(intervalDate)) {
            continue;
        }

        orderedDates.append(intervalDate);
        if (orderedDates.size() == MAX_FORECAST_DAYS) {
            break;
        }
    }

    for (const auto &date : orderedDates) {
        FutureDayForecast futureDay;
        futureDay.setMonthDay(date.day());
        futureDay.setWeekDay(QLocale().standaloneDayName(date.dayOfWeek(), QLocale::ShortFormat));

        const auto daySummary = summarizeDayPart(request.instantForecasts, request.intervalForecasts, date, false);
        if (daySummary.interval || !qIsNaN(daySummary.highTemp) || !qIsNaN(daySummary.lowTemp)) {
            FutureForecast dayForecast;
            if (daySummary.interval) {
                dayForecast.setCondition(conditionText(daySummary.interval->symbolId));
                dayForecast.setConditionIcon(weatherIconName(daySummary.interval->symbolId, false));
            }
            if (!qIsNaN(daySummary.highTemp)) {
                dayForecast.setHighTemp(daySummary.highTemp);
            }
            if (!qIsNaN(daySummary.lowTemp)) {
                dayForecast.setLowTemp(daySummary.lowTemp);
            }
            if (!qIsNaN(daySummary.probability)) {
                dayForecast.setConditionProbability(std::round(daySummary.probability));
            }
            futureDay.setDaytime(dayForecast);
        }

        const auto nightSummary = summarizeDayPart(request.instantForecasts, request.intervalForecasts, date, true);
        if (nightSummary.interval || !qIsNaN(nightSummary.highTemp) || !qIsNaN(nightSummary.lowTemp)) {
            FutureForecast nightForecast;
            if (nightSummary.interval) {
                nightForecast.setCondition(conditionText(nightSummary.interval->symbolId));
                nightForecast.setConditionIcon(weatherIconName(nightSummary.interval->symbolId, true));
            }
            if (!qIsNaN(nightSummary.highTemp)) {
                nightForecast.setHighTemp(nightSummary.highTemp);
            }
            if (!qIsNaN(nightSummary.lowTemp)) {
                nightForecast.setLowTemp(nightSummary.lowTemp);
            }
            if (!qIsNaN(nightSummary.probability)) {
                nightForecast.setConditionProbability(std::round(nightSummary.probability));
            }
            futureDay.setNight(nightForecast);
        }

        if (futureDay.daytime().has_value() || futureDay.night().has_value()) {
            futureDays->addDay(futureDay);
        }
    }

    forecast->setFutureDays(futureDays);

    qCDebug(WEATHER::ION::METEIREANN) << "Forecast received for" << request.locationInfo.name;

    if (request.promise && !request.promise->isCanceled()) {
        request.promise->addResult(forecast);
    }
    if (request.promise) {
        request.promise->finish();
    }
}

#include "ion_meteireann.moc"
