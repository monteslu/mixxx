#pragma once

#include <QString>
#include <memory>

#include "control/pollingcontrolproxy.h"
#include "engine/sync/syncable.h"
#include "preferences/configobject.h"

class ControlObject;
class ControlPushButton;
class EngineSync;

/// SmartFaderControl maps the crossfader position to BPM interpolation
/// between two decks. When enabled, both decks play at the same tempo
/// which smoothly transitions from the left deck's BPM to the right
/// deck's BPM as the crossfader moves. The InternalClock is used as
/// the sync leader, and both decks follow it.
class SmartFaderControl {
  public:
    SmartFaderControl(const QString& group, EngineSync* pEngineSync);
    ~SmartFaderControl();

    /// Called every audio callback from EngineMixer::processChannels().
    void process();

  private:
    void activate();
    void deactivate();

    /// Directly update InternalClock BPM and synchronously propagate to followers.
    void setLeaderBpmDirect(double bpm);

    /// Normalize BPMs for half/double relationships (e.g., 70 vs 140).
    static double normalizeBpmMultiplier(double myBpm, double targetBpm);

    EngineSync* m_pEngineSync;

    // Controls exposed to controllers/QML
    std::unique_ptr<ControlPushButton> m_pEnabled;
    std::unique_ptr<ControlObject> m_pActive;
    std::unique_ptr<ControlObject> m_pLeftBpm;
    std::unique_ptr<ControlObject> m_pRightBpm;
    std::unique_ptr<ControlObject> m_pTargetBpm;

    // Controls we read from
    PollingControlProxy m_crossfader;

    // State
    double m_capturedLeftBpm;
    double m_capturedRightBpm;
    bool m_bWasActive;

    // Saved sync modes for restoration on deactivate
    SyncMode m_savedLeftSyncMode;
    SyncMode m_savedRightSyncMode;
    SyncMode m_savedInternalClockSyncMode;
};
