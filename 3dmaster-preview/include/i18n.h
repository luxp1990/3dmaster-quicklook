#pragma once

#include <QCoreApplication>
#include <QFile>
#include <QLocale>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <atomic>

namespace master_i18n {

enum class Lang : int {
    Zh = 0,
    En = 1
};

inline std::atomic<int>& langStorage() {
    static std::atomic<int> s_lang{static_cast<int>(
        QLocale::system().language() == QLocale::Chinese ? Lang::Zh : Lang::En)};
    return s_lang;
}

inline Lang currentLang() {
    return static_cast<Lang>(langStorage().load(std::memory_order_relaxed));
}

inline bool isZh() {
    return currentLang() == Lang::Zh;
}

inline void setLanguage(Lang lang) {
    langStorage().store(static_cast<int>(lang), std::memory_order_relaxed);
}

inline Lang parseLanguageCode(const QString& raw) {
    const QString code = raw.trimmed().toLower();
    if (code.startsWith("zh") || code.contains("chinese")) {
        return Lang::Zh;
    }
    // sc/tc 只认宿主单独传来的简繁代码，避免 en-SC、sco 这类子串误判
    if (code == "sc" || code == "tc") {
        return Lang::Zh;
    }
    // zh-Hans / zh-Hant 已由 zh 前缀覆盖；这里兜单独传脚本代码的情况
    const QStringList parts = QString(code).replace('_', '-').split('-', Qt::SkipEmptyParts);
    if (parts.contains("hans") || parts.contains("hant")) {
        return Lang::Zh;
    }
    return Lang::En;
}

inline void initLanguage(const QString& iniPath = QString(), const QString& explicitCode = QString()) {
    // 1. Explicit override (e.g. CLI --lang from QuickLook)
    const QString trimmedExplicit = explicitCode.trimmed();
    if (!trimmedExplicit.isEmpty() && trimmedExplicit.compare("auto", Qt::CaseInsensitive) != 0) {
        setLanguage(parseLanguageCode(trimmedExplicit));
        return;
    }

    // 2. Local 3dmaster.ini override ([General] Language=zh|en|auto)
    if (!iniPath.isEmpty() && QFile::exists(iniPath)) {
        QSettings ini(iniPath, QSettings::IniFormat);
        const QString cfgLang = ini.value("General/Language", "auto").toString().trimmed();
        if (!cfgLang.isEmpty() && cfgLang.compare("auto", Qt::CaseInsensitive) != 0) {
            setLanguage(parseLanguageCode(cfgLang));
            return;
        }
    }

    // 3. Fallback to OS / Qt system locale
    setLanguage(QLocale::system().language() == QLocale::Chinese ? Lang::Zh : Lang::En);
}

inline QString tr(const char* zh, const char* en) {
    return QString::fromUtf8(isZh() ? zh : en);
}

} // namespace master_i18n
