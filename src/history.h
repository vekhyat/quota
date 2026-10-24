#pragma once

#include "usage.h"

#include <functional>

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// Transcript scan follows T3 Code (MIT, T3 Tools Inc.).
// The readers are claudeUsage.ts and codexUsage.ts in that tree.

struct HistoryScan {
    QVector<UsageRec> records;
    QStringList directories;
    qint64 bytesRead = 0;
    // A file grew during the read. Scan again.
    bool rescan = false;
};

// claudeDir is the Claude home. codexHome is the Codex home. cacheDir receives scan-cache.json.
// nowMs is the clock for the 90 day file cutoff. Zero means the current time.
HistoryScan scanHistory(const QString &claudeDir, const QString &codexHome, const QString &cacheDir, qint64 nowMs = 0);

struct RateLoad {
    QHash<QString, ModelRate> rates;
    // download, cache, bundled, or empty when every source failed.
    QString origin;
    // True when the disk copy is missing or older than one day.
    bool refresh = false;
};

// fetch returns the LiteLLM document, or empty when the download fails.
// An empty fetch skips the network and uses the disk cache, then the bundled file.
RateLoad loadModelRates(const QString &cacheDir, const QString &bundledPath, qint64 nowMs = 0,
                        const std::function<QByteArray()> &fetch = {});

QHash<QString, ModelRate> readRateFile(const QString &path);
bool storeRateDocument(const QString &cacheDir, const QJsonObject &document, qint64 nowMs);

// True when the T3 cache file exists and its version is one this app can read.
bool readT3UsageCache(const QString &path, QVector<UsageRec> *records, qint64 *modifiedMs);
