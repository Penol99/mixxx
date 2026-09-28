#pragma once

#ifdef __STEMSEP_ONNX__

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/usersettings.h"

class QCheckBox;
class QLineEdit;

/// Preferences pane for offline AI stem separation (Demucs/ONNX).
/// Keeps setup out of the config file: model path, ffmpeg path, and whether to
/// separate automatically on import.
class DlgPrefStems : public DlgPreferencePage {
    Q_OBJECT
  public:
    DlgPrefStems(QWidget* pParent, UserSettingsPointer pConfig);
    ~DlgPrefStems() override = default;

  public slots:
    void slotUpdate() override;
    void slotApply() override;
    void slotResetToDefaults() override;

  private slots:
    void slotBrowseModel();
    void slotBrowseFfmpeg();
    void slotDownloadModel();

  private:
    UserSettingsPointer m_pConfig;
    QCheckBox* m_pEnableOnImport;
    QLineEdit* m_pModelPath;
    QLineEdit* m_pFfmpegPath;
};

#endif // __STEMSEP_ONNX__
