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

constexpr double kBpmHalve = 0.5;
constexpr double kBpmDouble = 2.0;
constexpr double kBpmUnity = 1.0;

// Threshold for detecting a new track was loaded (BPM changed significantly)
constexpr double kBpmChangeThreshold = 1.0;
} // namespace

SmartFaderControl::SmartFaderControl(
        const QString& group, EngineSync* pEngineSync)
        : m_pEngineSync(pEngineSync),
          m_pEnabled(std::make_unique<ControlPushButton>(
                  ConfigKey(group, "smart_fader_enabled"))),
          m_pActive(std::make_unique<ControlObject>(
                  ConfigKey(group, "smart_fader_active"), false, false, false)),
          m_pLeftBpm(std::make_unique<ControlObject>(
                  ConfigKey(group, "smart_fader_left_bpm"), false, false, false)),
          m_pRightBpm(std::make_unique<ControlObject>(
                  ConfigKey(group, "smart_fader_right_bpm"), false, false, false)),
          m_pTargetBpm(std::make_unique<ControlObject>(
                  ConfigKey(group, "smart_fader_target_bpm"), false, false, false)),
          m_crossfader(ConfigKey(group, "crossfader")),
          m_capturedLeftBpm(0.0),
          m_capturedRightBpm(0.0),
          m_bWasActive(false),
          m_savedLeftSyncMode(SyncMode::None),
          m_savedRightSyncMode(SyncMode::None),
          m_savedInternalClockSyncMode(SyncMode::None) {
    m_pEnabled->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pActive->setReadOnly();
}

SmartFaderControl::~SmartFaderControl() = default;

// static
double SmartFaderControl::normalizeBpmMultiplier(double myBpm, double targetBpm) {
    if (myBpm <= 0.0 || targetBpm <= 0.0) {
        return kBpmUnity;
    }
    double unityRatio = myBpm / targetBpm;
    double unityRatioSquare = unityRatio * unityRatio;
    if (unityRatioSquare > kBpmDouble) {
        return kBpmDouble;
    } else if (unityRatioSquare < kBpmHalve) {
        return kBpmHalve;
    }
    return kBpmUnity;
}

void SmartFaderControl::setLeaderBpmDirect(double bpm) {
    Syncable* pInternalClock = m_pEngineSync->getInternalClock();
    mixxx::Bpm newBpm(bpm);

    // Update the InternalClock's internal state directly (beat length, etc.)
    pInternalClock->updateLeaderBpm(newBpm);

    // Synchronously propagate BPM to all follower decks
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

    mixxx::Bpm leftBaseBpm = pLeftDeck->getBaseBpm();
    mixxx::Bpm rightBaseBpm = pRightDeck->getBaseBpm();

    if (!leftBaseBpm.isValid() || !rightBaseBpm.isValid()) {
        m_pActive->forceSet(0.0);
        return;
    }

    m_capturedLeftBpm = leftBaseBpm.value();
    m_capturedRightBpm = rightBaseBpm.value();

    // Normalize for half/double BPM relationships: bring the left deck into
    // the same range as the right. If left is ~2x right, halve it; if left is
    // ~½x right, double it.
    double multiplier = normalizeBpmMultiplier(m_capturedLeftBpm, m_capturedRightBpm);
    if (multiplier == kBpmDouble) {
        m_capturedLeftBpm /= 2.0;
    } else if (multiplier == kBpmHalve) {
        m_capturedLeftBpm *= 2.0;
    }

    // Save current sync modes for restoration
    m_savedLeftSyncMode = pLeftDeck->getSyncMode();
    m_savedRightSyncMode = pRightDeck->getSyncMode();

    Syncable* pInternalClock = m_pEngineSync->getInternalClock();
    m_savedInternalClockSyncMode = pInternalClock->getSyncMode();

    // Make the InternalClock the explicit leader
    m_pEngineSync->requestSyncMode(pInternalClock, SyncMode::LeaderExplicit);

    // Set both decks as followers
    m_pEngineSync->requestSyncMode(pLeftDeck, SyncMode::Follower);
    m_pEngineSync->requestSyncMode(pRightDeck, SyncMode::Follower);

    // Set the initial target BPM based on current crossfader position
    double crossfaderPos = m_crossfader.get();
    double t = (crossfaderPos + 1.0) / 2.0;
    double targetBpm = m_capturedLeftBpm * (1.0 - t) + m_capturedRightBpm * t;

    // Directly update InternalClock BPM and propagate to followers
    setLeaderBpmDirect(targetBpm);

    // Request phase sync on both decks
    pLeftDeck->requestSync();
    pRightDeck->requestSync();

    m_pLeftBpm->forceSet(m_capturedLeftBpm);
    m_pRightBpm->forceSet(m_capturedRightBpm);
    m_pTargetBpm->forceSet(targetBpm);
    m_pActive->forceSet(1.0);
    m_bWasActive = true;

    kLogger.info() << "Smart fader activated: left=" << m_capturedLeftBpm
                   << "right=" << m_capturedRightBpm;
}

void SmartFaderControl::deactivate() {
    Syncable* pLeftDeck = m_pEngineSync->getSyncableForGroup(kLeftDeckGroup);
    Syncable* pRightDeck = m_pEngineSync->getSyncableForGroup(kRightDeckGroup);
    Syncable* pInternalClock = m_pEngineSync->getInternalClock();

    // Restore original sync modes
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
    m_capturedLeftBpm = 0.0;
    m_capturedRightBpm = 0.0;
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
            // Activation failed (e.g., no tracks loaded) — stay enabled,
            // will retry next callback until tracks are loaded
            return;
        }
    }

    // Check if either deck had a track change (BPM shifted significantly)
    Syncable* pLeftDeck = m_pEngineSync->getSyncableForGroup(kLeftDeckGroup);
    Syncable* pRightDeck = m_pEngineSync->getSyncableForGroup(kRightDeckGroup);

    if (!pLeftDeck || !pRightDeck) {
        deactivate();
        return;
    }

    mixxx::Bpm leftBaseBpm = pLeftDeck->getBaseBpm();
    mixxx::Bpm rightBaseBpm = pRightDeck->getBaseBpm();

    if (!leftBaseBpm.isValid() || !rightBaseBpm.isValid()) {
        deactivate();
        return;
    }

    // Re-capture if a new track was loaded
    double currentLeftBase = leftBaseBpm.value();
    double currentRightBase = rightBaseBpm.value();
    double normalizedLeft = currentLeftBase;
    double multiplier = normalizeBpmMultiplier(currentLeftBase, currentRightBase);
    if (multiplier == kBpmDouble) {
        normalizedLeft /= 2.0;
    } else if (multiplier == kBpmHalve) {
        normalizedLeft *= 2.0;
    }
    if (std::abs(normalizedLeft - m_capturedLeftBpm) > kBpmChangeThreshold ||
            std::abs(currentRightBase - m_capturedRightBpm) > kBpmChangeThreshold) {
        m_capturedLeftBpm = normalizedLeft;
        m_capturedRightBpm = currentRightBase;
        m_pLeftBpm->forceSet(m_capturedLeftBpm);
        m_pRightBpm->forceSet(m_capturedRightBpm);
    }

    // Ensure both decks are still followers and InternalClock is leader
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

    // Interpolate BPM based on crossfader position
    double crossfaderPos = m_crossfader.get();
    double t = (crossfaderPos + 1.0) / 2.0; // normalize -1..1 to 0..1
    double targetBpm = m_capturedLeftBpm * (1.0 - t) + m_capturedRightBpm * t;

    // Directly update InternalClock BPM and propagate to all followers
    setLeaderBpmDirect(targetBpm);
    m_pTargetBpm->forceSet(targetBpm);
}
