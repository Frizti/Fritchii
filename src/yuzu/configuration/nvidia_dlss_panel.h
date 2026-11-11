// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Shown under the NVIDIA DLSS setting: which of NVIDIA's files were found, and
// a button that fetches missing ones from NVIDIA's own Streamline release.

#pragma once

#ifdef EGLANTINE_STREAMLINE

#include <QWidget>

class QLabel;
class QPushButton;

class NvidiaDlssPanel : public QWidget {
public:
    explicit NvidiaDlssPanel(QWidget* parent);

    void Refresh();

    /// Fetches NVIDIA's missing files from the Streamline release, with DLSS Frame Generation's
    /// when it is asked for (LICHII_DLSS_FG). True when nothing is missing afterwards.
    bool Download();

    /// Whether DLSS, or DLSS Frame Generation with Reflex, still lacks one of NVIDIA's files.
    [[nodiscard]] static bool FrameGenFilesMissing();

protected:
    void showEvent(QShowEvent* event) override;

private:

    QLabel* icon_label;
    QLabel* status_label;
    QPushButton* download_button;
    QPushButton* folder_button;
};

#endif
