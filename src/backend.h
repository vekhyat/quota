#pragma once

#include "usage.h"

#include <QFileSystemWatcher>
#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString metric READ metric NOTIFY viewChanged)
    Q_PROPERTY(int windowDays READ windowDays NOTIFY viewChanged)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY dataChanged)
    Q_PROPERTY(QVariantList models READ models NOTIFY dataChanged)
    Q_PROPERTY(QVariantList buckets READ buckets NOTIFY dataChanged)
    Q_PROPERTY(QVariantList limits READ limits NOTIFY dataChanged)
    Q_PROPERTY(double totalCost READ totalCost NOTIFY dataChanged)
    Q_PROPERTY(double totalTokens READ totalTokens NOTIFY dataChanged)
    Q_PROPERTY(QStringList notices READ notices NOTIFY dataChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY dataChanged)
    Q_PROPERTY(qint64 cacheAt READ cacheAt NOTIFY dataChanged)
    Q_PROPERTY(qint64 limitsAt READ limitsAt NOTIFY dataChanged)
    Q_PROPERTY(bool preview READ preview CONSTANT)

public:
    explicit Backend(bool preview = false, QObject *parent = nullptr);

    QString metric() const { return m_metric; }
    int windowDays() const { return m_windowDays; }
    QVariantList rows() const { return m_rows; }
    QVariantList models() const { return m_models; }
    QVariantList buckets() const { return m_buckets; }
    QVariantList limits() const { return m_limits; }
    double totalCost() const { return m_totalCost; }
    double totalTokens() const { return m_totalTokens; }
    QStringList notices() const { return m_notices; }
    bool busy() const { return m_busy; }
    qint64 cacheAt() const { return m_cacheAt; }
    qint64 limitsAt() const { return m_limitsAt; }
    bool preview() const { return m_preview; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setMetric(const QString &metric);
    Q_INVOKABLE void setWindowDays(int days);
    Q_INVOKABLE void primaryAction();

signals:
    void viewChanged();
    void dataChanged();

private:
    void loadCache();
    void rebuild();
    void publish();
    void readClaude(int generation);
    void requestClaude(const QString &token, int generation);
    void readGrok(int generation);
    void readCodex(int generation);
    void readCodexAppServer(int generation);
    void finishProbe(int generation);
    void note(const QString &code, bool on);
    QString readTokenFile(const QString &path, const QString &objectKey, const QString &field) const;

    bool m_preview = false;
    QString m_metric = "cost";
    int m_windowDays = 7;
    QVector<UsageRec> m_records;
    QHash<QString, ModelRate> m_rates;
    QVector<LimitWindow> m_limitRows;
    QVariantList m_rows;
    QVariantList m_models;
    QVariantList m_buckets;
    QVariantList m_limits;
    double m_totalCost = 0;
    double m_totalTokens = 0;
    QStringList m_notices;
    bool m_busy = false;
    qint64 m_cacheAt = 0;
    qint64 m_limitsAt = 0;
    int m_probes = 0;
    int m_generation = 0;
    bool m_hasRates = false;
    QString m_t3Home;
    QString m_claudeDir;
    QString m_grokHome;
    QString m_codexBin;
    QString m_claudeBin;
    QString m_claudeVersion;
    QNetworkAccessManager m_network;
    QProcess *m_codex = nullptr;
    QByteArray m_codexBuf;
    int m_codexStep = 0;
    QFileSystemWatcher m_watcher;
    QTimer m_cacheDebounce;
    QTimer m_limitTimer;
};
