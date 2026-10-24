#include "backend.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimeZone>

namespace {
const QStringList kProviderOrder{"claude", "codex", "grok", "cursor", "opencode", "antigravity", "pi", "muse"};
// Used when the installed Claude Code does not answer --version.
const QString kFallbackClaudeVersion = QStringLiteral("2.1.310");

QString homePath(const char *env, const QString &fallback)
{
    const QString value = qEnvironmentVariable(env);
    return value.isEmpty() ? fallback : value;
}
}

Backend::Backend(bool preview, QObject *parent) : QObject(parent), m_preview(preview)
{
    m_t3Home = homePath("QUOTA_T3_HOME", QDir::homePath() + "/.t3");
    m_claudeDir = homePath("QUOTA_CLAUDE_DIR", QDir::homePath() + "/.claude");
    m_grokHome = homePath("QUOTA_GROK_HOME", QDir::homePath() + "/.grok");
    m_claudeVersion = qEnvironmentVariable("QUOTA_CLAUDE_VERSION");
    m_claudeBin = homePath("QUOTA_CLAUDE_BIN", QStandardPaths::findExecutable("claude"));
    if (m_claudeBin.isEmpty())
        m_claudeBin = "claude";
    m_codexBin = homePath("QUOTA_CODEX_BIN", QStandardPaths::findExecutable("codex"));
    if (m_codexBin.isEmpty())
        m_codexBin = "codex";

    m_cacheDebounce.setSingleShot(true);
    m_cacheDebounce.setInterval(200);
    connect(&m_cacheDebounce, &QTimer::timeout, this, [this] {
        loadCache();
        rebuild();
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_cacheDebounce.start(); });
    m_limitTimer.setInterval(5 * 60 * 1000);
    connect(&m_limitTimer, &QTimer::timeout, this, &Backend::refresh);
    if (!m_preview)
        m_limitTimer.start();
    refresh();
}

void Backend::primaryAction()
{
    refresh();
}

void Backend::setMetric(const QString &metric)
{
    if (metric != "cost" && metric != "tokens")
        return;
    if (metric == m_metric)
        return;
    m_metric = metric;
    rebuild();
    emit viewChanged();
}

void Backend::setWindowDays(int days)
{
    if (days != 1 && days != 7 && days != 30 && days != 90)
        return;
    if (days == m_windowDays)
        return;
    m_windowDays = days;
    rebuild();
    emit viewChanged();
}

void Backend::note(const QString &code, bool on)
{
    const bool has = m_notices.contains(code);
    if (on && !has)
        m_notices.append(code);
    if (!on && has)
        m_notices.removeAll(code);
}

void Backend::refresh()
{
    if (m_preview) {
        m_records = {{1, "claude", "claude-opus", {}, 1000, 0, 0, 100, -1, 0, {}},
                     {1, "codex", "gpt-5", {}, 400, 0, 0, 80, -1, 0, {}}};
        m_rates.clear();
        m_hasRates = false;
        // Resets sit relative to now, so the countdown and pace read the same in every capture.
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const auto at = [&](qint64 minutes) { return now.addSecs(minutes * 60).toString(Qt::ISODate); };
        m_limitRows = {{"claude", "session", {}, 42, at(100)},
                       {"claude", "weekly", {}, 18, at(3 * 24 * 60)},
                       {"claude", "weekly", "Opus", 64, at(3 * 24 * 60)},
                       {"codex", "session", {}, 81, at(180)},
                       {"codex", "weekly", {}, 11, at(5 * 24 * 60)}};
        m_cacheAt = now.addSecs(-14 * 60).toMSecsSinceEpoch();
        m_limitsAt = now.addSecs(-2 * 60).toMSecsSinceEpoch();
        m_notices.clear();
        m_busy = false;
        rebuild();
        return;
    }
    loadCache();
    rebuild();
    const int generation = ++m_generation;
    m_busy = true;
    m_probes = 3;
    note("limits-busy", true);
    emit dataChanged();
    readClaude(generation);
    readGrok(generation);
    readCodex(generation);
}

void Backend::loadCache()
{
    const QString cachePath = m_t3Home + "/userdata/usage-scan-cache-v9.json";
    const QString ratesPath = m_t3Home + "/userdata/usage-model-rates.json";
    m_records.clear();
    m_rates.clear();
    m_hasRates = false;
    m_cacheAt = 0;
    QFile cache(cachePath);
    if (!cache.open(QIODevice::ReadOnly)) {
        note("no-cache", true);
    } else {
        note("no-cache", false);
        m_cacheAt = QFileInfo(cache).lastModified().toMSecsSinceEpoch();
        m_records = parseUsageCache(QJsonDocument::fromJson(cache.readAll()).object());
        if (!m_watcher.files().contains(cachePath))
            m_watcher.addPath(cachePath);
    }
    QFile rates(ratesPath);
    if (rates.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(rates.readAll()).object();
        const QJsonValue document = root.value("document");
        m_rates = parseRateTable(document.isObject() ? document.toObject() : root);
        m_hasRates = !m_rates.isEmpty();
    }
    note("no-rates", !m_hasRates);
}

void Backend::rebuild()
{
    const QTimeZone zone = QTimeZone::systemTimeZone();
    const UsageWindow window = makeUsageWindow(m_windowDays, QDateTime::currentDateTime(), zone);
    UsageSummary summary = summarizeUsage(m_records, m_rates, window, zone);
    if (m_preview) {
        summary.total = {1580, 24.5, 2, 0};
        summary.byProvider.insert("claude", {1100, 18.4, 1, 0});
        summary.byProvider.insert("codex", {480, 6.1, 1, 0});
        const QDate until = QDate::fromString(window.untilDay, Qt::ISODate);
        const double sampleCosts[7] = {2.1, 4.4, 1.2, 6.8, 3.3, 5.0, 1.7};
        for (int i = 0; i < 7; ++i)
            summary.byDay.insert(until.addDays(i - 6).toString(Qt::ISODate), {200.0 * (i + 1), sampleCosts[i], 1, 0});
        for (auto it = summary.byDay.cbegin(); it != summary.byDay.cend(); ++it) {
            const UsageSlice &day = it.value();
            summary.byDayProvider[it.key()].insert("claude", {day.tokens * 0.7, day.cost * 0.75, 1, 0});
            summary.byDayProvider[it.key()].insert("codex", {day.tokens * 0.3, day.cost * 0.25, 1, 0});
        }
        summary.models = {{"claude", "claude-opus-4-5", 820, 14.2},
                          {"codex", "gpt-5-codex", 480, 6.1},
                          {"claude", "claude-sonnet-4-5", 280, 4.2}};
    }
    m_totalCost = summary.total.cost;
    m_totalTokens = summary.total.tokens;
    note("empty", summary.total.records == 0 && !m_notices.contains("no-cache"));
    note("unpriced", summary.total.unpriced > 0);

    const double shareBase = m_metric == "tokens" ? summary.total.tokens : summary.total.cost;
    m_rows.clear();
    QStringList providers = kProviderOrder;
    for (auto it = summary.byProvider.cbegin(); it != summary.byProvider.cend(); ++it) {
        if (!providers.contains(it.key()))
            providers.append(it.key());
    }
    for (const QString &provider : providers) {
        if (!summary.byProvider.contains(provider))
            continue;
        const UsageSlice slice = summary.byProvider.value(provider);
        const double value = m_metric == "tokens" ? slice.tokens : slice.cost;
        m_rows.append(QVariantMap{{"provider", provider},
                                  {"cost", slice.cost},
                                  {"tokens", slice.tokens},
                                  {"share", shareBase > 0 ? value / shareBase : 0}});
    }
    m_models.clear();
    for (const ModelSlice &model : summary.models) {
        m_models.append(QVariantMap{{"provider", model.provider},
                                    {"model", model.model},
                                    {"cost", model.cost},
                                    {"tokens", model.tokens}});
    }
    // One bucket per hour for the rolling 24 hours, one per day otherwise. Each bucket
    // splits its value by provider so the chart can stack them.
    m_buckets.clear();
    const auto bucket = [&](const QHash<QString, UsageSlice> &byProvider) {
        QVariantList parts;
        double value = 0;
        for (const QString &provider : providers) {
            if (!byProvider.contains(provider))
                continue;
            const UsageSlice slice = byProvider.value(provider);
            const double part = m_metric == "tokens" ? slice.tokens : slice.cost;
            if (part <= 0)
                continue;
            value += part;
            parts.append(QVariantMap{{"provider", provider}, {"value", part}});
        }
        return QVariantMap{{"value", value}, {"parts", parts}};
    };
    if (window.rolling24h) {
        for (int hour = 0; hour < 24; ++hour) {
            QVariantMap entry = bucket(summary.byHourProvider.value(hour));
            entry.insert("start", window.sinceMs + hour * 3'600'000LL);
            m_buckets.append(entry);
        }
    } else {
        QDate day = QDate::fromString(window.sinceDay, Qt::ISODate);
        const QDate until = QDate::fromString(window.untilDay, Qt::ISODate);
        while (day.isValid() && day <= until) {
            QVariantMap entry = bucket(summary.byDayProvider.value(day.toString(Qt::ISODate)));
            entry.insert("day", day.toString(Qt::ISODate));
            m_buckets.append(entry);
            day = day.addDays(1);
        }
    }
    m_limits.clear();
    for (const LimitWindow &row : m_limitRows) {
        m_limits.append(QVariantMap{{"provider", row.provider},
                                    {"kind", row.kind},
                                    {"extra", row.extra},
                                    {"used", row.usedPercent},
                                    {"left", 100 - row.usedPercent},
                                    {"resets", row.resetsAt}});
    }
    publish();
}

void Backend::publish()
{
    emit dataChanged();
}

QString Backend::readTokenFile(const QString &path, const QString &objectKey, const QString &field) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject object = objectKey.isEmpty() ? root : root.value(objectKey).toObject();
    return object.value(field).toString().trimmed();
}

void Backend::finishProbe(int generation)
{
    if (generation != m_generation)
        return;
    m_probes -= 1;
    if (m_probes > 0)
        return;
    m_busy = false;
    m_limitsAt = QDateTime::currentMSecsSinceEpoch();
    note("limits-busy", false);
    rebuild();
}

void Backend::readClaude(int generation)
{
    QString token = readTokenFile(m_claudeDir + "/.credentials.json", "claudeAiOauth", "accessToken");
    if (token.isEmpty())
        token = readTokenFile(m_claudeDir + "/.credentials.json", "claudeAiOauth", "access_token");
    if (token.isEmpty()) {
        note("claude-login", true);
        finishProbe(generation);
        return;
    }
    note("claude-login", false);
    if (!m_claudeVersion.isEmpty()) {
        requestClaude(token, generation);
        return;
    }
    // The usage route can refuse a stale client name, so send the version of the
    // installed Claude Code. Ask it once per run and keep the answer.
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process, token, generation] {
        process->deleteLater();
        const QString found = parseClaudeVersion(process->readAllStandardOutput());
        m_claudeVersion = found.isEmpty() ? kFallbackClaudeVersion : found;
        requestClaude(token, generation);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, token, generation](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        m_claudeVersion = kFallbackClaudeVersion;
        requestClaude(token, generation);
    });
    QTimer::singleShot(5000, process, [process] { process->kill(); });
    process->start(m_claudeBin, {"--version"});
}

void Backend::requestClaude(const QString &token, int generation)
{
    if (generation != m_generation)
        return;
    QNetworkRequest request(QUrl("https://api.anthropic.com/api/oauth/usage"));
    request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    request.setRawHeader("anthropic-beta", "oauth-2025-04-20");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", ("claude-code/" + m_claudeVersion).toUtf8());
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        for (const QString &code : QStringList(m_notices)) {
            if (code.startsWith("claude-http-"))
                note(code, false);
        }
        if (status < 200 || status >= 300) {
            note(status > 0 ? "claude-http-" + QString::number(status) : "claude-failed", true);
        } else {
            note("claude-failed", false);
            QVector<LimitWindow> parsed = parseClaudeLimits(body);
            note("claude-empty", parsed.isEmpty());
            QVector<LimitWindow> next;
            for (const LimitWindow &row : m_limitRows) {
                if (row.provider != "claude")
                    next.append(row);
            }
            next += parsed;
            m_limitRows = next;
        }
        finishProbe(generation);
    });
}

void Backend::readGrok(int generation)
{
    if (!qEnvironmentVariable("XAI_API_KEY").trimmed().isEmpty()) {
        note("grok-key", true);
        finishProbe(generation);
        return;
    }
    note("grok-key", false);
    QFile file(m_grokHome + "/auth.json");
    QString token;
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QStringList keys{"https://auth.x.ai::b1a00492-073a-47ea-816f-4c329264a828",
                               "https://accounts.x.ai/sign-in"};
        for (const QString &key : keys) {
            const QJsonObject entry = root.value(key).toObject();
            if (entry.value("auth_mode").toString() == "api_key")
                continue;
            token = entry.value("key").toString().trimmed();
            if (!token.isEmpty())
                break;
        }
    }
    if (token.isEmpty()) {
        note("grok-login", true);
        finishProbe(generation);
        return;
    }
    note("grok-login", false);
    QNetworkRequest request(QUrl("https://cli-chat-proxy.grok.com/v1/billing?format=credits"));
    request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            note("grok-failed", true);
        } else {
            note("grok-failed", false);
            QVector<LimitWindow> next;
            for (const LimitWindow &row : m_limitRows)
                if (row.provider != "grok")
                    next.append(row);
            next += parseGrokLimits(QJsonDocument::fromJson(reply->readAll()).object());
            m_limitRows = next;
        }
        finishProbe(generation);
    });
}

static QString jwtAccountId(const QString &jwt)
{
    const QStringList parts = jwt.split('.');
    if (parts.size() < 2)
        return {};
    QByteArray body = parts.at(1).toUtf8();
    body.replace('-', '+').replace('_', '/');
    while (body.size() % 4)
        body.append('=');
    const QJsonObject claim = QJsonDocument::fromJson(QByteArray::fromBase64(body)).object();
    const QJsonObject auth = claim.value("https://api.openai.com/auth").toObject();
    const QString id = auth.value("chatgpt_account_id").toString().trimmed();
    return id.isEmpty() ? auth.value("chatgpt_account_user_id").toString().trimmed() : id;
}

void Backend::readCodex(int generation)
{
    const QString codexHome = homePath("QUOTA_CODEX_HOME", QDir::homePath() + "/.codex");
    QFile authFile(codexHome + "/auth.json");
    QString token;
    QString accountId;
    if (authFile.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(authFile.readAll()).object();
        const QJsonObject tokens = root.value("tokens").toObject();
        token = tokens.value("access_token").toString().trimmed();
        if (token.isEmpty())
            token = root.value("access_token").toString().trimmed();
        accountId = tokens.value("account_id").toString().trimmed();
        if (accountId.isEmpty())
            accountId = jwtAccountId(tokens.value("id_token").toString());
    }
    if (!token.isEmpty()) {
        QNetworkRequest request(QUrl("https://chatgpt.com/backend-api/wham/usage"));
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        if (!accountId.isEmpty())
            request.setRawHeader("ChatGPT-Account-Id", accountId.toUtf8());
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("User-Agent", "codex-cli");
        QNetworkReply *reply = m_network.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
            reply->deleteLater();
            if (generation != m_generation)
                return;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
            const QVector<LimitWindow> parsed = status >= 200 && status < 300 ? parseCodexWham(body) : QVector<LimitWindow>{};
            if (!parsed.isEmpty()) {
                note("codex-failed", false);
                QVector<LimitWindow> next;
                for (const LimitWindow &row : m_limitRows)
                    if (row.provider != "codex")
                        next.append(row);
                next += parsed;
                m_limitRows = next;
                finishProbe(generation);
                return;
            }
            readCodexAppServer(generation);
        });
        return;
    }
    readCodexAppServer(generation);
}

void Backend::readCodexAppServer(int generation)
{
    if (m_codex) {
        m_codex->disconnect(this);
        m_codex->kill();
        m_codex->deleteLater();
        m_codex = nullptr;
    }
    m_codexBuf.clear();
    m_codexStep = 0;
    m_codex = new QProcess(this);
    m_codex->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_codex, &QProcess::readyReadStandardOutput, this, [this, generation] {
        if (!m_codex)
            return;
        m_codexBuf += m_codex->readAllStandardOutput();
        while (true) {
            const int end = m_codexBuf.indexOf('\n');
            if (end < 0)
                return;
            const QByteArray line = m_codexBuf.left(end);
            m_codexBuf.remove(0, end + 1);
            const QJsonObject msg = QJsonDocument::fromJson(line).object();
            if (msg.value("id").toInt() == 1 && m_codexStep == 0) {
                m_codexStep = 1;
                m_codex->write("{\"method\":\"initialized\"}\n");
                m_codex->write("{\"id\":2,\"method\":\"account/rateLimits/read\",\"params\":null}\n");
            } else if (msg.value("id").toInt() == 2 && m_codexStep == 1) {
                m_codexStep = 2;
                note("codex-failed", msg.contains("error"));
                if (!msg.contains("error")) {
                    QVector<LimitWindow> next;
                    for (const LimitWindow &row : m_limitRows)
                        if (row.provider != "codex")
                            next.append(row);
                    next += parseCodexLimits(msg.value("result").toObject());
                    m_limitRows = next;
                }
                m_codexStep = 3;
                m_codex->kill();
                finishProbe(generation);
            }
        }
    });
    connect(m_codex, &QProcess::finished, this, [this, generation] {
        if (m_codexStep < 2) {
            note("codex-failed", true);
            m_codexStep = 3;
            finishProbe(generation);
        }
    });
    connect(m_codex, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_codexStep >= 2)
            return;
        note("codex-failed", true);
        m_codexStep = 3;
        finishProbe(generation);
    });
    QTimer::singleShot(15000, m_codex, [this, generation] {
        if (!m_codex || m_codexStep >= 2)
            return;
        note("codex-failed", true);
        m_codexStep = 3;
        m_codex->kill();
        finishProbe(generation);
    });
    m_codex->start(m_codexBin, {"app-server"});
    connect(m_codex, &QProcess::started, this, [this] {
        const QByteArray hello =
            "{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{\"name\":\"quota\",\"title\":\"Quota\","
            "\"version\":\"0.1.0\"},\"capabilities\":{\"experimentalApi\":true}}}\n";
        m_codex->write(hello);
    });
}
