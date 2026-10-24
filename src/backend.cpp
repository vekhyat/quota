#include "backend.h"

#include "history.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <memory>
#include <QTimeZone>
#include <QtConcurrent>

namespace {
const QStringList kProviderOrder{"claude", "codex", "grok", "cursor", "opencode", "antigravity", "pi", "muse"};
// Quota reads these itself. The T3 cache supplies every other provider.
const QStringList kLocalProviders{"claude", "codex", "pi", "grok"};
// T3 Code rewrites its cache while it runs. An older cache means T3 Code is closed or
// gone, and local history is the fresher source.
constexpr qint64 kT3FreshMs = 24LL * 60 * 60 * 1000;
// Folder watches miss a transcript that grows, so local history is also read on a timer.
constexpr int kHistoryPollMs = 60 * 1000;
// Used when the installed Claude Code does not answer --version.
const QString kFallbackClaudeVersion = QStringLiteral("2.1.310");

QString homePath(const char *env, const QString &fallback)
{
    const QString value = qEnvironmentVariable(env);
    return value.isEmpty() ? fallback : value;
}

QString firstEnv(const char *first, const char *second, const QString &fallback)
{
    const QString primary = qEnvironmentVariable(first);
    if (!primary.isEmpty())
        return primary;
    return homePath(second, fallback);
}
}

Backend::~Backend()
{
    for (QProcess *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) {
        process->disconnect(this);
        process->kill();
        process->waitForFinished(1000);
    }
    if (!m_rateReply)
        return;
    m_rateReply->disconnect(this);
    m_rateReply->abort();
}

Backend::Backend(bool preview, QObject *parent) : QObject(parent), m_preview(preview)
{
    m_network.setTransferTimeout(15000);
    m_t3Home = homePath("QUOTA_T3_HOME", QDir::homePath() + "/.t3");
    m_claudeDir = firstEnv("QUOTA_CLAUDE_DIR", "CLAUDE_CONFIG_DIR", QDir::homePath() + "/.claude");
    m_codexHome = firstEnv("QUOTA_CODEX_HOME", "CODEX_HOME", QDir::homePath() + "/.codex");
    m_cacheDir = homePath("QUOTA_CACHE_DIR", QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/quota");
    m_sourcesFile = homePath("QUOTA_SOURCES_FILE",
                             QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/quota/sources");
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
        loadUsage();
        rebuild();
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_cacheDebounce.start(); });
    m_historyDebounce.setSingleShot(true);
    m_historyDebounce.setInterval(1000);
    connect(&m_historyDebounce, &QTimer::timeout, this, [this] {
        if (!m_preview)
            loadUsage();
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_historyDebounce.start(); });
    m_historyPoll.setInterval(kHistoryPollMs);
    connect(&m_historyPoll, &QTimer::timeout, this, &Backend::startLocalScan);
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
                       {"codex", "session", {}, 81, at(180), "~/.codex-personal"},
                       {"codex", "weekly", {}, 11, at(5 * 24 * 60), "~/.codex-personal"},
                       {"codex", "session", {}, 23, at(120), "~/.codex-work"},
                       {"codex", "weekly", {}, 46, at(4 * 24 * 60), "~/.codex-work"}};
        m_cacheAt = now.addSecs(-14 * 60).toMSecsSinceEpoch();
        m_limitsAt = now.addSecs(-2 * 60).toMSecsSinceEpoch();
        m_notices.clear();
        m_busy = false;
        rebuild();
        return;
    }
    loadUsage();
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

void Backend::loadUsage()
{
    const QString cachePath = m_t3Home + "/userdata/usage-scan-cache-v9.json";
    QVector<UsageRec> t3Records;
    qint64 modified = 0;
    // Local history gives the same numbers as T3 Code for the providers Quota reads, and
    // it is current while a session is still writing. The T3 cache adds only the
    // providers Quota cannot read, and only while T3 Code keeps it fresh.
    const bool t3 = readT3UsageCache(cachePath, &t3Records, &modified)
        && QDateTime::currentMSecsSinceEpoch() - modified < kT3FreshMs;
    m_t3Extra.clear();
    if (t3) {
        for (const UsageRec &rec : t3Records) {
            if (!kLocalProviders.contains(rec.provider))
                m_t3Extra.append(rec);
        }
    }
    // Watch the cache even while it is stale, so its rows return when T3 Code runs again.
    if (QFileInfo::exists(cachePath) && !m_watcher.files().contains(cachePath))
        m_watcher.addPath(cachePath);
    loadRates(t3);
    if (!m_preview)
        m_historyPoll.start();
    startLocalScan();
}

void Backend::startLocalScan()
{
    if (m_preview)
        return;
    if (m_scanRunning) {
        m_rescan = true;
        return;
    }
    m_scanRunning = true;
    const int generation = ++m_scanGeneration;
    const QString claudeDir = m_claudeDir;
    const QString codexHome = m_codexHome;
    const QString grokHome = m_grokHome;
    const QString sourcesFile = m_sourcesFile;
    const QString cacheDir = m_cacheDir;
    auto *watcher = new QFutureWatcher<HistoryScan>(this);
    connect(watcher, &QFutureWatcher<HistoryScan>::finished, this, [this, watcher, generation] {
        const HistoryScan scan = watcher->result();
        watcher->deleteLater();
        if (generation != m_scanGeneration)
            return;
        m_scanRunning = false;
        m_records = scan.records + m_t3Extra;
        m_usageSource = m_t3Extra.isEmpty() ? "local" : "local+t3";
        m_cacheAt = QDateTime::currentMSecsSinceEpoch();
        note("no-cache", m_records.isEmpty());
        armHistoryWatch(scan.directories);
        rebuild();
        if (m_rescan) {
            m_rescan = false;
            startLocalScan();
        } else if (scan.rescan) {
            m_historyDebounce.start();
        }
    });
    watcher->setFuture(QtConcurrent::run([claudeDir, codexHome, grokHome, sourcesFile, cacheDir] {
        const HistorySources sources = discoverSources(QDir::homePath(), claudeDir, codexHome, grokHome, sourcesFile);
        return scanHistory(sources, cacheDir, 0);
    }));
}

void Backend::armHistoryWatch(const QStringList &dirs)
{
    const QStringList current = m_watcher.directories();
    for (const QString &path : current) {
        if (!dirs.contains(path))
            m_watcher.removePath(path);
    }
    for (const QString &path : dirs) {
        if (!m_watcher.directories().contains(path))
            m_watcher.addPath(path);
    }
}

void Backend::loadRates(bool preferT3)
{
    if (preferT3) {
        const QHash<QString, ModelRate> t3Rates = readRateFile(m_t3Home + "/userdata/usage-model-rates.json");
        if (!t3Rates.isEmpty()) {
            m_rates = t3Rates;
            m_hasRates = true;
            m_ratesFromT3 = true;
            note("no-rates", false);
            return;
        }
    }
    m_ratesFromT3 = false;
    const RateLoad loaded = loadModelRates(m_cacheDir, QStringLiteral(":/model-prices.json"));
    if (!loaded.rates.isEmpty())
        m_rates = loaded.rates;
    m_hasRates = !m_rates.isEmpty();
    note("no-rates", !m_hasRates);
    if (loaded.refresh)
        fetchRates();
}

void Backend::fetchRates()
{
    if (m_rateReply || m_preview)
        return;
    // This is the same LiteLLM price file that T3 Code downloads.
    QNetworkRequest request(QUrl(QStringLiteral(
        "https://raw.githubusercontent.com/BerriAI/litellm/main/model_prices_and_context_window.json")));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("quota"));
    request.setTransferTimeout(10000);
    m_rateReply = m_network.get(request);
    m_rateReply->setParent(this);
    connect(m_rateReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_rateReply;
        m_rateReply = nullptr;
        if (!reply)
            return;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QJsonObject document = QJsonDocument::fromJson(reply->readAll()).object();
        const QHash<QString, ModelRate> parsed = parseRateTable(document);
        if (parsed.isEmpty())
            return;
        storeRateDocument(m_cacheDir, document, QDateTime::currentMSecsSinceEpoch());
        if (m_ratesFromT3)
            return;
        m_rates = parsed;
        m_hasRates = true;
        note("no-rates", false);
        rebuild();
    });
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
    note("empty", summary.total.records == 0 && !m_notices.contains("no-cache") && !m_scanRunning);
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
                                    {"account", row.account},
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
    QStringList homes = discoverCodexHomes(QDir::homePath(), m_codexHome, m_sourcesFile);
    if (homes.isEmpty())
        homes.append(m_codexHome);
    m_probes += homes.size() - 1;
    for (qsizetype i = m_limitRows.size(); i-- > 0;)
        if (m_limitRows[i].provider == "codex")
            m_limitRows.removeAt(i);
    for (const QString &code : QStringList(m_notices))
        if (code.startsWith("codex-failed"))
            note(code, false);
    for (const QString &home : homes)
        readCodexHome(home, generation);
}

void Backend::finishCodex(const QString &home, const QVector<LimitWindow> &rows, int generation)
{
    if (generation != m_generation)
        return;
    note("codex-failed:" + home, rows.isEmpty());
    for (LimitWindow row : rows) {
        row.account = home;
        m_limitRows.append(row);
    }
    rebuild();
    finishProbe(generation);
}

void Backend::readCodexHome(const QString &home, int generation)
{
    QFile authFile(home + "/auth.json");
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
        connect(reply, &QNetworkReply::finished, this, [this, reply, generation, home] {
            reply->deleteLater();
            if (generation != m_generation)
                return;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
            const QVector<LimitWindow> parsed = status >= 200 && status < 300 ? parseCodexWham(body) : QVector<LimitWindow>{};
            if (!parsed.isEmpty()) {
                finishCodex(home, parsed, generation);
                return;
            }
            readCodexAppServer(home, generation);
        });
        return;
    }
    readCodexAppServer(home, generation);
}

void Backend::readCodexAppServer(const QString &home, int generation)
{
    auto *process = new QProcess(this);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("CODEX_HOME", home);
    process->setProcessEnvironment(environment);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    auto buffer = std::make_shared<QByteArray>();
    auto step = std::make_shared<int>(0);
    const auto finish = [this, process, step, home, generation](const QVector<LimitWindow> &rows) {
        if (*step == 2)
            return;
        *step = 2;
        process->disconnect(this);
        if (process->state() == QProcess::NotRunning)
            process->deleteLater();
        else {
            connect(process, &QProcess::finished, process, &QObject::deleteLater);
            process->kill();
        }
        finishCodex(home, rows, generation);
    };
    connect(process, &QProcess::readyReadStandardOutput, this, [process, buffer, step, finish] {
        *buffer += process->readAllStandardOutput();
        while (buffer->contains('\n')) {
            const int end = buffer->indexOf('\n');
            const QByteArray line = buffer->left(end);
            buffer->remove(0, end + 1);
            const QJsonObject msg = QJsonDocument::fromJson(line).object();
            if (msg.value("id").toInt() == 1 && *step == 0) {
                if (msg.contains("error")) {
                    finish({});
                    return;
                }
                *step = 1;
                process->write("{\"method\":\"initialized\"}\n");
                process->write("{\"id\":2,\"method\":\"account/rateLimits/read\",\"params\":null}\n");
            } else if (msg.value("id").toInt() == 2 && *step == 1) {
                finish(msg.contains("error") ? QVector<LimitWindow>{} : parseCodexLimits(msg.value("result").toObject()));
                return;
            }
        }
        if (buffer->size() > 1024 * 1024)
            finish({});
    });
    connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::finished, this, [finish] { finish({}); });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError) { finish({}); });
    QTimer::singleShot(15000, process, [finish] { finish({}); });
    connect(process, &QProcess::started, this, [process] {
        process->write("{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{\"name\":\"quota\",\"title\":\"Quota\","
                       "\"version\":\"0.3.0\"},\"capabilities\":{\"experimentalApi\":true}}}\n");
    });
    process->start(m_codexBin, {"app-server"});
}
