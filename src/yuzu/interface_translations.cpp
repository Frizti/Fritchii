// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/interface_translations.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>

void InterfaceTranslations::Load(const QString& language) {
    QCoreApplication::removeTranslator(&frontend);
    QCoreApplication::removeTranslator(&qt);
    const QLocale locale = language.isEmpty() ? QLocale::system() : QLocale(language);
    const QString path = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qt.load(locale, QStringLiteral("qt"), QStringLiteral("_"), path) ||
        qt.load(locale, QStringLiteral("qtbase"), QStringLiteral("_"), path)) {
        QCoreApplication::installTranslator(&qt);
    }
    if (frontend.load(locale, {}, {}, QStringLiteral(":/languages/"))) {
        QCoreApplication::installTranslator(&frontend);
    }
}
