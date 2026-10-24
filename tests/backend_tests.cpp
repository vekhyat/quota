#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include "backend.h"
#include "history.h"

class BackendTests : public QObject {
    Q_OBJECT
private slots:
    void independentCodexProfiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // This executable runs separately so HOME and provider overrides never touch real accounts.
        qputenv("HOME", dir.path().toUtf8());
        qputenv("QUOTA_CODEX_HOME", (dir.path() + "/.codex").toUtf8());
        qputenv("QUOTA_CLAUDE_DIR", (dir.path() + "/claude").toUtf8());
        qputenv("QUOTA_GROK_HOME", (dir.path() + "/grok").toUtf8());
        qputenv("QUOTA_T3_HOME", (dir.path() + "/t3").toUtf8());
        qputenv("QUOTA_CACHE_DIR", (dir.path() + "/cache").toUtf8());
        qputenv("QUOTA_SOURCES_FILE", (dir.path() + "/sources").toUtf8());
        qputenv("QUOTA_CLAUDE_VERSION", "2.1.310");
        const QString personal = dir.path() + "/.codex";
        const QString work = dir.path() + "/work account";
        QVERIFY(QDir().mkpath(personal));
        QVERIFY(QDir().mkpath(work));
        QFile sources(dir.path() + "/sources");
        QVERIFY(sources.open(QIODevice::WriteOnly));
        sources.write(("codex " + work + "\n").toUtf8());
        sources.close();
        QVERIFY(storeRateDocument(dir.path() + "/cache",
            QJsonObject{{"test", QJsonObject{{"input_cost_per_token", 0.001}, {"output_cost_per_token", 0.002}}}}, QDateTime::currentMSecsSinceEpoch()));
        QFile fake(dir.path() + "/fake-codex");
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write(R"SH(#!/bin/sh
read -r hello
printf '%s\n' '{"id":1,"result":{}}'
read -r initialized
read -r request
if [ -f "$CODEX_HOME/fail" ]; then
    printf '%s\n' '{"id":2,"error":{"message":"expired"}}'
else
    case "$CODEX_HOME" in */.codex) used=12;; *) used=73;; esac
    printf '{"id":2,"result":{"rateLimits":{"primary":{"usedPercent":%s}}}}\n' "$used"
fi
)SH");
        fake.close();
        QVERIFY(fake.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("QUOTA_CODEX_BIN", fake.fileName().toUtf8());
        Backend backend;
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 5000);
        auto limits = backend.limits();
        QCOMPARE(limits.size(), 2);
        QMap<QString, double> used;
        for (const auto &value : limits)
            used.insert(value.toMap().value("account").toString(), value.toMap().value("used").toDouble());
        QCOMPARE(used.value(personal), 12.0);
        QCOMPARE(used.value(work), 73.0);
        QFile fail(work + "/fail");
        QVERIFY(fail.open(QIODevice::WriteOnly));
        fail.close();
        backend.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 5000);
        QCOMPARE(backend.limits().size(), 1);
        QCOMPARE(backend.limits().first().toMap().value("account").toString(), personal);
        QVERIFY(backend.notices().contains("codex-failed:" + work));
        QVERIFY(fail.remove());
        backend.refresh();
        backend.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 5000);
        QCOMPARE(backend.limits().size(), 2);
        QVERIFY(!backend.notices().contains("codex-failed:" + work));
    }
};
QTEST_GUILESS_MAIN(BackendTests)
#include "backend_tests.moc"
