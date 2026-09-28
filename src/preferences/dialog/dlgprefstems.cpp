#include "preferences/dialog/dlgprefstems.h"

#ifdef __STEMSEP_ONNX__

#include <QCheckBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "moc_dlgprefstems.cpp"

namespace {
const ConfigKey kEnabledKey("[Library]", "StemSeparationEnabled");
const ConfigKey kModelKey("[Library]", "StemSeparationModelPath");
const ConfigKey kFfmpegKey("[Library]", "StemSeparationFfmpegPath");
constexpr char kDefaultFfmpeg[] = "ffmpeg";
// Published HTDemucs ONNX export (fp16, ~166 MB).
constexpr char kModelDownloadUrl[] =
        "https://huggingface.co/StemSplitio/htdemucs-onnx/resolve/main/"
        "htdemucs_fp16weights.onnx";

// Small helper: a line edit paired with a button on its right.
QHBoxLayout* rowWithButton(QLineEdit* pEdit, QPushButton* pButton) {
    auto* pRow = new QHBoxLayout();
    pRow->addWidget(pEdit);
    pRow->addWidget(pButton);
    return pRow;
}
} // anonymous namespace

DlgPrefStems::DlgPrefStems(QWidget* pParent, UserSettingsPointer pConfig)
        : DlgPreferencePage(pParent),
          m_pConfig(pConfig) {
    auto* pLayout = new QVBoxLayout(this);

    auto* pIntro = new QLabel(
            tr("Split tracks into 4 stems (vocals, drums, bass, other) using a "
               "local AI model. Separation runs once per track and is cached, "
               "so a track loads instantly the next time. Requires a 44.1 kHz "
               "stereo track, the Demucs ONNX model (~166 MB) and FFmpeg."),
            this);
    pIntro->setWordWrap(true);
    pLayout->addWidget(pIntro);

    auto* pForm = new QFormLayout();

    m_pEnableOnImport = new QCheckBox(
            tr("Separate stems automatically when analyzing tracks"), this);
    pForm->addRow(m_pEnableOnImport);

    m_pModelPath = new QLineEdit(this);
    auto* pBrowseModel = new QPushButton(tr("Browse…"), this);
    auto* pDownloadModel = new QPushButton(tr("Download model…"), this);
    auto* pModelRow = rowWithButton(m_pModelPath, pBrowseModel);
    pModelRow->addWidget(pDownloadModel);
    pForm->addRow(tr("Demucs ONNX model:"), pModelRow);

    m_pFfmpegPath = new QLineEdit(this);
    auto* pBrowseFfmpeg = new QPushButton(tr("Browse…"), this);
    pForm->addRow(tr("FFmpeg executable:"), rowWithButton(m_pFfmpegPath, pBrowseFfmpeg));

    pLayout->addLayout(pForm);
    pLayout->addStretch();

    connect(pBrowseModel, &QPushButton::clicked, this, &DlgPrefStems::slotBrowseModel);
    connect(pBrowseFfmpeg, &QPushButton::clicked, this, &DlgPrefStems::slotBrowseFfmpeg);
    connect(pDownloadModel, &QPushButton::clicked, this, &DlgPrefStems::slotDownloadModel);

    slotUpdate();
}

void DlgPrefStems::slotUpdate() {
    m_pEnableOnImport->setChecked(m_pConfig->getValue(kEnabledKey, false));
    m_pModelPath->setText(m_pConfig->getValue(kModelKey, QString()));
    m_pFfmpegPath->setText(
            m_pConfig->getValue(kFfmpegKey, QString::fromUtf8(kDefaultFfmpeg)));
}

void DlgPrefStems::slotApply() {
    m_pConfig->setValue(kEnabledKey, m_pEnableOnImport->isChecked());
    m_pConfig->setValue(kModelKey, m_pModelPath->text());
    m_pConfig->setValue(kFfmpegKey, m_pFfmpegPath->text());
}

void DlgPrefStems::slotResetToDefaults() {
    m_pEnableOnImport->setChecked(false);
    m_pModelPath->clear();
    m_pFfmpegPath->setText(QString::fromUtf8(kDefaultFfmpeg));
}

void DlgPrefStems::slotBrowseModel() {
    const QString path = QFileDialog::getOpenFileName(this,
            tr("Select Demucs ONNX model"),
            m_pModelPath->text(),
            tr("ONNX model (*.onnx)"));
    if (!path.isEmpty()) {
        m_pModelPath->setText(path);
    }
}

void DlgPrefStems::slotBrowseFfmpeg() {
    const QString path = QFileDialog::getOpenFileName(this,
            tr("Select FFmpeg executable"),
            m_pFfmpegPath->text());
    if (!path.isEmpty()) {
        m_pFfmpegPath->setText(path);
    }
}

void DlgPrefStems::slotDownloadModel() {
    // ponytail: open the model in the browser to download, then the user points
    // the path at it. A built-in downloader with a progress bar can replace this
    // later, but this keeps setup dependency-free and robust.
    QDesktopServices::openUrl(QUrl(QString::fromUtf8(kModelDownloadUrl)));
}

#endif // __STEMSEP_ONNX__
