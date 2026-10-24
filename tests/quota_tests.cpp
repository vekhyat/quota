#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include "theme.h"
#include "translator.h"
#include "usage.h"

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

private:
};

QTEST_MAIN(QuotaTests)
#include "quota_tests.moc"
