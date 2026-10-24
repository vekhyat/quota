#include "history.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <cmath>
#include <cstring>
#include <optional>

namespace {
constexpr qint64 kRetentionMs = 90LL * 24 * 60 * 60 * 1000;
constexpr qint64 kRateTtlMs = 24LL * 60 * 60 * 1000;
constexpr qint64 kForkGapMs = 1000;
constexpr int kGuardLength = 64;
// shortcut: a longer line is skipped, raise this if a transcript exceeds it.
constexpr int kMaxLine = 8 * 1024 * 1024;
constexpr int kCacheVersion = 1;

// These markers are the only lines that carry a count, a model, or a session id.
const char *kMarkers[] = {"\"usage\"", "\"token_count\"", "\"turn_context\"", "\"session_meta\"",
                          "\"thread_settings_applied\""};

quint32 fnv1a(const QByteArray &data)
{
    quint32 hash = 2166136261u;
    for (unsigned char byte : data)
        hash = (hash ^ byte) * 16777619u;
    return hash;
}

qint64 timestampMs(const QJsonValue &value)
{
    if (value.isDouble() && std::isfinite(value.toDouble()))
        return static_cast<qint64>(value.toDouble());
    if (!value.isString())
        return -1;
    const QString text = value.toString();
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(text, Qt::ISODate);
    return parsed.isValid() ? parsed.toMSecsSinceEpoch() : -1;
}

double tokenOrZero(const QJsonValue &value)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        return 0;
    return std::max(0.0, value.toDouble());
}

bool wholeCount(const QJsonValue &value, double *out)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        return false;
    const double number = value.toDouble();
    if (number < 0 || std::floor(number) != number)
        return false;
    *out = number;
    return true;
}

struct CodexUsage {
    double input = 0;
    double cached = 0;
    double write = 0;
    double output = 0;
    double reasoning = 0;
    double total = 0;
};

bool validUsage(const CodexUsage &usage)
{
    return usage.cached + usage.write <= usage.input && usage.reasoning <= usage.output
        && usage.total >= usage.input + usage.output;
}

bool readUsage(const QJsonValue &value, CodexUsage *usage)
{
    if (!value.isObject())
        return false;
    const QJsonObject object = value.toObject();
    CodexUsage next;
    if (!wholeCount(object.value("input_tokens"), &next.input) || !wholeCount(object.value("output_tokens"), &next.output))
        return false;
    const auto optional = [&](const char *key, double *field) {
        if (!object.contains(QLatin1String(key)) || object.value(QLatin1String(key)).isNull()) {
            *field = 0;
            return true;
        }
        return wholeCount(object.value(QLatin1String(key)), field);
    };
    if (!optional("cached_input_tokens", &next.cached) || !optional("cache_write_input_tokens", &next.write)
        || !optional("reasoning_output_tokens", &next.reasoning))
        return false;
    if (!object.contains("total_tokens") || object.value("total_tokens").isNull())
        next.total = next.input + next.output;
    else if (!wholeCount(object.value("total_tokens"), &next.total))
        return false;
    if (!validUsage(next))
        return false;
    *usage = next;
    return true;
}

bool sameUsage(const CodexUsage &left, const CodexUsage &right)
{
    return left.input == right.input && left.cached == right.cached && left.write == right.write && left.output == right.output
        && left.reasoning == right.reasoning && left.total == right.total;
}

QString usageSignature(const CodexUsage &usage)
{
    return QString::number(usage.input, 'f', 0) + ',' + QString::number(usage.cached, 'f', 0) + ','
        + QString::number(usage.write, 'f', 0) + ',' + QString::number(usage.output, 'f', 0) + ','
        + QString::number(usage.reasoning, 'f', 0) + ',' + QString::number(usage.total, 'f', 0);
}

CodexUsage addUsage(const CodexUsage *previous, const CodexUsage &last, bool *ok)
{
    CodexUsage sum = last;
    if (previous) {
        sum.input += previous->input;
        sum.cached += previous->cached;
        sum.write += previous->write;
        sum.output += previous->output;
        sum.reasoning += previous->reasoning;
        sum.total += previous->total;
    }
    *ok = validUsage(sum);
    return sum;
}

struct CodexCarry {
    QString model;
    int speed = 0;
    QString sessionId;
    QString signature;
    bool hasSignature = false;
    bool hasCumulative = false;
    CodexUsage cumulative;
    bool sawMeta = false;
    bool suppressFork = false;
    qint64 forkAnchor = 0;
};

bool forkedMeta(const QJsonObject &payload)
{
    if (payload.value("forked_from_id").isString())
        return true;
    const QJsonObject spawn = payload.value("source").toObject().value("subagent").toObject().value("thread_spawn").toObject();
    return spawn.value("parent_thread_id").isString();
}

int speedFromTier(const QJsonValue &tier)
{
    const QString text = tier.toString();
    if (text == "priority" || text == "fast")
        return 1;
    if (text == "ultrafast")
        return 2;
    return 0;
}

std::optional<UsageRec> parseClaudeLine(const QByteArray &line)
{
    const QJsonObject record = QJsonDocument::fromJson(line).object();
    if (record.value("type").toString() != "assistant")
        return std::nullopt;
    const QJsonObject message = record.value("message").toObject();
    const QJsonObject usage = message.value("usage").toObject();
    if (!message.value("usage").isObject())
        return std::nullopt;
    const qint64 stamp = timestampMs(record.value("timestamp"));
    const QString model = message.value("model").toString();
    // Local placeholders never reached the API. The price table also blocks this name.
    if (stamp < 0 || model.isEmpty() || model == "<synthetic>")
        return std::nullopt;
    UsageRec rec;
    rec.timestampMs = stamp;
    rec.provider = "claude";
    rec.model = model;
    rec.uncached = tokenOrZero(usage.value("input_tokens"));
    rec.cached = tokenOrZero(usage.value("cache_read_input_tokens"));
    rec.cacheWrite = tokenOrZero(usage.value("cache_creation_input_tokens"));
    rec.output = tokenOrZero(usage.value("output_tokens"));
    rec.speed = usage.value("speed").toString() == "fast" ? 1 : 0;
    const QJsonValue cost = record.value("costUSD");
    if (cost.isDouble() && std::isfinite(cost.toDouble()))
        rec.reportedCost = cost.toDouble();
    const QJsonValue messageId = message.value("id");
    const QJsonValue requestId = record.value("requestId");
    // Streaming writes the same usage again for each content block. The key drops the copies.
    if (messageId.isString() || requestId.isString())
        rec.dedupeKey = (messageId.isString() ? messageId.toString() : QString()) + ':'
            + (requestId.isString() ? requestId.toString() : QString());
    return rec;
}

std::optional<UsageRec> parseCodexLine(const QByteArray &line, CodexCarry &state)
{
    const QJsonObject record = QJsonDocument::fromJson(line).object();
    const QJsonObject payload = record.value("payload").toObject();
    if (!record.value("payload").isObject())
        return std::nullopt;
    const QString recordType = record.value("type").toString();
    const QString payloadType = payload.value("type").toString();
    if (recordType == "session_meta") {
        // A later meta belongs to an ancestor. Keep the id of this file.
        if (state.sawMeta)
            return std::nullopt;
        state.sawMeta = true;
        const QString id = payload.value("id").isString() ? payload.value("id").toString() : payload.value("session_id").toString();
        if (!id.isEmpty())
            state.sessionId = id;
        const qint64 stamp = timestampMs(record.value("timestamp"));
        if (stamp >= 0 && forkedMeta(payload)) {
            state.suppressFork = true;
            state.forkAnchor = stamp;
        }
        return std::nullopt;
    }
    if (recordType == "turn_context") {
        if (payload.value("model").isString())
            state.model = payload.value("model").toString();
        return std::nullopt;
    }
    if (payloadType == "thread_settings_applied") {
        state.speed = speedFromTier(payload.value("thread_settings").toObject().value("service_tier"));
        return std::nullopt;
    }
    if (payloadType != "token_count")
        return std::nullopt;
    const QJsonObject info = payload.value("info").toObject();
    if (!payload.value("info").isObject())
        return std::nullopt;
    const qint64 stamp = timestampMs(record.value("timestamp"));
    // A count before the model is known is not a billable request.
    if (stamp < 0 || state.model.isEmpty())
        return std::nullopt;

    CodexUsage last;
    const bool hasLast = readUsage(info.value("last_token_usage"), &last);
    const bool hasCumulative = info.contains("total_token_usage") && !info.value("total_token_usage").isNull();
    CodexUsage cumulative;
    const bool cumulativeOk = !hasCumulative || readUsage(info.value("total_token_usage"), &cumulative);

    if (state.suppressFork) {
        if (stamp - state.forkAnchor < kForkGapMs) {
            state.forkAnchor = stamp;
            // The parent session already billed this copy. Keep it only as the next baseline.
            if (hasCumulative && cumulativeOk) {
                state.cumulative = cumulative;
                state.hasCumulative = true;
            } else if (!hasCumulative && hasLast && (!state.hasSignature || state.signature != usageSignature(last))) {
                bool ok = false;
                const CodexUsage advanced = addUsage(state.hasCumulative ? &state.cumulative : nullptr, last, &ok);
                if (ok) {
                    state.cumulative = advanced;
                    state.hasCumulative = true;
                }
            }
            state.hasSignature = hasLast;
            state.signature = hasLast ? usageSignature(last) : QString();
            return std::nullopt;
        }
        state.suppressFork = false;
        state.hasSignature = false;
        state.signature.clear();
    }

    const bool hadCumulative = state.hasCumulative;
    const CodexUsage previous = state.cumulative;
    if (hasCumulative && cumulativeOk) {
        state.cumulative = cumulative;
        state.hasCumulative = true;
    }
    if (!hasLast || (hasCumulative && !cumulativeOk))
        return std::nullopt;
    // A zero count reports context size, not a request.
    if (last.input + last.output == 0) {
        state.hasSignature = false;
        state.signature.clear();
        return std::nullopt;
    }
    if (hasCumulative && cumulativeOk && hadCumulative && sameUsage(cumulative, previous))
        return std::nullopt;
    if (last.total != last.input + last.output)
        return std::nullopt;

    const QString signature = usageSignature(last);
    if (hasCumulative && cumulativeOk) {
        const bool restarted = sameUsage(cumulative, last);
        bool reconciled = false;
        if (!hadCumulative) {
            CodexUsage prefix;
            prefix.input = cumulative.input - last.input;
            prefix.cached = cumulative.cached - last.cached;
            prefix.write = cumulative.write - last.write;
            prefix.output = cumulative.output - last.output;
            prefix.reasoning = cumulative.reasoning - last.reasoning;
            prefix.total = cumulative.total - last.total;
            reconciled = prefix.input >= 0 && prefix.cached >= 0 && prefix.write >= 0 && prefix.output >= 0
                && prefix.reasoning >= 0 && prefix.total >= 0 && validUsage(prefix);
        } else {
            reconciled = cumulative.input - previous.input == last.input && cumulative.cached - previous.cached == last.cached
                && cumulative.write - previous.write == last.write && cumulative.output - previous.output == last.output
                && cumulative.reasoning - previous.reasoning == last.reasoning && cumulative.total - previous.total == last.total;
        }
        if (!restarted && !reconciled)
            return std::nullopt;
    } else {
        if (state.hasSignature && state.signature == signature)
            return std::nullopt;
        bool ok = false;
        const CodexUsage advanced = addUsage(state.hasCumulative ? &state.cumulative : nullptr, last, &ok);
        if (!ok)
            return std::nullopt;
        state.cumulative = advanced;
        state.hasCumulative = true;
    }
    state.signature = signature;
    state.hasSignature = true;
    // Codex input_tokens already includes the cached and cache-write parts.
    const double uncached = last.input - last.cached - last.write;
    if (uncached + last.cached + last.write + last.output == 0)
        return std::nullopt;
    UsageRec rec;
    rec.timestampMs = stamp;
    rec.provider = "codex";
    rec.model = state.model;
    rec.uncached = uncached;
    rec.cached = last.cached;
    rec.cacheWrite = last.write;
    rec.output = last.output;
    rec.speed = state.speed;
    return rec;
}

bool lineMayCount(const QByteArray &line)
{
    for (const char *marker : kMarkers) {
        if (line.contains(marker))
            return true;
    }
    return false;
}

struct FileRead {
    QVector<UsageRec> records;
    CodexCarry carry;
    QString sessionId;
    qint64 offset = 0;
    quint32 guard = 0;
    int guardLength = 0;
    qint64 size = 0;
    qint64 mtimeMs = 0;
    qint64 bytesRead = 0;
    bool rescan = false;
    bool ok = false;
};

struct CachedFile {
    qint64 size = 0;
    qint64 mtimeMs = 0;
    qint64 offset = 0;
    quint32 guard = 0;
    int guardLength = 0;
    QString provider;
    QString sessionId;
    CodexCarry carry;
    QVector<UsageRec> records;
};

QJsonObject carryToJson(const CodexCarry &carry)
{
    return {{"model", carry.model},
            {"speed", carry.speed},
            {"session", carry.sessionId},
            {"sig", carry.signature},
            {"hasSig", carry.hasSignature},
            {"hasCum", carry.hasCumulative},
            {"in", carry.cumulative.input},
            {"cached", carry.cumulative.cached},
            {"write", carry.cumulative.write},
            {"out", carry.cumulative.output},
            {"reason", carry.cumulative.reasoning},
            {"total", carry.cumulative.total},
            {"meta", carry.sawMeta},
            {"fork", carry.suppressFork},
            {"anchor", carry.forkAnchor}};
}

CodexCarry carryFromJson(const QJsonObject &object)
{
    CodexCarry carry;
    carry.model = object.value("model").toString();
    carry.speed = object.value("speed").toInt();
    carry.sessionId = object.value("session").toString();
    carry.signature = object.value("sig").toString();
    carry.hasSignature = object.value("hasSig").toBool();
    carry.hasCumulative = object.value("hasCum").toBool();
    carry.cumulative.input = object.value("in").toDouble();
    carry.cumulative.cached = object.value("cached").toDouble();
    carry.cumulative.write = object.value("write").toDouble();
    carry.cumulative.output = object.value("out").toDouble();
    carry.cumulative.reasoning = object.value("reason").toDouble();
    carry.cumulative.total = object.value("total").toDouble();
    carry.sawMeta = object.value("meta").toBool();
    carry.suppressFork = object.value("fork").toBool();
    carry.forkAnchor = static_cast<qint64>(object.value("anchor").toDouble());
    return carry;
}

QJsonArray recordToJson(const UsageRec &rec)
{
    return {rec.timestampMs, rec.model, rec.dedupeKey, rec.uncached, rec.cached, rec.cacheWrite, rec.output, rec.reportedCost,
            rec.speed};
}

UsageRec recordFromJson(const QJsonArray &row, const QString &provider)
{
    UsageRec rec;
    if (row.size() < 9)
        return rec;
    rec.timestampMs = static_cast<qint64>(row.at(0).toDouble());
    rec.provider = provider;
    rec.model = row.at(1).toString();
    rec.dedupeKey = row.at(2).toString();
    rec.uncached = row.at(3).toDouble();
    rec.cached = row.at(4).toDouble();
    rec.cacheWrite = row.at(5).toDouble();
    rec.output = row.at(6).toDouble();
    rec.reportedCost = row.at(7).toDouble();
    rec.speed = row.at(8).toInt();
    return rec;
}

QHash<QString, CachedFile> readScanCache(const QString &path)
{
    QHash<QString, CachedFile> files;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return files;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value("v").toInt() != kCacheVersion)
        return files;
    const QJsonObject table = root.value("files").toObject();
    for (auto it = table.begin(); it != table.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        CachedFile cached;
        cached.size = static_cast<qint64>(entry.value("size").toDouble());
        cached.mtimeMs = static_cast<qint64>(entry.value("mtime").toDouble());
        cached.offset = static_cast<qint64>(entry.value("offset").toDouble());
        cached.guard = static_cast<quint32>(entry.value("guard").toDouble());
        cached.guardLength = entry.value("glen").toInt();
        cached.provider = entry.value("provider").toString();
        cached.sessionId = entry.value("session").toString();
        cached.carry = carryFromJson(entry.value("carry").toObject());
        for (const QJsonValue &row : entry.value("records").toArray()) {
            const UsageRec rec = recordFromJson(row.toArray(), cached.provider);
            if (!rec.model.isEmpty())
                cached.records.append(rec);
        }
        files.insert(it.key(), cached);
    }
    return files;
}

void writeScanCache(const QString &path, const QHash<QString, CachedFile> &files)
{
    QJsonObject table;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QJsonArray records;
        for (const UsageRec &rec : it.value().records)
            records.append(recordToJson(rec));
        table.insert(it.key(), QJsonObject{{"size", it.value().size},
                                           {"mtime", it.value().mtimeMs},
                                           {"offset", it.value().offset},
                                           {"guard", static_cast<double>(it.value().guard)},
                                           {"glen", it.value().guardLength},
                                           {"provider", it.value().provider},
                                           {"session", it.value().sessionId},
                                           {"carry", carryToJson(it.value().carry)},
                                           {"records", records}});
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(QJsonObject{{"v", kCacheVersion}, {"files", table}}).toJson(QJsonDocument::Compact));
    file.commit();
}

bool guardMatches(QFile &file, qint64 offset, int length, quint32 expected)
{
    if (length <= 0 || length > kGuardLength || offset < length)
        return false;
    if (!file.seek(offset - length))
        return false;
    const QByteArray window = file.read(length);
    return window.size() == length && fnv1a(window) == expected;
}

FileRead readTranscript(const QString &path, const QString &provider, const CachedFile *cached, qint64 cutoffMs)
{
    FileRead result;
    const QFileInfo info(path);
    if (!info.isFile() || info.lastModified().toMSecsSinceEpoch() < cutoffMs)
        return result;
    result.mtimeMs = info.lastModified().toMSecsSinceEpoch();
    result.size = info.size();
    if (cached && cached->provider == provider && cached->size == result.size && cached->mtimeMs == result.mtimeMs) {
        result.records = cached->records;
        result.carry = cached->carry;
        result.sessionId = cached->sessionId;
        result.offset = cached->offset;
        result.guard = cached->guard;
        result.guardLength = cached->guardLength;
        result.ok = true;
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (cached && cached->provider == provider) {
            result = FileRead();
            result.records = cached->records;
            result.carry = cached->carry;
            result.sessionId = cached->sessionId;
            result.offset = cached->offset;
            result.guard = cached->guard;
            result.guardLength = cached->guardLength;
            result.size = cached->size;
            result.mtimeMs = cached->mtimeMs;
            result.ok = true;
            result.rescan = true;
        }
        return result;
    }

    qint64 start = 0;
    if (cached && cached->provider == provider && cached->offset > 0 && cached->offset <= result.size
        && guardMatches(file, cached->offset, cached->guardLength, cached->guard)) {
        start = cached->offset;
        result.offset = start;
        result.records = cached->records;
        result.carry = cached->carry;
        result.sessionId = cached->sessionId.isEmpty() ? cached->carry.sessionId : cached->sessionId;
    }
    if (!file.seek(start))
        return result;

    QByteArray line;
    qint64 absolute = start;
    bool skipLine = false;
    char buffer[256 * 1024];
    const auto finishLine = [&](qint64 lineEnd) {
        if (!skipLine && lineMayCount(line)) {
            if (provider == "claude") {
                if (const std::optional<UsageRec> rec = parseClaudeLine(line))
                    result.records.append(*rec);
            } else if (const std::optional<UsageRec> rec = parseCodexLine(line, result.carry)) {
                result.records.append(*rec);
            }
            result.sessionId = result.carry.sessionId;
        }
        // A line without a newline can still grow. Commit only a finished line.
        line.clear();
        skipLine = false;
        result.offset = lineEnd;
    };
    while (true) {
        const qint64 got = file.read(buffer, sizeof buffer);
        if (got < 0) {
            result.rescan = true;
            break;
        }
        if (got == 0)
            break;
        result.bytesRead += got;
        int index = 0;
        while (index < got) {
            if (skipLine) {
                const void *newline = std::memchr(buffer + index, '\n', static_cast<size_t>(got - index));
                if (!newline) {
                    absolute += got - index;
                    index = static_cast<int>(got);
                    break;
                }
                const int at = static_cast<const char *>(newline) - buffer;
                absolute += at - index + 1;
                index = at + 1;
                skipLine = false;
                result.offset = absolute;
                continue;
            }
            const void *newline = std::memchr(buffer + index, '\n', static_cast<size_t>(got - index));
            if (!newline) {
                const int add = static_cast<int>(got - index);
                if (line.size() + add > kMaxLine) {
                    skipLine = true;
                    line.clear();
                    absolute += add;
                    index = static_cast<int>(got);
                    break;
                }
                line.append(buffer + index, add);
                absolute += add;
                break;
            }
            const int at = static_cast<const char *>(newline) - buffer;
            const int add = at - index;
            absolute += add + 1;
            index = at + 1;
            if (line.size() + add > kMaxLine) {
                line.clear();
                result.offset = absolute;
                continue;
            }
            line.append(buffer + index - add - 1, add);
            finishLine(absolute);
        }
    }
    const QFileInfo after(path);
    // Store the size we finished. Ignore bytes that arrive after the read stops.
    if (after.size() != start + result.bytesRead || after.lastModified().toMSecsSinceEpoch() != result.mtimeMs)
        result.rescan = true;
    result.size = start + result.bytesRead;
    if (result.offset > 0) {
        const int length = static_cast<int>(std::min<qint64>(kGuardLength, result.offset));
        if (file.seek(result.offset - length)) {
            const QByteArray window = file.read(length);
            if (window.size() == length) {
                result.guard = fnv1a(window);
                result.guardLength = length;
            }
        }
    }
    result.ok = true;
    return result;
}

void collectDirs(const QString &root, QStringList *dirs)
{
    const QFileInfo info(root);
    if (!info.exists() || !info.isDir())
        return;
    dirs->append(info.absoluteFilePath());
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        dirs->append(it.fileInfo().absoluteFilePath());
    }
}

QString eventKey(const QString &sessionId, const UsageRec &rec)
{
    return sessionId + '\n' + QString::number(rec.timestampMs) + '\n' + rec.model + '\n'
        + QString::number(rec.uncached, 'f', 0) + '\n' + QString::number(rec.cached, 'f', 0) + '\n'
        + QString::number(rec.cacheWrite, 'f', 0) + '\n' + QString::number(rec.output, 'f', 0);
}

QJsonObject rateDocument(const QJsonObject &root)
{
    const QJsonValue document = root.value("document");
    return document.isObject() ? document.toObject() : root;
}
}

HistoryScan scanHistory(const QString &claudeDir, const QString &codexHome, const QString &cacheDir, qint64 nowMs)
{
    HistoryScan scan;
    if (nowMs <= 0)
        nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 cutoff = nowMs - kRetentionMs;
    const QString cachePath = QDir(cacheDir).filePath("scan-cache.json");
    const QHash<QString, CachedFile> previous = readScanCache(cachePath);
    QHash<QString, CachedFile> next;
    struct Held {
        QString sessionId;
        QString provider;
        QVector<UsageRec> records;
    };
    QVector<Held> held;
    const auto take = [&](const QString &root, const QString &provider) {
        if (!QDir(root).exists())
            return;
        QDirIterator it(root, {"*.jsonl"}, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo info = it.fileInfo();
            if (info.lastModified().toMSecsSinceEpoch() < cutoff)
                continue;
            const QString path = info.absoluteFilePath();
            const auto found = previous.constFind(path);
            const CachedFile *cached = found == previous.cend() ? nullptr : &found.value();
            const FileRead read = readTranscript(path, provider, cached, cutoff);
            if (!read.ok)
                continue;
            if (read.rescan)
                scan.rescan = true;
            scan.bytesRead += read.bytesRead;
            CachedFile stored;
            stored.size = read.size;
            stored.mtimeMs = read.mtimeMs;
            stored.offset = read.offset;
            stored.guard = read.guard;
            stored.guardLength = read.guardLength;
            stored.provider = provider;
            stored.sessionId = read.sessionId;
            stored.carry = read.carry;
            stored.records = read.records;
            next.insert(info.absoluteFilePath(), stored);
            held.append({read.sessionId, provider, read.records});
        }
    };
    take(QDir(claudeDir).filePath("projects"), "claude");
    take(QDir(codexHome).filePath("sessions"), "codex");
    writeScanCache(cachePath, next);

    QHash<QString, int> sessionFiles;
    for (const Held &file : held) {
        if (file.provider == "codex" && !file.sessionId.isEmpty())
            sessionFiles[file.sessionId] += 1;
    }
    for (const Held &file : held) {
        const bool shared = file.provider == "codex" && sessionFiles.value(file.sessionId) > 1;
        QHash<QString, int> seen;
        for (UsageRec rec : file.records) {
            if (shared) {
                const QString base = eventKey(file.sessionId, rec);
                const int occurrence = seen[base]++;
                rec.dedupeKey = base + '\n' + QString::number(occurrence);
            }
            if (!rec.model.isEmpty())
                scan.records.append(rec);
        }
    }
    collectDirs(QDir(claudeDir).filePath("projects"), &scan.directories);
    collectDirs(QDir(codexHome).filePath("sessions"), &scan.directories);
    if (scan.directories.isEmpty()) {
        if (QFileInfo(claudeDir).isDir())
            scan.directories.append(QFileInfo(claudeDir).absoluteFilePath());
        if (QFileInfo(codexHome).isDir())
            scan.directories.append(QFileInfo(codexHome).absoluteFilePath());
    }
    return scan;
}

QHash<QString, ModelRate> readRateFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.isEmpty())
        return {};
    return parseRateTable(rateDocument(root));
}

bool storeRateDocument(const QString &cacheDir, const QJsonObject &document, qint64 nowMs)
{
    if (document.isEmpty() || cacheDir.isEmpty())
        return false;
    if (nowMs <= 0)
        nowMs = QDateTime::currentMSecsSinceEpoch();
    QDir().mkpath(cacheDir);
    QSaveFile file(QDir(cacheDir).filePath("model-prices.json"));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QJsonObject wrapped{{"fetchedAtMs", nowMs}, {"document", document}};
    file.write(QJsonDocument(wrapped).toJson(QJsonDocument::Compact));
    return file.commit();
}

RateLoad loadModelRates(const QString &cacheDir, const QString &bundledPath, qint64 nowMs,
                        const std::function<QByteArray()> &fetch)
{
    RateLoad load;
    if (nowMs <= 0)
        nowMs = QDateTime::currentMSecsSinceEpoch();
    const QString cachePath = QDir(cacheDir).filePath("model-prices.json");
    QFile cache(cachePath);
    qint64 fetchedAt = 0;
    QHash<QString, ModelRate> cached;
    if (cache.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(cache.readAll()).object();
        fetchedAt = static_cast<qint64>(root.value("fetchedAtMs").toDouble());
        cached = parseRateTable(rateDocument(root));
    }
    const bool fresh = !cached.isEmpty() && fetchedAt > 0 && nowMs - fetchedAt < kRateTtlMs;
    if (fresh) {
        load.rates = cached;
        load.origin = "cache";
        return load;
    }
    if (fetch) {
        const QJsonObject document = QJsonDocument::fromJson(fetch()).object();
        const QHash<QString, ModelRate> downloaded = parseRateTable(document);
        if (!downloaded.isEmpty() && storeRateDocument(cacheDir, document, nowMs)) {
            load.rates = downloaded;
            load.origin = "download";
            return load;
        }
        if (!downloaded.isEmpty()) {
            load.rates = downloaded;
            load.origin = "download";
            return load;
        }
    }
    if (!cached.isEmpty()) {
        load.rates = cached;
        load.origin = "cache";
        load.refresh = true;
        return load;
    }
    load.rates = readRateFile(bundledPath);
    if (!load.rates.isEmpty()) {
        load.origin = "bundled";
        load.refresh = true;
    } else {
        load.refresh = true;
    }
    return load;
}

bool readT3UsageCache(const QString &path, QVector<UsageRec> *records, qint64 *modifiedMs)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !records)
        return false;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const int version = root.value("version").toInt();
    if (version < 4 || version > 9)
        return false;
    *records = parseUsageCache(root);
    if (modifiedMs)
        *modifiedMs = QFileInfo(file).lastModified().toMSecsSinceEpoch();
    return true;
}
