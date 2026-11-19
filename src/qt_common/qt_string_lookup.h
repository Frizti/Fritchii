// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include "frozen/map.h"
#include "frozen/string.h"

/// Small helper to look up enums.
/// res = the result code
/// base = the base matching value in the StringKey table
#define LOOKUP_ENUM(res, base)                                                                     \
    QtCommon::StringLookup::Lookup(                                                                \
        QtCommon::StringLookup::StringKey((int)res + (int)QtCommon::StringLookup::base))

namespace QtCommon::StringLookup {

Q_NAMESPACE

// TODO: QML interface
enum StringKey {
    DataManagerSavesTooltip,
    DataManagerShadersTooltip,
    DataManagerUserNandTooltip,
    DataManagerSysNandTooltip,
    DataManagerModsTooltip,

    // Key install results
    KeyInstallSuccess,
    KeyInstallInvalidDir,
    KeyInstallErrorFailedCopy,
    KeyInstallErrorWrongFilename,
    KeyInstallErrorFailedInit,

    // Firmware install results
    FwInstallSuccess,
    FwInstallNoNCAs,
    FwInstallFailedDelete,
    FwInstallFailedCopy,
    FwInstallFailedCorrupted,

    // Firmware Check results
    FwCheckErrorFirmwareMissing,
    FwCheckErrorFirmwareCorrupted,

    // user data migrator
    MigrationPromptPrefix,
    MigrationPrompt,
    MigrationTooltipClearShader,
    MigrationTooltipKeepOld,
    MigrationTooltipClearOld,
    MigrationTooltipLinkOld,

    // ryujinx
    KvdbNonexistent,
    KvdbNoHeader,
    KvdbInvalidMagic,
    KvdbMisaligned,
    KvdbNoImens,
    RyujinxNoSaveId,
};

// NB: the constexpr check always succeeds (in clangd at least) if size arg < size
// always triple-check the size arg
static const constexpr frozen::map<StringKey, frozen::string, 29> strings = {
    // 0-4
    {DataManagerSavesTooltip,
     QT_TRANSLATE_NOOP("QObject", "Contains game save data. DO NOT REMOVE UNLESS YOU KNOW WHAT YOU'RE DOING!")},
    {DataManagerShadersTooltip,
     QT_TRANSLATE_NOOP("QObject", "Contains Vulkan and OpenGL pipeline caches. Generally safe to remove.")},
    {DataManagerUserNandTooltip, QT_TRANSLATE_NOOP("QObject", "Contains updates and DLC for games.")},
    {DataManagerSysNandTooltip, QT_TRANSLATE_NOOP("QObject", "Contains firmware and applet data.")},
    {DataManagerModsTooltip, QT_TRANSLATE_NOOP("QObject", "Contains game mods, patches, and cheats.")},

    // Key install
    // 5-9
    {KeyInstallSuccess, QT_TRANSLATE_NOOP("QObject", "Decryption Keys were successfully installed")},
    {KeyInstallInvalidDir, QT_TRANSLATE_NOOP("QObject", "Unable to read key directory, aborting")},
    {KeyInstallErrorFailedCopy, QT_TRANSLATE_NOOP("QObject", "One or more keys failed to copy.")},
    {KeyInstallErrorWrongFilename,
     QT_TRANSLATE_NOOP("QObject", "Verify your keys file has a .keys extension and try again.")},
    {KeyInstallErrorFailedInit,
     QT_TRANSLATE_NOOP("QObject",
         "Decryption Keys failed to initialize. Check that your dumping tools are up to date and "
         "re-dump keys.")},

    // fw install
    // 10-14
    {FwInstallSuccess, QT_TRANSLATE_NOOP("QObject", "Successfully installed firmware version %1")},
    {FwInstallNoNCAs, QT_TRANSLATE_NOOP("QObject", "Unable to locate potential firmware NCA files")},
    {FwInstallFailedDelete, QT_TRANSLATE_NOOP("QObject", "Failed to delete one or more firmware files.")},
    {FwInstallFailedCopy, QT_TRANSLATE_NOOP("QObject", "One or more firmware files failed to copy into NAND.")},
    {FwInstallFailedCorrupted,
     QT_TRANSLATE_NOOP("QObject",
         "Firmware installation cancelled, firmware may be in a bad state or corrupted. Restart "
         "Fritchii or re-install firmware.")},

    {FwCheckErrorFirmwareMissing,
     QT_TRANSLATE_NOOP("QObject",
         "Firmware missing. Firmware is required to run certain games and use the Home Menu.")},
    {FwCheckErrorFirmwareCorrupted,
     QT_TRANSLATE_NOOP("QObject",
         "Firmware reported as present, but was unable to be read. Check for decryption keys and "
         "redump firmware if necessary.")},

    // migrator
    // 17-22
    {MigrationPromptPrefix,
     QT_TRANSLATE_NOOP("QObject", "Fritchii has detected user data for the following emulators:")},
    {MigrationPrompt,
     QT_TRANSLATE_NOOP("QObject", "Would you like to migrate your data for use in Fritchii?\n"
                "Select the corresponding button to migrate data from that emulator.\n"
                "This may take a while.")},
    {MigrationTooltipClearShader, QT_TRANSLATE_NOOP("QObject", "Clearing shader cache is recommended when migrating "
                                             "from another emulator.\nEarlier builds kept "
                                             "their own cache in an older data folder.")},
    {MigrationTooltipKeepOld,
     QT_TRANSLATE_NOOP("QObject", "Keeps the old data directory. This is recommended if you aren't\n"
                "space-constrained and want to keep separate data for the old emulator.")},
    {MigrationTooltipClearOld, QT_TRANSLATE_NOOP("QObject", "Copies and verifies the full profile before moving the "
                                          "source aside as a recovery backup.\nRequires enough "
                                          "free space for a full copy. Keep the backup until "
                                          "you have checked your saves.")},
    {MigrationTooltipLinkOld,
     QT_TRANSLATE_NOOP("QObject", "Creates a filesystem link between the old directory and the Fritchii directory.\n"
                "This is recommended if you want to share data between emulators.")},

    // why am I writing these comments again
    // 23-28
    {KvdbNonexistent, QT_TRANSLATE_NOOP("QObject", "Ryujinx title database does not exist.")},
    {KvdbNoHeader, QT_TRANSLATE_NOOP("QObject", "Invalid header on Ryujinx title database.")},
    {KvdbInvalidMagic, QT_TRANSLATE_NOOP("QObject", "Invalid magic header on Ryujinx title database.")},
    {KvdbMisaligned, QT_TRANSLATE_NOOP("QObject", "Invalid byte alignment on Ryujinx title database.")},
    {KvdbNoImens, QT_TRANSLATE_NOOP("QObject", "No items found in Ryujinx title database.")},
    {RyujinxNoSaveId, QT_TRANSLATE_NOOP("QObject", "Title %1 not found in Ryujinx title database.")},
};

static inline const QString Lookup(StringKey key) {
    return QObject::tr(strings.at(key).data());
}

} // namespace QtCommon::StringLookup
