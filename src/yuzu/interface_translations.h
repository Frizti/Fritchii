// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QTranslator>

class InterfaceTranslations {
public:
    void Load(const QString& language);

private:
    QTranslator qt;
    QTranslator frontend;
};
