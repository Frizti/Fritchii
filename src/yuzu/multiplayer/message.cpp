// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QMessageBox>
#include <QString>

#include "yuzu/multiplayer/message.h"

namespace NetworkMessage {
const ConnectionError ErrorManager::USERNAME_NOT_VALID(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Username is not valid. Must be 4 to 20 alphanumeric characters."));
const ConnectionError ErrorManager::ROOMNAME_NOT_VALID(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Room name is not valid. Must be 4 to 20 alphanumeric characters."));
const ConnectionError ErrorManager::USERNAME_NOT_VALID_SERVER(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Username is already in use or not valid. Please choose another."));
const ConnectionError ErrorManager::IP_ADDRESS_NOT_VALID(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "IP is not a valid IPv4 address."));
const ConnectionError ErrorManager::PORT_NOT_VALID(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Port must be a number between 0 to 65535."));
const ConnectionError ErrorManager::GAME_NOT_SELECTED(QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager",
    "You must choose a Preferred Game to host a room. If you do not have any games in your game "
    "list yet, add a game folder by clicking on the plus icon in the game list."));
const ConnectionError ErrorManager::NO_INTERNET(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Unable to find an internet connection. Check your internet settings."));
const ConnectionError ErrorManager::UNABLE_TO_CONNECT(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Unable to connect to the host. Verify that the connection settings are correct. If "
               "you still cannot connect, contact the room host and verify that the host is "
               "properly configured with the external port forwarded."));
const ConnectionError ErrorManager::ROOM_IS_FULL(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Unable to connect to the room because it is already full."));
const ConnectionError ErrorManager::COULD_NOT_CREATE_ROOM(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Creating a room failed. Please retry. Restarting Fritchii might be necessary."));
const ConnectionError ErrorManager::HOST_BANNED(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "The host of the room has banned you. Speak with the host to unban you "
               "or try a different room."));
const ConnectionError ErrorManager::WRONG_VERSION(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Version mismatch! Please update to the latest version of Fritchii. If the problem "
               "persists, contact the room host and ask them to update the server."));
const ConnectionError ErrorManager::WRONG_PASSWORD(QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Incorrect password."));
const ConnectionError ErrorManager::GENERIC_ERROR(QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager",
    "An unknown error occurred. If this error continues to occur, please open an issue"));
const ConnectionError ErrorManager::LOST_CONNECTION(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "Connection to room lost. Try to reconnect."));
const ConnectionError ErrorManager::HOST_KICKED(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "You have been kicked by the room host."));
const ConnectionError ErrorManager::IP_COLLISION(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "IP address is already in use. Please choose another."));
const ConnectionError ErrorManager::PERMISSION_DENIED(
    QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager", "You do not have enough permission to perform this action."));
const ConnectionError ErrorManager::NO_SUCH_USER(QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager",
    "The user you are trying to kick/ban could not be found.\nThey may have left the room."));
const ConnectionError ErrorManager::NO_INTERFACE_SELECTED(QT_TRANSLATE_NOOP("NetworkMessage::ErrorManager",
    "No valid network interface is selected.\nPlease go to Configure -> System -> Network and "
    "make a selection."));

static bool WarnMessage(const std::string& title, const std::string& text) {
    return QMessageBox::Ok == QMessageBox::warning(nullptr, QObject::tr(title.c_str()),
                                                   QObject::tr(text.c_str()),
                                                   QMessageBox::Ok | QMessageBox::Cancel);
}

void ErrorManager::ShowError(const ConnectionError& e) {
    QMessageBox::critical(nullptr, tr("Error"), tr(e.GetString().c_str()));
}

bool WarnGameRunning() {
    return WarnMessage(
        QT_TRANSLATE_NOOP("QObject", "Game already running"),
        QT_TRANSLATE_NOOP("QObject", "Joining a room when the game is already running is discouraged "
                   "and can cause the room feature not to work correctly.\nProceed anyway?"));
}

bool WarnCloseRoom() {
    return WarnMessage(
        QT_TRANSLATE_NOOP("QObject", "Leave Room"),
        QT_TRANSLATE_NOOP("QObject", "You are about to close the room. Any network connections will be closed."));
}

bool WarnDisconnect() {
    return WarnMessage(
        QT_TRANSLATE_NOOP("QObject", "Disconnect"),
        QT_TRANSLATE_NOOP("QObject", "You are about to leave the room. Any network connections will be closed."));
}

} // namespace NetworkMessage
