#pragma once

#include <QList>
#include <QObject>
#include <QUrl>
#include <QVariant>
#include <memory>

#include "analyzer/trackanalysisscheduler.h"
#include "control/controlobject.h"
#include "library/libraryfeature.h"
#include "library/treeitemmodel.h"
#include "preferences/usersettings.h"
#include "util/parented_ptr.h"

class DlgAnalysis;

class AnalysisFeature : public LibraryFeature {
    Q_OBJECT
  public:
    AnalysisFeature(Library* pLibrary,
                    UserSettingsPointer pConfig);
    ~AnalysisFeature() override;

    QVariant title() override {
        return m_title;
    }

    bool dropAccept(const QList<QUrl>& urls, QObject* pSource) override;
    bool dragMoveAccept(const QList<QUrl>& urls) override;
    void bindLibraryWidget(WLibrary* libraryWidget,
                    KeyboardEventFilter* keyboard) override;

    TreeItemModel* sidebarModel() const override;
    void refreshLibraryModels();

  signals:
    void analysisActive(bool bActive);
    void trackProgress(TrackId trackId, AnalyzerProgress progress);

  public slots:
    void activate() override;
    void analyzeTracks(const QList<AnalyzerScheduledTrack>& tracks);

    void suspendAnalysis();
    void resumeAnalysis();
    void stopAnalysis();

  private slots:
    void onTrackAnalysisSchedulerProgress(AnalyzerProgress currentTrackProgress, int currentTrackNumber, int totalTracksCount);
    void onTrackAnalysisSchedulerFinished();

  private:
    // Sets the title of this feature to the default name, given by
    // m_sAnalysisTitleName
    void resetTitle();

    // Sets the title of this feature to the default name followed by (x / y)
    // where x is the current track being analyzed and y is the total number of
    // tracks in the job
    void setTitleProgress(int currentTrackNumber, int totalTracksCount);

    const QString m_baseTitle;

    TrackAnalysisScheduler::Pointer m_pTrackAnalysisScheduler;

    // Global 0-100 progress of the currently running AI stem separation, set by
    // AnalyzerStemSeparation and read by skins to show an "unmissable" readout.
    std::unique_ptr<ControlObject> m_pStemSeparationProgress;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    DlgAnalysis* m_pAnalysisView;

    // The title is dynamic and reflects the current progress
    QString m_title;
};
