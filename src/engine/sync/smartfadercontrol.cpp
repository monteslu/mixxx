#include "engine/sync/smartfadercontrol.h"

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "engine/sync/enginesync.h"
#include "engine/sync/internalclock.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("SmartFaderControl");

const QString kLeftDeckGroup = QStringLiteral("[Channel1]");
const QString kRightDeckGroup = QStringLiteral("[Channel2]");

// Threshold for re-publishing the per-deck BPM controls (UI smoothing).
constexpr double kBpmChangeThreshold = 0.5;

constexpr double kUnityFactor = 1.0;
} // namespace

SmartFaderControl::SmartFaderControl(
        const QString& group, EngineSync* pEngineSync)
        : m_pEngineSync(pEngineSync),
          m_pEnabled(std::make_unique<ControlPushButton>(
                  ConfigKey(group, "smart_fader_enabled"),
                  true /*persist*/)),
          m_pActive(std::make_unique<ControlObject>(
                  ConfigKey(group, "smart_fader_active"), false, false, false)),
          m_pLeftBpm(std::make_unique<ControlObject>(
                  ConfigKey(group, "smart_fader_left_bpm"), false, false, false)),
          m_pRightBpm(std::make_unique<ControlObject>(
                  ConfigKey(group, "smart_fader_right_bpm"), false, false, false)),
          m_pTargetBpm(std::make_unique<ControlObject>(
                  ConfigKey(group, "smart_fader_target_bpm"), false, false, false)),
          m_crossfader(ConfigKey(group, "crossfader")),
          m_lastLeftFileBpm(0.0),
          m_lastRightFileBpm(0.0),
          m_bWasActive(false),
          m_savedLeftSyncMode(SyncMode::None),
          m_savedRightSyncMode(SyncMode::None),
          m_savedInternalClockSyncMode(SyncMode::None) {
    m_pEnabled->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pActive->setReadOnly();
}

SmartFaderControl::~SmartFaderControl() = default;

void SmartFaderControl::setLeaderBpmDirect(double bpm) {
    Syncable* pInternalClock = m_pEngineSync->getInternalClock();
    mixxx::Bpm newBpm(bpm);

    // Update the InternalClock's internal state directly (beat length, etc.)
    pInternalClock->updateLeaderBpm(newBpm);

    // Synchronously propagate BPM to all follower decks.
    m_pEngineSync->notifyRateChanged(pInternalClock, newBpm);
}

void SmartFaderControl::activate() {
    Syncable* pLeftDeck = m_pEngineSync->getSyncableForGroup(kLeftDeckGroup);
    Syncable* pRightDeck = m_pEngineSync->getSyncableForGroup(kRightDeckGroup);

    if (!pLeftDeck || !pRightDeck) {
        kLogger.warning() << "Smart fader: could not find both decks";
        m_pActive->forceSet(0.0);
        return;
    }

    double leftFileBpm = ControlObject::get(ConfigKey(kLeftDeckGroup, "file_bpm"));
    double rightFileBpm = ControlObject::get(ConfigKey(kRightDeckGroup, "file_bpm"));
    if (leftFileBpm <= 0.0 || rightFileBpm <= 0.0) {
        m_pActive->forceSet(0.0);
        return;
    }

    // Save current sync modes for restoration.
    m_savedLeftSyncMode = pLeftDeck->getSyncMode();
    m_savedRightSyncMode = pRightDeck->getSyncMode();

    Syncable* pInternalClock = m_pEngineSync->getInternalClock();
    m_savedInternalClockSyncMode = pInternalClock->getSyncMode();

    // Make the InternalClock the explicit leader.
    m_pEngineSync->requestSyncMode(pInternalClock, SyncMode::LeaderExplicit);

    // Set both decks as followers. This call path will set each deck's
    // m_leaderBpmAdjustFactor via reinitLeaderParams; we override it below.
    m_pEngineSync->requestSyncMode(pLeftDeck, SyncMode::Follower);
    m_pEngineSync->requestSyncMode(pRightDeck, SyncMode::Follower);

    // Force unity adjust factor so neither deck plays at half/double the
    // leader BPM. Smart fader owns the rate mapping while it's active.
    pLeftDeck->setLeaderBpmAdjustFactor(kUnityFactor);
    pRightDeck->setLeaderBpmAdjustFactor(kUnityFactor);

    // Interpolate the leader BPM strictly between the two file BPMs.
    double crossfaderPos = m_crossfader.get();
    double t = (crossfaderPos + 1.0) / 2.0;
    double targetBpm = leftFileBpm * (1.0 - t) + rightFileBpm * t;

    setLeaderBpmDirect(targetBpm);

    // Request phase sync on both decks so beats stay locked.
    pLeftDeck->requestSync();
    pRightDeck->requestSync();

    m_lastLeftFileBpm = leftFileBpm;
    m_lastRightFileBpm = rightFileBpm;
    m_pLeftBpm->forceSet(leftFileBpm);
    m_pRightBpm->forceSet(rightFileBpm);
    m_pTargetBpm->forceSet(targetBpm);
    m_pActive->forceSet(1.0);
    m_bWasActive = true;

    kLogger.info() << "Smart fader activated: left=" << leftFileBpm
                   << "right=" << rightFileBpm;
}

void SmartFaderControl::deactivate() {
    Syncable* pLeftDeck = m_pEngineSync->getSyncableForGroup(kLeftDeckGroup);
    Syncable* pRightDeck = m_pEngineSync->getSyncableForGroup(kRightDeckGroup);
    Syncable* pInternalClock = m_pEngineSync->getInternalClock();

    // Restore original sync modes.
    if (pLeftDeck) {
        m_pEngineSync->requestSyncMode(pLeftDeck, m_savedLeftSyncMode);
    }
    if (pRightDeck) {
        m_pEngineSync->requestSyncMode(pRightDeck, m_savedRightSyncMode);
    }
    if (pInternalClock) {
        m_pEngineSync->requestSyncMode(pInternalClock, m_savedInternalClockSyncMode);
    }

    m_pActive->forceSet(0.0);
    m_pTargetBpm->forceSet(0.0);
    m_lastLeftFileBpm = 0.0;
    m_lastRightFileBpm = 0.0;
    m_bWasActive = false;

    kLogger.info() << "Smart fader deactivated";
}

void SmartFaderControl::process() {
    bool enabled = m_pEnabled->toBool();

    if (!enabled) {
        if (m_bWasActive) {
            deactivate();
        }
        return;
    }

    if (!m_bWasActive) {
        activate();
        if (!m_bWasActive) {
            // Activation failed (no tracks loaded yet). Retry next callback.
            return;
        }
    }

    Syncable* pLeftDeck = m_pEngineSync->getSyncableForGroup(kLeftDeckGroup);
    Syncable* pRightDeck = m_pEngineSync->getSyncableForGroup(kRightDeckGroup);
    if (!pLeftDeck || !pRightDeck) {
        deactivate();
        return;
    }

    double leftFileBpm = ControlObject::get(ConfigKey(kLeftDeckGroup, "file_bpm"));
    double rightFileBpm = ControlObject::get(ConfigKey(kRightDeckGroup, "file_bpm"));
    if (leftFileBpm <= 0.0 || rightFileBpm <= 0.0) {
        deactivate();
        return;
    }

    // Re-publish per-deck BPMs on track change.
    if (std::abs(leftFileBpm - m_lastLeftFileBpm) > kBpmChangeThreshold) {
        m_lastLeftFileBpm = leftFileBpm;
        m_pLeftBpm->forceSet(leftFileBpm);
    }
    if (std::abs(rightFileBpm - m_lastRightFileBpm) > kBpmChangeThreshold) {
        m_lastRightFileBpm = rightFileBpm;
        m_pRightBpm->forceSet(rightFileBpm);
    }

    // Ensure both decks are still followers and InternalClock is leader.
    Syncable* pInternalClock = m_pEngineSync->getInternalClock();
    if (!isLeader(pInternalClock->getSyncMode())) {
        m_pEngineSync->requestSyncMode(pInternalClock, SyncMode::LeaderExplicit);
    }
    if (!isFollower(pLeftDeck->getSyncMode())) {
        m_pEngineSync->requestSyncMode(pLeftDeck, SyncMode::Follower);
    }
    if (!isFollower(pRightDeck->getSyncMode())) {
        m_pEngineSync->requestSyncMode(pRightDeck, SyncMode::Follower);
    }

    // Keep the adjust factor pinned to 1.0 so sync's half/double cliff
    // can't reintroduce itself after a track load or sync-mode change.
    pLeftDeck->setLeaderBpmAdjustFactor(kUnityFactor);
    pRightDeck->setLeaderBpmAdjustFactor(kUnityFactor);

    // Lerp the leader BPM strictly between the two real file BPMs.
    double crossfaderPos = m_crossfader.get();
    double t = (crossfaderPos + 1.0) / 2.0;
    double targetBpm = leftFileBpm * (1.0 - t) + rightFileBpm * t;

    setLeaderBpmDirect(targetBpm);
    m_pTargetBpm->forceSet(targetBpm);
}
