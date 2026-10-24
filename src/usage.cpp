#include "usage.h"

#include <QJsonArray>
#include <QTimeZone>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

QString parseClaudeVersion(const QByteArray &output)
{
    static const QRegularExpression version(R"(^\s*(\d+\.\d+\.\d+))");
    return version.match(QString::fromUtf8(output)).captured(1);
}

double clampPercent(double value)
{
    if (!std::isfinite(value))
        return 0;
    return std::clamp(value, 0.0, 100.0);
}

static QString normKey(const QString &model)
{
    return model.trimmed().toLower();
}

static QString bareName(const QString &key)
{
    const int slash = key.lastIndexOf('/');
    return slash < 0 ? key : key.mid(slash + 1);
}

static QString stripVariant(const QString &key)
{
    const int bracket = key.indexOf('[');
    return bracket < 0 ? key : key.left(bracket);
}

static bool sameRate(const TokenRate &a, const TokenRate &b)
{
    return a.input == b.input && a.output == b.output && a.cacheRead == b.cacheRead && a.cacheWrite == b.cacheWrite;
}

static TokenRate readRates(const QJsonObject &entry, const QString &suffix, const TokenRate *standard)
{
    TokenRate rate;
    const QJsonValue input = entry.value("input_cost_per_token" + suffix);
    const QJsonValue output = entry.value("output_cost_per_token" + suffix);
    if (!input.isDouble() || !output.isDouble())
        return {};
    rate.input = input.toDouble();
    rate.output = output.toDouble();
    const auto cache = [&](const QString &name, double standardField, double standardInput) {
        const QJsonValue value = entry.value(name + suffix);
        if (value.isDouble())
            return value.toDouble();
        if (standard && standardInput > 0)
            return (standardField / standardInput) * rate.input;
        return rate.input;
    };
    rate.cacheRead = cache("cache_read_input_token_cost", standard ? standard->cacheRead : 0,
                           standard ? standard->input : 0);
    rate.cacheWrite = cache("cache_creation_input_token_cost", standard ? standard->cacheWrite : 0,
                            standard ? standard->input : 0);
    return rate;
}

static bool ratePresent(const TokenRate &rate)
{
    return rate.input != 0 || rate.output != 0;
}

QHash<QString, ModelRate> parseRateTable(const QJsonObject &document)
{
    QHash<QString, ModelRate> table;
    for (auto it = document.begin(); it != document.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject entry = it.value().toObject();
        const TokenRate standard = readRates(entry, {}, nullptr);
        if (!ratePresent(standard) && !entry.value("input_cost_per_token").isDouble())
            continue;
        if (!entry.value("input_cost_per_token").isDouble() || !entry.value("output_cost_per_token").isDouble())
            continue;
        const QString key = normKey(it.key());
        if (key.isEmpty())
            continue;
        ModelRate model;
        model.standard = standard;
        const QJsonObject specific = entry.value("provider_specific_entry").toObject();
        if (specific.value("fast").isDouble() && specific.value("fast").toDouble() > 0) {
            const double multiple = specific.value("fast").toDouble();
            model.hasFast = true;
            model.fast = {standard.input * multiple, standard.output * multiple, standard.cacheRead * multiple,
                          standard.cacheWrite * multiple};
        } else {
            const TokenRate fast = readRates(entry, "_priority", &standard);
            if (entry.value("input_cost_per_token_priority").isDouble()) {
                model.hasFast = true;
                model.fast = fast;
            }
        }
        if (entry.value("input_cost_per_token_ultrafast").isDouble()) {
            model.hasUltra = true;
            model.ultra = readRates(entry, "_ultrafast", &standard);
        }
        table.insert(key, model);
    }

    QHash<QString, ModelRate> aliases;
    QSet<QString> mixed;
    for (auto it = table.cbegin(); it != table.cend(); ++it) {
        const QString alias = bareName(it.key());
        if (alias.isEmpty() || alias == it.key() || table.contains(alias))
            continue;
        if (!aliases.contains(alias)) {
            aliases.insert(alias, it.value());
            continue;
        }
        if (!sameRate(aliases.value(alias).standard, it.value().standard))
            mixed.insert(alias);
    }
    for (auto it = aliases.cbegin(); it != aliases.cend(); ++it) {
        if (!mixed.contains(it.key()))
            table.insert(it.key(), it.value());
    }
    return table;
}

static double tokenCount(const QJsonValue &value)
{
    return value.isDouble() ? std::max(0.0, value.toDouble()) : 0;
}

QVector<UsageRec> parseUsageCache(const QJsonObject &root)
{
    QVector<UsageRec> records;
    const int version = root.value("version").toInt();
    if (version < 4 || version > 9)
        return records;
    const QJsonArray models = root.value("models").toArray();
    const QJsonObject files = root.value("files").toObject();
    const auto at = [](const QJsonArray &table, const QJsonValue &index) {
        if (!index.isDouble())
            return QString();
        const int i = index.toInt();
        return i >= 0 && i < table.size() ? table.at(i).toString() : QString();
    };
    const QJsonArray sessions = root.value("sessions").toArray();
    struct FileRows {
        QVector<UsageRec> records;
        QStringList sessionIds;
    };
    QVector<FileRows> perFile;
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject file = it.value().toObject();
        const QString provider = file.value("p").toString();
        if (provider.isEmpty())
            continue;
        FileRows rows;
        const auto take = [&](const QJsonArray &table) {
            for (const QJsonValue &raw : table) {
                const QJsonArray row = raw.toArray();
                if (row.size() < 8)
                    continue;
                UsageRec rec;
                rec.timestampMs = static_cast<qint64>(row.at(0).toDouble());
                rec.provider = provider;
                rec.model = at(models, row.at(1));
                rec.dedupeKey = row.size() > 8 && row.at(8).isString() ? row.at(8).toString() : QString();
                rec.uncached = tokenCount(row.at(3));
                rec.cached = tokenCount(row.at(4));
                rec.cacheWrite = tokenCount(row.at(5));
                rec.output = tokenCount(row.at(6));
                rec.reportedCost = row.size() > 9 && row.at(9).isDouble() ? row.at(9).toDouble() : -1;
                rec.speed = row.size() > 10 && row.at(10).isDouble() ? row.at(10).toInt() : 0;
                if (row.size() > 11 && row.at(11).isDouble())
                    rec.rateModel = at(models, row.at(11));
                // T3 Code drops these local placeholders when it reads the cache.
                if (provider == "claude" && rec.model == "<synthetic>")
                    continue;
                if (!rec.model.isEmpty()) {
                    rows.records.append(rec);
                    rows.sessionIds.append(at(sessions, row.at(2)));
                }
            }
        };
        take(file.value("r").toArray());
        take(file.value("t").toArray());
        perFile.append(rows);
    }

    // T3 Code dedupes Codex sessions that span files when it sums, not in the cache.
    // A resumed or forked session copies earlier requests into the new file.
    QHash<QString, int> firstFile;
    QSet<QString> shared;
    for (int index = 0; index < perFile.size(); ++index) {
        for (int i = 0; i < perFile[index].records.size(); ++i) {
            const QString &session = perFile[index].sessionIds[i];
            if (perFile[index].records[i].provider != "codex" || session.isEmpty())
                continue;
            const auto found = firstFile.constFind(session);
            if (found == firstFile.cend())
                firstFile.insert(session, index);
            else if (found.value() != index)
                shared.insert(session);
        }
    }
    for (FileRows &file : perFile) {
        QHash<QString, int> occurrences;
        for (int i = 0; i < file.records.size(); ++i) {
            UsageRec rec = file.records[i];
            const QString &session = file.sessionIds[i];
            if (rec.provider == "codex" && shared.contains(session)) {
                const QString key = session + '\n' + QString::number(rec.timestampMs) + '\n' + rec.model + '\n'
                    + QString::number(rec.uncached, 'f', 0) + '\n' + QString::number(rec.cached, 'f', 0) + '\n'
                    + QString::number(rec.cacheWrite, 'f', 0) + '\n' + QString::number(rec.output, 'f', 0);
                rec.dedupeKey = key + '\n' + QString::number(++occurrences[key]);
            }
            records.append(rec);
        }
    }
    return records;
}

QString localDay(qint64 timestampMs, const QTimeZone &zone)
{
    return QDateTime::fromMSecsSinceEpoch(timestampMs, zone).date().toString(Qt::ISODate);
}

UsageWindow makeUsageWindow(int days, const QDateTime &now, const QTimeZone &zone)
{
    UsageWindow window;
    const QDateTime local = now.toTimeZone(zone);
    window.untilMs = local.toMSecsSinceEpoch();
    if (days <= 1) {
        window.rolling24h = true;
        window.sinceMs = window.untilMs - 24LL * 60 * 60 * 1000;
        window.untilDay = local.date().toString(Qt::ISODate);
        window.sinceDay = QDateTime::fromMSecsSinceEpoch(window.sinceMs, zone).date().toString(Qt::ISODate);
        return window;
    }
    window.untilDay = local.date().toString(Qt::ISODate);
    window.sinceDay = local.date().addDays(-(days - 1)).toString(Qt::ISODate);
    return window;
}

static const TokenRate *ratesFor(const ModelRate &rate, int speed)
{
    if (speed == 1 && rate.hasFast)
        return &rate.fast;
    if (speed == 2 && rate.hasUltra)
        return &rate.ultra;
    return &rate.standard;
}

static const ModelRate *lookupRate(const QHash<QString, ModelRate> &table, const QString &model)
{
    static const QSet<QString> blocked{"<synthetic>", "synthetic", "opus", "sonnet", "haiku", "fable"};
    const QString key = stripVariant(normKey(model));
    const QString bare = bareName(key);
    if (bare.isEmpty() || blocked.contains(bare))
        return nullptr;
    const auto it = table.constFind(key);
    return it == table.cend() ? nullptr : &it.value();
}

static double priceRecord(const QHash<QString, ModelRate> &table, const UsageRec &rec, bool *unpriced)
{
    const ModelRate *rate = lookupRate(table, rec.rateModel.isEmpty() ? rec.model : rec.rateModel);
    const bool reported = rec.reportedCost >= 0 && std::isfinite(rec.reportedCost);
    if (!rate) {
        if (unpriced && !reported)
            *unpriced = true;
        return reported ? rec.reportedCost : 0;
    }
    if (reported)
        return rec.reportedCost;
    const TokenRate *tokenRate = ratesFor(*rate, rec.speed);
    return rec.uncached * tokenRate->input + rec.cached * tokenRate->cacheRead
        + rec.cacheWrite * tokenRate->cacheWrite + rec.output * tokenRate->output;
}

UsageSummary summarizeUsage(const QVector<UsageRec> &records, const QHash<QString, ModelRate> &rates,
                            const UsageWindow &window, const QTimeZone &zone)
{
    UsageSummary summary;
    QSet<QString> seen;
    QHash<QString, ModelSlice> models;
    for (const UsageRec &rec : records) {
        if (!rec.dedupeKey.isEmpty()) {
            if (seen.contains(rec.dedupeKey))
                continue;
            seen.insert(rec.dedupeKey);
        }
        const QString day = localDay(rec.timestampMs, zone);
        if (window.rolling24h) {
            if (rec.timestampMs < window.sinceMs || rec.timestampMs >= window.untilMs)
                continue;
        } else if (day < window.sinceDay || day > window.untilDay) {
            continue;
        }
        bool unpriced = false;
        const double cost = priceRecord(rates, rec, &unpriced);
        const double tokens = rec.uncached + rec.cached + rec.cacheWrite + rec.output;
        auto add = [&](UsageSlice &slice) {
            slice.tokens += tokens;
            slice.cost += cost;
            slice.records += 1;
            if (unpriced)
                slice.unpriced += 1;
        };
        add(summary.byProvider[rec.provider]);
        add(summary.byDay[day]);
        add(summary.byDayProvider[day][rec.provider]);
        if (window.rolling24h) {
            const int hour = std::clamp(static_cast<int>((rec.timestampMs - window.sinceMs) / 3'600'000), 0, 23);
            add(summary.byHourProvider[hour][rec.provider]);
        }
        add(summary.total);
        const QString modelKey = rec.provider + QChar(0) + rec.model;
        ModelSlice &model = models[modelKey];
        model.provider = rec.provider;
        model.model = rec.model;
        model.tokens += tokens;
        model.cost += cost;
    }
    summary.models = models.values();
    std::sort(summary.models.begin(), summary.models.end(), [](const ModelSlice &a, const ModelSlice &b) {
        if (a.cost != b.cost)
            return a.cost > b.cost;
        return a.tokens > b.tokens;
    });
    if (summary.models.size() > 12)
        summary.models.resize(12);
    return summary;
}

static QString isoReset(const QJsonValue &value)
{
    if (value.isString() && !value.toString().isEmpty())
        return value.toString();
    if (value.isDouble() && value.toDouble() > 0) {
        return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(value.toDouble()) * 1000, QTimeZone::utc())
            .toString(Qt::ISODateWithMs);
    }
    return {};
}

static QJsonValue usedValue(const QJsonObject &window)
{
    for (const char *key : {"utilization", "used_percent", "usedPercent", "percent"}) {
        const QJsonValue value = window.value(QLatin1String(key));
        if (value.isDouble())
            return value;
    }
    return {};
}

static QString resetValue(const QJsonObject &window)
{
    const QString iso = isoReset(window.value("resets_at"));
    return iso.isEmpty() ? isoReset(window.value("resetsAt")) : iso;
}

QVector<LimitWindow> parseClaudeLimits(const QJsonObject &body)
{
    // The Claude CLI returns windows at the top level. The SDK wraps the same
    // windows under rate_limits. Read whichever shape arrived.
    QJsonObject limits = body.value("rate_limits").toObject();
    if (limits.isEmpty())
        limits = body;
    struct Raw {
        QString kind;
        QString extra;
        double used = 0;
        QString resets;
    };
    QVector<Raw> raws;
    const auto push = [&](const QString &kind, const QString &extra, const QJsonObject &window) {
        const QJsonValue used = usedValue(window);
        if (!used.isDouble())
            return;
        raws.append({kind, extra, used.toDouble(), resetValue(window)});
    };
    for (auto it = limits.begin(); it != limits.end(); ++it) {
        if (it.key() == "five_hour" && it.value().isObject())
            push("session", {}, it.value().toObject());
        else if (it.key() == "seven_day" && it.value().isObject())
            push("weekly", {}, it.value().toObject());
        else if ((it.key() == "seven_day_opus" || it.key() == "seven_day_sonnet" || it.key() == "seven_day_haiku")
                 && it.value().isObject()) {
            QString extra = it.key().mid(QStringLiteral("seven_day_").size());
            if (!extra.isEmpty())
                extra[0] = extra[0].toUpper();
            push("weekly", extra, it.value().toObject());
        } else if (it.key() == "model_scoped" && it.value().isArray()) {
            for (const QJsonValue &entry : it.value().toArray())
                push("weekly", entry.toObject().value("display_name").toString(), entry.toObject());
        } else if (it.key() == "extra_usage" && it.value().isObject() && it.value().toObject().value("is_enabled").toBool()) {
            push("spend", {}, it.value().toObject());
        }
    }
    // The site route already sends a percent from 0 to 100.
    QVector<LimitWindow> windows;
    for (const Raw &raw : raws)
        windows.append({"claude", raw.kind, raw.extra, clampPercent(raw.used), raw.resets});
    const QJsonObject spend = limits.value("spend").toObject();
    if (spend.value("enabled").toBool(true) && spend.value("used").isObject()) {
        const QJsonObject used = spend.value("used").toObject();
        const QJsonObject cap = spend.value("limit").toObject();
        if (used.value("amount_minor").isDouble() && cap.value("amount_minor").toDouble() > 0
            && used.value("currency") == cap.value("currency")) {
            const double percent = used.value("amount_minor").toDouble() / cap.value("amount_minor").toDouble() * 100.0;
            windows.append({"claude", "spend", {}, clampPercent(percent), {}});
        }
    }
    return windows;
}

static LimitWindow codexWindow(const QString &kind, const QJsonObject &window)
{
    LimitWindow row;
    row.provider = "codex";
    row.kind = kind;
    row.usedPercent = clampPercent(window.value("usedPercent").toDouble());
    row.resetsAt = isoReset(window.value("resetsAt"));
    return row;
}

QVector<LimitWindow> parseCodexLimits(const QJsonObject &body)
{
    QJsonObject snapshot = body.value("rateLimits").toObject();
    const QJsonObject byId = body.value("rateLimitsByLimitId").toObject();
    if (byId.contains("codex"))
        snapshot = byId.value("codex").toObject();
    const QString limitId = snapshot.value("limitId").toString();
    if (!limitId.isEmpty() && limitId != "codex")
        return {};
    QVector<LimitWindow> windows;
    const QString plan = snapshot.value("planType").toString();
    const bool monthly = plan == "free" || plan == "go";
    if (snapshot.value("primary").isObject())
        windows.append(codexWindow(monthly ? "monthly" : "session", snapshot.value("primary").toObject()));
    if (snapshot.value("secondary").isObject())
        windows.append(codexWindow("weekly", snapshot.value("secondary").toObject()));
    const QJsonObject credit = snapshot.value("individualLimit").toObject();
    if (credit.value("remainingPercent").isDouble()) {
        windows.append({"codex", "credit", {}, clampPercent(100 - credit.value("remainingPercent").toDouble()),
                        isoReset(credit.value("resetsAt"))});
    }
    return windows;
}

QVector<LimitWindow> parseCodexWham(const QJsonObject &body)
{
    QJsonObject rate = body.value("rate_limit").toObject();
    if (rate.isEmpty())
        rate = body.value("rateLimit").toObject();
    QVector<LimitWindow> windows;
    const auto add = [&](const QJsonValue &raw) {
        if (!raw.isObject())
            return;
        const QJsonObject window = raw.toObject();
        const QJsonValue used = window.value("used_percent").isDouble() ? window.value("used_percent")
                                                                          : window.value("usedPercent");
        if (!used.isDouble())
            return;
        const double seconds = window.value("limit_window_seconds").toDouble();
        const QString kind = seconds >= 20 * 24 * 3600 ? "monthly" : seconds >= 6 * 24 * 3600 ? "weekly" : "session";
        const QJsonValue reset = window.value("reset_at").isDouble() ? window.value("reset_at") : window.value("resetAt");
        windows.append({"codex", kind, {}, clampPercent(used.toDouble()), isoReset(reset)});
    };
    add(rate.value("primary_window"));
    add(rate.value("secondary_window"));
    return windows;
}

QVector<LimitWindow> parseGrokLimits(const QJsonObject &body)
{
    const QJsonObject config = body.value("config").toObject();
    const QJsonObject period = config.value("currentPeriod").toObject();
    QString type = period.value("type").toString();
    type.remove(QStringLiteral("USAGE_PERIOD_TYPE_"));
    QString kind = "subscription";
    if (type == "WEEKLY")
        kind = "weekly";
    else if (type == "MONTHLY")
        kind = "monthly";
    if (!config.value("creditUsagePercent").isDouble() && kind == "subscription")
        return {};
    const double used = config.value("creditUsagePercent").isDouble() ? config.value("creditUsagePercent").toDouble() : 0;
    if (!std::isfinite(used))
        return {};
    return {{"grok", kind, {}, clampPercent(used), period.value("end").toString()}};
}
