#ifndef APP_STAGE_COREGISTER_WITH_MAT_H
#define APP_STAGE_COREGISTER_WITH_MAT_H

//-- includes -----
#include "ClientGeometry_CAPI.h"
#include "ClientConstants.h"
#include <chrono>
#include <string.h>  // Required for memset in Xcode

//-- definitions -----
class AppSubStage_CalibrateWithMat
{
public:
    enum eMenuState
    {
        invalid,

        initial,
        calibrationStepPlaceController,
        calibrationStepRecordController,
        calibrationStepPlaceHMD,
        calibrationStepRecordHMD,
        calibrationStepComputeTrackerPoses,

        calibrateStepSuccess,
        calibrateStepFailed,
    };

	enum ePaperFormat
	{
		MIN_PAPER_FORMATS = -1,

		formatLetter,
		formatA4,
		formatA3,
		formatQuadLetter,
		formatQuadA4,
		formatQuadA3,

		MAX_PAPER_FORMATS
	};
	ePaperFormat m_iPaperFormat;

	static const char *AppSubStage_CalibrateWithMat::k_paper_formats_names[AppSubStage_CalibrateWithMat::ePaperFormat::MAX_PAPER_FORMATS];
	 
    AppSubStage_CalibrateWithMat(class AppStage_ComputeTrackerPoses *parentStage);
	virtual ~AppSubStage_CalibrateWithMat();

    void enter();
    void exit();
    void update();
    void render();

    void renderUI();

    inline eMenuState getMenuState() const
    {
        return m_menuState;
    }

protected:
    void setState(eMenuState newState);
    void onExitState(eMenuState newState);
    void onEnterState(eMenuState newState);

private:
    class AppStage_ComputeTrackerPoses *m_parentStage;
    eMenuState m_menuState;

    std::chrono::time_point<std::chrono::high_resolution_clock> m_stableStartTime;
    bool m_bIsStable;
    bool m_bForceStable;

	struct TrackerRelativePoseStatistics *m_deviceTrackerPoseStats[PSMOVESERVICE_MAX_TRACKER_COUNT];

    int m_sampleTrackerId;
    int m_sampleLocationIndex;
    bool m_bNeedMoreSamplesAtLocation;
	int m_iLightFlicker;
};

#endif // APP_STAGE_COREGISTER_WITH_MAT_H