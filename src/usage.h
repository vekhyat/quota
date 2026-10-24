#pragma once

#include <QDateTime>
#include <QTimeZone>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>

// Cost and limit rules follow T3 Code (MIT, T3 Tools Inc.).

struct TokenRate {
    double input = 0;
    double output = 0;
    double cacheRead = 0;
    double cacheWrite = 0;
};

struct ModelRate {
    TokenRate standard;
    bool hasFast = false;
    TokenRate fast;
    bool hasUltra = false;
    TokenRate ultra;
};

struct UsageRec {
    qint64 timestampMs = 0;
    QString provider;
    QString model;
    QString dedupeKey;
    double uncached = 0;
    double cached = 0;
    double cacheWrite = 0;
    double output = 0;
    double reportedCost = -1;
    int speed = 0;
    QString rateModel;
};

struct UsageWindow {
    bool rolling24h = false;
    qint64 sinceMs = 0;
    qint64 untilMs = 0;
    QString sinceDay;
    QString untilDay;
};

struct UsageSlice {
    double tokens = 0;
    double cost = 0;
    int records = 0;
    int unpriced = 0;
};

struct ModelSlice {
    QString provider;
    QString model;
    double tokens = 0;
    double cost = 0;
};

struct UsageSummary {
    QHash<QString, UsageSlice> byProvider;
    QVector<ModelSlice> models;
    QHash<QString, UsageSlice> byDay;
    QHash<QString, QHash<QString, UsageSlice>> byDayProvider;
    // Only filled for the rolling 24 hours. Hour 0 starts at the window start.
    QHash<int, QHash<QString, UsageSlice>> byHourProvider;
    UsageSlice total;
};

struct LimitWindow {
    QString provider;
    QString kind;
    QString extra;
    double usedPercent = 0;
    QString resetsAt;
};

QHash<QString, ModelRate> parseRateTable(const QJsonObject &document);
QVector<UsageRec> parseUsageCache(const QJsonObject &root);
UsageWindow makeUsageWindow(int days, const QDateTime &now, const QTimeZone &zone);
QString localDay(qint64 timestampMs, const QTimeZone &zone);
UsageSummary summarizeUsage(const QVector<UsageRec> &records, const QHash<QString, ModelRate> &rates,
                            const UsageWindow &window, const QTimeZone &zone);

QVector<LimitWindow> parseClaudeLimits(const QJsonObject &body);
QVector<LimitWindow> parseCodexLimits(const QJsonObject &body);
QVector<LimitWindow> parseCodexWham(const QJsonObject &body);
QVector<LimitWindow> parseGrokLimits(const QJsonObject &body);

QString parseClaudeVersion(const QByteArray &output);

double clampPercent(double value);
