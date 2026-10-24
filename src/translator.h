#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonValue>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

// Follows the language picked under Omarchy Setup > Language.
// Catalogs are flat JSON objects from English text to a translation.
class Translator : public QTranslator {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY changed)

public:
    explicit Translator(const QString &languageFile = {}, const QString &catalogDirectory = ":/i18n",
                        QObject *parent = nullptr);

    QString language() const { return m_language; }
    bool isEmpty() const override { return m_strings.isEmpty(); }
    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr,
                      int n = -1) const override;

    static QString readLanguage(const QString &path);
    static QString pluralCategory(const QString &language, long long count);
    static QLocale formatLocale(const QString &language);

signals:
    void changed();

private:
    void reload();

    QString m_languageFile;
    QString m_catalogDirectory;
    QString m_language;
    QHash<QString, QJsonValue> m_strings;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
