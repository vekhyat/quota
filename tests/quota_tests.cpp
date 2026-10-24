#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include "history.h"
#include "theme.h"
#include "translator.h"
#include "usage.h"

namespace {
QByteArray claudeLine(const char *id, int input, int output, const char *stamp = "2026-08-07T04:05:13.944Z")
{
    const QJsonObject line{{"type", "assistant"},
                           {"timestamp", stamp},
                           {"requestId", QString("req-") + id},
                           {"message", QJsonObject{{"id", QString("msg-") + id},
                                                     {"model", "claude-opus"},
                                                     {"usage", QJsonObject{{"input_tokens", input}, {"output_tokens", output}}}}}};
    return QJsonDocument(line).toJson(QJsonDocument::Compact);
}

QByteArray codexCount(int input, int cached, int write, int output, int cumInput, int cumCached, int cumWrite, int cumOutput,
                      const char *stamp)
{
    const auto usage = [](int in, int cache, int created, int out) {
        return QJsonObject{{"input_tokens", in},
                           {"cached_input_tokens", cache},
                           {"cache_write_input_tokens", created},
                           {"output_tokens", out},
                           {"reasoning_output_tokens", 0},
                           {"total_tokens", in + out}};
    };
    const QJsonObject line{
        {"type", "event_msg"},
        {"timestamp", stamp},
        {"payload", QJsonObject{{"type", "token_count"},
                                 {"info", QJsonObject{{"last_token_usage", usage(input, cached, write, output)},
                                                       {"total_token_usage", usage(cumInput, cumCached, cumWrite, cumOutput)}}}}},
    };
    return QJsonDocument(line).toJson(QJsonDocument::Compact);
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(bytes) == bytes.size();
}
}

class QuotaTests : public QObject {
    Q_OBJECT

private slots:
    void pricedRecordDropsDuplicateAndOldRows()
    {
        const QJsonObject rates = QJsonDocument::fromJson(R"({
            "claude-opus": {
                "input_cost_per_token": 0.000003,
                "output_cost_per_token": 0.000015
            }
        })").object();
        const auto table = parseRateTable(rates);
        const QTimeZone utc("UTC");
        const QDateTime now(QDate(2026, 10, 24), QTime(12, 0), utc);
        const UsageWindow window = makeUsageWindow(7, now, utc);
        const qint64 inside = now.addDays(-1).toMSecsSinceEpoch();
        const qint64 old = now.addDays(-30).toMSecsSinceEpoch();
        const QVector<UsageRec> records = {
            {inside, "claude", "claude-opus", "same", 1000, 0, 0, 100, -1, 0, {}},
            {inside, "claude", "claude-opus", "same", 1000, 0, 0, 100, -1, 0, {}},
            {old, "claude", "claude-opus", "old", 5000, 0, 0, 0, -1, 0, {}},
        };
        const UsageSummary summary = summarizeUsage(records, table, window, utc);
        QCOMPARE(summary.total.records, 1);
        QCOMPARE(summary.total.tokens, 1100.0);
        QCOMPARE(summary.total.cost, 0.0045);
        QCOMPARE(summary.total.unpriced, 0);
    }

    void bucketsSplitByProviderAndHour()
    {
        const QTimeZone utc("UTC");
        const QDateTime now(QDate(2026, 10, 24), QTime(12, 0), utc);
        const qint64 hourAgo = now.addSecs(-30 * 60).toMSecsSinceEpoch();
        const qint64 dayStart = now.addSecs(-23 * 3600 - 30 * 60).toMSecsSinceEpoch();
        const QVector<UsageRec> records = {
            {hourAgo, "claude", "a", {}, 10, 0, 0, 0, 1.0, 0, {}},
            {hourAgo, "codex", "b", {}, 5, 0, 0, 0, 2.0, 0, {}},
            {dayStart, "claude", "a", {}, 7, 0, 0, 0, 0.5, 0, {}},
        };
        const UsageSummary rolling = summarizeUsage(records, {}, makeUsageWindow(1, now, utc), utc);
        QCOMPARE(rolling.byHourProvider.value(23).value("claude").tokens, 10.0);
        QCOMPARE(rolling.byHourProvider.value(23).value("codex").cost, 2.0);
        QCOMPARE(rolling.byHourProvider.value(0).value("claude").tokens, 7.0);

        const UsageSummary week = summarizeUsage(records, {}, makeUsageWindow(7, now, utc), utc);
        QCOMPARE(week.byDayProvider.value("2026-10-24").value("claude").tokens, 10.0);
        QCOMPARE(week.byDayProvider.value("2026-10-23").value("claude").tokens, 7.0);
        QVERIFY(week.byHourProvider.isEmpty());
    }

    void reportedCostWinsWhenTheModelHasNoRate()
    {
        const UsageWindow window = makeUsageWindow(1, QDateTime::fromMSecsSinceEpoch(1'000'000, QTimeZone::utc()), QTimeZone("UTC"));
        const UsageRec rec{500000, "pi", "local-model", {}, 10, 0, 0, 10, 1.25, 0, {}};
        const UsageSummary summary = summarizeUsage({rec}, {}, window, QTimeZone("UTC"));
        QCOMPARE(summary.total.cost, 1.25);
        QCOMPARE(summary.total.unpriced, 0);
    }

    void claudeLimitsMapSessionWeeklyAndSpend()
    {
        const QJsonObject body = QJsonDocument::fromJson(R"({
            "rate_limits_available": true,
            "rate_limits": {
                "five_hour": {"utilization": 54, "resets_at": "2026-07-18T14:39:00Z"},
                "seven_day": {"utilization": 250, "resets_at": null},
                "model_scoped": [{"display_name": "Fable", "utilization": 73, "resets_at": "2026-07-24T08:59:00Z"}],
                "spend": {
                    "enabled": true,
                    "used": {"amount_minor": 4631, "currency": "USD", "exponent": 2},
                    "limit": {"amount_minor": 50000, "currency": "USD", "exponent": 2}
                }
            }
        })").object();
        const auto rows = parseClaudeLimits(body);
        QCOMPARE(rows.size(), 4);
        const auto session = std::find_if(rows.begin(), rows.end(), [](const LimitWindow &row) {
            return row.kind == "session";
        });
        const auto week = std::find_if(rows.begin(), rows.end(), [](const LimitWindow &row) {
            return row.kind == "weekly" && row.extra.isEmpty();
        });
        const auto scoped = std::find_if(rows.begin(), rows.end(), [](const LimitWindow &row) {
            return row.extra == "Fable";
        });
        const auto spend = std::find_if(rows.begin(), rows.end(), [](const LimitWindow &row) {
            return row.kind == "spend";
        });
        QVERIFY(session != rows.end());
        QVERIFY(week != rows.end());
        QVERIFY(scoped != rows.end());
        QVERIFY(spend != rows.end());
        QCOMPARE(session->usedPercent, 54.0);
        QCOMPARE(week->usedPercent, 100.0);
        QVERIFY(spend->usedPercent > 9.0 && spend->usedPercent < 9.3);
    }

    void codexLimitsUseTheCodexBucket()
    {
        const QJsonObject body = QJsonDocument::fromJson(R"({
            "rateLimits": {"planType": "plus", "limitId": "other", "primary": {"usedPercent": 99}},
            "rateLimitsByLimitId": {
                "codex": {
                    "planType": "plus",
                    "limitId": "codex",
                    "primary": {"usedPercent": 12, "resetsAt": 1780000000},
                    "secondary": {"usedPercent": 3}
                }
            }
        })").object();
        const auto rows = parseCodexLimits(body);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).kind, "session");
        QCOMPARE(rows.at(0).usedPercent, 12.0);
        QVERIFY(!rows.at(0).resetsAt.isEmpty());
        QCOMPARE(rows.at(1).kind, "weekly");
    }

    void claudeSiteRouteKeepsPercent()
    {
        const QJsonObject body = QJsonDocument::fromJson(R"({
            "five_hour": {"utilization": 7, "resets_at": "2026-10-24T18:00:00Z"},
            "seven_day": {"utilization": 21, "resets_at": "2026-10-30T08:00:00Z"},
            "seven_day_opus": {"utilization": 42, "resets_at": null}
        })").object();
        const auto rows = parseClaudeLimits(body);
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.at(0).usedPercent, 7.0);
        QCOMPARE(rows.at(1).kind, "weekly");
        QCOMPARE(rows.at(2).extra, "Opus");
        QCOMPARE(rows.at(2).usedPercent, 42.0);
        const QJsonObject noisy = QJsonDocument::fromJson(R"({
            "five_hour": {"utilization": 7},
            "seven_day_omelette": {"utilization": 90},
            "seven_day_cowork": {"utilization": 12}
        })").object();
        QCOMPARE(parseClaudeLimits(noisy).size(), 1);
    }

    void codexSiteRouteReadsWindows()
    {
        const QJsonObject body = QJsonDocument::fromJson(R"({
            "rate_limit": {
                "primary_window": {"used_percent": 12, "limit_window_seconds": 18000, "reset_at": 1780000000},
                "secondary_window": {"used_percent": 3, "limit_window_seconds": 604800, "reset_at": 1780500000}
            }
        })").object();
        const auto rows = parseCodexWham(body);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).kind, "session");
        QCOMPARE(rows.at(0).usedPercent, 12.0);
        QCOMPARE(rows.at(1).kind, "weekly");
        QVERIFY(!rows.at(0).resetsAt.isEmpty());
    }

    void claudeVersionComesFromTheCli()
    {
        QCOMPARE(parseClaudeVersion("2.1.310 (Claude Code)\n"), QString("2.1.310"));
        QCOMPARE(parseClaudeVersion("  3.0.1\n"), QString("3.0.1"));
        QVERIFY(parseClaudeVersion("command not found").isEmpty());
    }

    void cacheSkipsABadVersion()
    {
        QCOMPARE(parseUsageCache(QJsonObject{{"version", 1}}).size(), 0);
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\n");
        colors.close();
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void claudeDedupeKeepsTheFirstStreamCopy()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/claude/projects/demo/session.jsonl";
        QVERIFY(writeFile(path, claudeLine("one", 10, 2) + "\n" + claudeLine("one", 99, 8) + "\n" + claudeLine("two", 4, 1) + "\n"));
        const HistoryScan scan = scanHistory(dir.path() + "/claude", dir.path() + "/codex", dir.path() + "/cache");
        QCOMPARE(scan.records.size(), 3);
        QCOMPARE(scan.records.at(0).dedupeKey, scan.records.at(1).dedupeKey);
        const QTimeZone utc("UTC");
        const UsageSummary summary = summarizeUsage(scan.records, {}, makeUsageWindow(7, QDateTime(QDate(2026, 8, 8), QTime(12, 0), utc), utc), utc);
        QCOMPARE(summary.total.records, 2);
        QCOMPARE(summary.total.tokens, 17.0);
    }

    void codexSkipsARepeatedTokenCount()
    {
        QTemporaryDir dir;
        const QByteArray turn = QJsonDocument(QJsonObject{{"type", "turn_context"}, {"timestamp", "2026-08-01T05:17:42.694Z"}, {"payload", QJsonObject{{"model", "gpt-5"}}}}).toJson(QJsonDocument::Compact);
        const QByteArray body = turn + "\n" + codexCount(100, 0, 0, 10, 100, 0, 0, 10, "2026-08-01T05:17:49.919Z") + "\n"
            + codexCount(100, 0, 0, 10, 100, 0, 0, 10, "2026-08-01T05:17:50.919Z") + "\n";
        QVERIFY(writeFile(dir.path() + "/codex/sessions/day/session.jsonl", body));
        const HistoryScan scan = scanHistory(dir.path() + "/claude", dir.path() + "/codex", dir.path() + "/cache");
        QCOMPARE(scan.records.size(), 1);
        QCOMPARE(scan.records.at(0).output, 10.0);
    }

    void codexSubtractsCachedInput()
    {
        QTemporaryDir dir;
        const QByteArray turn = QJsonDocument(QJsonObject{{"type", "turn_context"}, {"timestamp", "2026-08-01T05:17:42.694Z"}, {"payload", QJsonObject{{"model", "gpt-5"}}}}).toJson(QJsonDocument::Compact);
        const QByteArray body = turn + "\n" + codexCount(100, 40, 10, 5, 100, 40, 10, 5, "2026-08-01T05:17:49.919Z") + "\n";
        QVERIFY(writeFile(dir.path() + "/codex/sessions/day/session.jsonl", body));
        const HistoryScan scan = scanHistory(dir.path() + "/claude", dir.path() + "/codex", dir.path() + "/cache");
        QCOMPARE(scan.records.size(), 1);
        QCOMPARE(scan.records.at(0).uncached, 50.0);
        QCOMPARE(scan.records.at(0).cached, 40.0);
        QCOMPARE(scan.records.at(0).cacheWrite, 10.0);
        QCOMPARE(scan.records.at(0).output, 5.0);
    }

    void halfWrittenLineIsIgnoredUntilTheNextScan()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/claude/projects/demo/session.jsonl";
        QVERIFY(writeFile(path, claudeLine("one", 10, 1) + "\n" + claudeLine("two", 4, 2)));
        const QString cache = dir.path() + "/cache";
        HistoryScan first = scanHistory(dir.path() + "/claude", dir.path() + "/codex", cache);
        QCOMPARE(first.records.size(), 1);
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        QVERIFY(file.write("\n") == 1);
        file.close();
        const HistoryScan second = scanHistory(dir.path() + "/claude", dir.path() + "/codex", cache);
        QCOMPARE(second.records.size(), 2);
        QCOMPARE(second.records.at(1).uncached, 4.0);
    }

    void grownFileReadsOnlyTheNewBytes()
    {
        QTemporaryDir dir;
        QByteArray body;
        for (int i = 0; i < 80; ++i)
            body += QByteArray(1000, 'x') + "\n";
        const QByteArray turn = QJsonDocument(QJsonObject{{"type", "turn_context"}, {"timestamp", "2026-08-01T05:17:42.694Z"}, {"payload", QJsonObject{{"model", "gpt-5"}}}}).toJson(QJsonDocument::Compact);
        body += turn + "\n" + codexCount(100, 0, 0, 10, 100, 0, 0, 10, "2026-08-01T05:17:49.919Z") + "\n";
        const QString path = dir.path() + "/codex/sessions/day/session.jsonl";
        QVERIFY(writeFile(path, body));
        const QString cache = dir.path() + "/cache";
        const HistoryScan first = scanHistory(dir.path() + "/claude", dir.path() + "/codex", cache);
        QCOMPARE(first.records.size(), 1);
        QVERIFY(first.bytesRead > 70000);
        const QByteArray extra = codexCount(50, 0, 0, 5, 150, 0, 0, 15, "2026-08-01T05:18:49.919Z") + "\n";
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        QCOMPARE(file.write(extra), qint64(extra.size()));
        file.close();
        const HistoryScan second = scanHistory(dir.path() + "/claude", dir.path() + "/codex", cache);
        QCOMPARE(second.records.size(), 2);
        QCOMPARE(second.records.at(1).output, 5.0);
        QVERIFY(second.bytesRead <= extra.size() + 64);
        QVERIFY(second.bytesRead < first.bytesRead / 2);
        QVERIFY(QFile::exists(cache + "/scan-cache.json"));
        QVERIFY(!QFile::exists(dir.path() + "/codex/scan-cache.json"));
    }

    void oldTranscriptIsSkipped()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/claude/projects/demo/session.jsonl";
        QVERIFY(writeFile(path, claudeLine("one", 10, 1) + "\n"));
        const qint64 now = QDateTime(QDate(2026, 8, 8), QTime(12, 0), QTimeZone("UTC")).toMSecsSinceEpoch();
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::fromMSecsSinceEpoch(now - 91LL * 24 * 60 * 60 * 1000), QFileDevice::FileModificationTime));
        file.close();
        const HistoryScan scan = scanHistory(dir.path() + "/claude", dir.path() + "/codex", dir.path() + "/cache", now);
        QCOMPARE(scan.records.size(), 0);
    }

    void t3CacheWinsOnlyWhenTheVersionIsKnown()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("usage.json");
        QVERIFY(writeFile(path, R"({"version":9,"models":["claude-opus"],"files":{"a":{"p":"claude","r":[[1000,0,0,1,0,0,1,-1]]}}})"));
        QVector<UsageRec> records;
        QVERIFY(readT3UsageCache(path, &records, nullptr));
        QCOMPARE(records.size(), 1);
        QVERIFY(writeFile(path, R"({"version":1,"models":[],"files":{}})"));
        QVERIFY(!readT3UsageCache(path, &records, nullptr));
        QVERIFY(!readT3UsageCache(dir.filePath("missing"), &records, nullptr));
    }

    void sharedCodexSessionCountsOnce()
    {
        QTemporaryDir dir;
        const QByteArray meta = QJsonDocument(QJsonObject{{"type", "session_meta"}, {"timestamp", "2026-08-01T05:00:00.000Z"}, {"payload", QJsonObject{{"id", "same"}}}}).toJson(QJsonDocument::Compact);
        const QByteArray turn = QJsonDocument(QJsonObject{{"type", "turn_context"}, {"timestamp", "2026-08-01T05:17:42.694Z"}, {"payload", QJsonObject{{"model", "gpt-5"}}}}).toJson(QJsonDocument::Compact);
        const QByteArray body = meta + "\n" + turn + "\n" + codexCount(100, 0, 0, 10, 100, 0, 0, 10, "2026-08-07T05:17:49.919Z") + "\n";
        QVERIFY(writeFile(dir.path() + "/codex/sessions/a/one.jsonl", body));
        QVERIFY(writeFile(dir.path() + "/codex/sessions/b/two.jsonl", body));
        const HistoryScan scan = scanHistory(dir.path() + "/claude", dir.path() + "/codex", dir.path() + "/cache");
        const QTimeZone utc("UTC");
        const UsageSummary summary = summarizeUsage(scan.records, {}, makeUsageWindow(7, QDateTime(QDate(2026, 8, 8), QTime(12, 0), utc), utc), utc);
        QCOMPARE(summary.total.records, 1);
        QCOMPARE(summary.total.tokens, 110.0);
    }

    void piReadsAssistantUsageAndReportedCost()
    {
        QTemporaryDir dir;
        const QJsonObject usage{{"input", 225}, {"cacheRead", 15104}, {"cacheWrite", 0}, {"output", 24},
                                {"cost", QJsonObject{{"total", 0.012}}}};
        const QByteArray reply = QJsonDocument(QJsonObject{{"type", "message"}, {"id", "6b2e8a3f"}, {"timestamp", "2026-08-01T09:14:06.978Z"},
            {"message", QJsonObject{{"role", "assistant"}, {"provider", "openai-codex"}, {"model", "gpt-5"}, {"usage", usage}}}}).toJson(QJsonDocument::Compact);
        const QByteArray prompt = QJsonDocument(QJsonObject{{"type", "message"}, {"id", "1"}, {"timestamp", "2026-08-01T09:14:00Z"},
            {"message", QJsonObject{{"role", "user"}, {"usage", QJsonObject{{"input", 9}}}}}}).toJson(QJsonDocument::Compact);
        QVERIFY(writeFile(dir.path() + "/home/.pi/agent/sessions/demo/session.jsonl", prompt + "\n" + reply + "\n"));
        const HistorySources sources = discoverSources(dir.path() + "/home", dir.path() + "/none", dir.path() + "/none",
                                                       dir.path() + "/none", dir.path() + "/none");
        QCOMPARE(sources.pi.size(), 1);
        const HistoryScan scan = scanHistory(sources, dir.path() + "/cache");
        QCOMPARE(scan.records.size(), 1);
        const UsageRec &rec = scan.records.at(0);
        QCOMPARE(rec.provider, QString("pi"));
        QCOMPARE(rec.model, QString("openai-codex/gpt-5"));
        QCOMPARE(rec.uncached, 225.0);
        QCOMPARE(rec.cached, 15104.0);
        QCOMPARE(rec.output, 24.0);
        QCOMPARE(rec.reportedCost, 0.012);
    }

    void grokReadsOneRecordPerPromptAndModel()
    {
        QTemporaryDir dir;
        const auto completed = [](const char *event, double input, double cached, double output, double ticks) {
            const QJsonObject counts{{"inputTokens", input}, {"cachedReadTokens", cached}, {"cacheCreationTokens", 0},
                                     {"outputTokens", output}, {"costUsdTicks", ticks}};
            return QJsonDocument(QJsonObject{{"method", "_x.ai/session/update"}, {"timestamp", 1785575646},
                {"params", QJsonObject{{"_meta", QJsonObject{{"agentTimestampMs", 1785575646500.0}, {"eventId", event}}},
                    {"update", QJsonObject{{"sessionUpdate", "turn_completed"},
                        {"usage", QJsonObject{{"modelUsage", QJsonObject{{"grok-4.7-build", counts}}}}}}}}}}).toJson(QJsonDocument::Compact);
        };
        const QString session = dir.path() + "/grok/sessions/%2Fhome/01a16934";
        QVERIFY(writeFile(session + "/updates.jsonl", completed("e1", 4428836, 4275712, 33551, 8994394000) + "\n"
                                                         + completed("e2", 20602, 0, 375, 100000000) + "\n"));
        QVERIFY(writeFile(session + "/events.jsonl", completed("e9", 1, 0, 1, 1) + "\n"));
        HistorySources sources;
        sources.grok = {dir.path() + "/grok/sessions"};
        const HistoryScan scan = scanHistory(sources, dir.path() + "/cache");
        QCOMPARE(scan.records.size(), 2);
        QCOMPARE(scan.records.at(0).uncached, 153124.0);
        QCOMPARE(scan.records.at(0).cached, 4275712.0);
        QCOMPARE(scan.records.at(0).reportedCost, 0.8994394);
        QCOMPARE(scan.records.at(0).timestampMs, qint64(1785575646500));
        QVERIFY(scan.records.at(0).dedupeKey != scan.records.at(1).dedupeKey);
    }

    void sourcesFindSiblingHomesAndTheSourcesFile()
    {
        QTemporaryDir dir;
        const QString home = dir.path() + "/home";
        QVERIFY(QDir().mkpath(home + "/.claude/projects"));
        QVERIFY(QDir().mkpath(home + "/.claude-work/projects"));
        QVERIFY(QDir().mkpath(home + "/.claude-backup"));
        QVERIFY(QDir().mkpath(home + "/.codex/sessions"));
        QVERIFY(QDir().mkpath(dir.path() + "/elsewhere/codex/sessions"));
        // A shadow Codex home links its sessions back to the shared home.
        QVERIFY(QDir().mkpath(home + "/.codex-shadow"));
        QVERIFY(QFile::link(home + "/.codex/sessions", home + "/.codex-shadow/sessions"));
        QVERIFY(writeFile(home + "/sources", "# extra homes\ncodex ~/../elsewhere/codex\nclaude ~/.claude-work\nbogus line\n"));
        const HistorySources sources = discoverSources(home, home + "/.claude", home + "/.codex", home + "/.grok", home + "/sources");
        QCOMPARE(sources.claude.size(), 2);
        QCOMPARE(sources.codex.size(), 2);
        QVERIFY(sources.codex.contains(QFileInfo(dir.path() + "/elsewhere/codex/sessions").canonicalFilePath()));
        QVERIFY(sources.grok.isEmpty());
    }

    void t3CacheCountsASharedCodexSessionOnce()
    {
        // The same request appears in a session file and in the file that resumed it.
        const QJsonArray row{1785575646000.0, 0, 0, 100, 0, 0, 10, 0, QJsonValue(), QJsonValue(), 0};
        const QJsonArray other{1785575647000.0, 0, 0, 50, 0, 0, 5, 0, QJsonValue(), QJsonValue(), 0};
        const QJsonObject root{{"version", 9}, {"models", QJsonArray{"gpt-5"}}, {"sessions", QJsonArray{"s1"}},
            {"files", QJsonObject{{"/a.jsonl", QJsonObject{{"p", "codex"}, {"r", QJsonArray{row}}, {"t", QJsonArray{}}}},
                                  {"/b.jsonl", QJsonObject{{"p", "codex"}, {"r", QJsonArray{row, other}}, {"t", QJsonArray{}}}}}}};
        const QVector<UsageRec> records = parseUsageCache(root);
        QCOMPARE(records.size(), 3);
        const QTimeZone utc("UTC");
        const UsageSummary summary = summarizeUsage(records, {}, makeUsageWindow(7, QDateTime(QDate(2026, 8, 1), QTime(12, 0), utc), utc), utc);
        QCOMPARE(summary.total.records, 2);
        QCOMPARE(summary.total.tokens, 165.0);
    }

    void rateFallbackUsesDownloadThenCacheThenBundle()
    {
        QTemporaryDir dir;
        const QString bundle = dir.filePath("bundle.json");
        QVERIFY(writeFile(bundle, R"({"claude-opus":{"input_cost_per_token":0.001,"output_cost_per_token":0.002}})"));
        const QString cache = dir.filePath("cache");
        const qint64 now = 10'000'000'000;
        bool fetched = false;
        const RateLoad bundled = loadModelRates(cache, bundle, now, {});
        QCOMPARE(bundled.origin, QString("bundled"));
        QCOMPARE(bundled.rates.value("claude-opus").standard.input, 0.001);

        const QJsonObject cachedDoc{{"claude-opus", QJsonObject{{"input_cost_per_token", 0.009}, {"output_cost_per_token", 0.002}}}};
        QVERIFY(storeRateDocument(cache, cachedDoc, now - 1000));
        const RateLoad fresh = loadModelRates(cache, bundle, now, [&] {
            fetched = true;
            return QByteArray();
        });
        QVERIFY(!fetched);
        QCOMPARE(fresh.origin, QString("cache"));
        QCOMPARE(fresh.rates.value("claude-opus").standard.input, 0.009);

        QVERIFY(storeRateDocument(cache, cachedDoc, now - 3LL * 24 * 60 * 60 * 1000));
        const RateLoad stale = loadModelRates(cache, bundle, now, [] { return QByteArray("nope"); });
        QCOMPARE(stale.origin, QString("cache"));
        QCOMPARE(stale.rates.value("claude-opus").standard.input, 0.009);

        QFile::remove(cache + "/model-prices.json");
        const RateLoad downloaded = loadModelRates(cache, bundle, now, [] {
            return QByteArray(R"({"claude-opus":{"input_cost_per_token":0.05,"output_cost_per_token":0.02}})");
        });
        QCOMPARE(downloaded.origin, QString("download"));
        QCOMPARE(downloaded.rates.value("claude-opus").standard.input, 0.05);
        QVERIFY(QFile::exists(cache + "/model-prices.json"));

        QFile::remove(cache + "/model-prices.json");
        const RateLoad none = loadModelRates(cache, dir.filePath("missing.json"), now, [] { return QByteArray(); });
        QCOMPARE(none.origin, QString());
        QVERIFY(none.rates.isEmpty());

        const QHash<QString, ModelRate> builtIn = readRateFile(QStringLiteral(":/model-prices.json"));
        QVERIFY(builtIn.contains("claude-opus-5-5"));
        QVERIFY(builtIn.contains("gpt-6.1-sol"));
        QVERIFY(builtIn.value("claude-opus-5-5").standard.input > 0);
    }

private:
};

QTEST_MAIN(QuotaTests)
#include "quota_tests.moc"
