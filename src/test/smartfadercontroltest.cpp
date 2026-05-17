#include <gtest/gtest.h>

#include "control/controlobject.h"
#include "engine/sync/enginesync.h"
#include "test/mockedenginebackendtest.h"
#include "track/beats.h"

namespace {
constexpr double kBpmEpsilon = 0.5;
} // namespace

class SmartFaderControlTest : public MockedEngineBackendTest {
  protected:
    void setTrackBpm(TrackPointer pTrack, double bpm) {
        auto pBeats = mixxx::Beats::fromConstTempo(
                pTrack->getSampleRate(),
                mixxx::audio::kStartFramePos,
                mixxx::Bpm(bpm));
        pTrack->trySetBeats(pBeats);
    }

    void setSmartFaderEnabled(bool enabled) {
        ControlObject::set(
                ConfigKey(m_sMainGroup, "smart_fader_enabled"),
                enabled ? 1.0 : 0.0);
    }

    double getSmartFaderActive() {
        return ControlObject::get(
                ConfigKey(m_sMainGroup, "smart_fader_active"));
    }

    double getSmartFaderTargetBpm() {
        return ControlObject::get(
                ConfigKey(m_sMainGroup, "smart_fader_target_bpm"));
    }

    double getSmartFaderLeftBpm() {
        return ControlObject::get(
                ConfigKey(m_sMainGroup, "smart_fader_left_bpm"));
    }

    double getSmartFaderRightBpm() {
        return ControlObject::get(
                ConfigKey(m_sMainGroup, "smart_fader_right_bpm"));
    }

    void setCrossfader(double value) {
        ControlObject::set(
                ConfigKey(m_sMainGroup, "crossfader"), value);
    }

    double getDeckBpm(const QString& group) {
        return ControlObject::get(ConfigKey(group, "bpm"));
    }
};

TEST_F(SmartFaderControlTest, ActivateWithTwoTracksLoaded) {
    setTrackBpm(m_pTrack1, 100.0);
    setTrackBpm(m_pTrack2, 125.0);
    ProcessBuffer();

    setSmartFaderEnabled(true);
    ProcessBuffer();

    EXPECT_DOUBLE_EQ(1.0, getSmartFaderActive());
    EXPECT_NEAR(100.0, getSmartFaderLeftBpm(), kBpmEpsilon);
    EXPECT_NEAR(125.0, getSmartFaderRightBpm(), kBpmEpsilon);

    // InternalClock should be explicit leader
    Syncable* pInternalClock = m_pEngineSync->getInternalClock();
    EXPECT_TRUE(isLeader(pInternalClock->getSyncMode()));

    // Both decks should be followers
    EXPECT_EQ(SyncMode::Follower,
            static_cast<SyncMode>(ControlObject::get(
                    ConfigKey(m_sGroup1, "sync_mode"))));
    EXPECT_EQ(SyncMode::Follower,
            static_cast<SyncMode>(ControlObject::get(
                    ConfigKey(m_sGroup2, "sync_mode"))));
}

TEST_F(SmartFaderControlTest, ActivateFailsWithNoTracks) {
    // Tracks are loaded in MockedEngineBackendTest but have 0 BPM by default.
    // Don't set beats, so BPM is invalid.
    ProcessBuffer();

    setSmartFaderEnabled(true);
    ProcessBuffer();

    // Should stay enabled but not active
    EXPECT_DOUBLE_EQ(1.0,
            ControlObject::get(
                    ConfigKey(m_sMainGroup, "smart_fader_enabled")));
    EXPECT_DOUBLE_EQ(0.0, getSmartFaderActive());
}

TEST_F(SmartFaderControlTest, CrossfaderFullLeftMatchesLeftBpm) {
    setTrackBpm(m_pTrack1, 100.0);
    setTrackBpm(m_pTrack2, 140.0);
    setCrossfader(-1.0);
    ProcessBuffer();

    setSmartFaderEnabled(true);
    ProcessBuffer();

    EXPECT_NEAR(100.0, getSmartFaderTargetBpm(), kBpmEpsilon);
}

TEST_F(SmartFaderControlTest, CrossfaderFullRightMatchesRightBpm) {
    setTrackBpm(m_pTrack1, 100.0);
    setTrackBpm(m_pTrack2, 140.0);
    setCrossfader(1.0);
    ProcessBuffer();

    setSmartFaderEnabled(true);
    ProcessBuffer();

    EXPECT_NEAR(140.0, getSmartFaderTargetBpm(), kBpmEpsilon);
}

TEST_F(SmartFaderControlTest, CrossfaderCenterIsMidpoint) {
    setTrackBpm(m_pTrack1, 100.0);
    setTrackBpm(m_pTrack2, 140.0);
    setCrossfader(0.0);
    ProcessBuffer();

    setSmartFaderEnabled(true);
    ProcessBuffer();

    EXPECT_NEAR(120.0, getSmartFaderTargetBpm(), kBpmEpsilon);
}

TEST_F(SmartFaderControlTest, CrossfaderMovementUpdatesBpm) {
    setTrackBpm(m_pTrack1, 100.0);
    setTrackBpm(m_pTrack2, 140.0);
    setCrossfader(-1.0);
    ProcessBuffer();

    setSmartFaderEnabled(true);
    ProcessBuffer();
    EXPECT_NEAR(100.0, getSmartFaderTargetBpm(), kBpmEpsilon);

    // Move crossfader to center
    setCrossfader(0.0);
    ProcessBuffer();
    EXPECT_NEAR(120.0, getSmartFaderTargetBpm(), kBpmEpsilon);

    // Move crossfader to right
    setCrossfader(1.0);
    ProcessBuffer();
    EXPECT_NEAR(140.0, getSmartFaderTargetBpm(), kBpmEpsilon);

    // Move back to left
    setCrossfader(-1.0);
    ProcessBuffer();
    EXPECT_NEAR(100.0, getSmartFaderTargetBpm(), kBpmEpsilon);
}

TEST_F(SmartFaderControlTest, DeactivateRestoresSyncModes) {
    setTrackBpm(m_pTrack1, 120.0);
    setTrackBpm(m_pTrack2, 130.0);
    ProcessBuffer();

    // Verify both decks start with sync off
    EXPECT_EQ(SyncMode::None,
            static_cast<SyncMode>(ControlObject::get(
                    ConfigKey(m_sGroup1, "sync_mode"))));
    EXPECT_EQ(SyncMode::None,
            static_cast<SyncMode>(ControlObject::get(
                    ConfigKey(m_sGroup2, "sync_mode"))));

    setSmartFaderEnabled(true);
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(1.0, getSmartFaderActive());

    // Deactivate
    setSmartFaderEnabled(false);
    ProcessBuffer();

    EXPECT_DOUBLE_EQ(0.0, getSmartFaderActive());

    // Sync modes should be restored to None
    EXPECT_EQ(SyncMode::None,
            static_cast<SyncMode>(ControlObject::get(
                    ConfigKey(m_sGroup1, "sync_mode"))));
    EXPECT_EQ(SyncMode::None,
            static_cast<SyncMode>(ControlObject::get(
                    ConfigKey(m_sGroup2, "sync_mode"))));
}

TEST_F(SmartFaderControlTest, ActivatesWhenTracksLoadedLater) {
    // Enable smart fader before tracks have BPM
    setSmartFaderEnabled(true);
    ProcessBuffer();

    // Should be enabled but not active
    EXPECT_DOUBLE_EQ(0.0, getSmartFaderActive());

    // Now load tracks with BPM
    setTrackBpm(m_pTrack1, 110.0);
    setTrackBpm(m_pTrack2, 130.0);
    ProcessBuffer();

    // Should now be active
    EXPECT_DOUBLE_EQ(1.0, getSmartFaderActive());
}

// Regression: two tracks more than √2 apart in BPM (here 72.5 vs 128) must
// interpolate strictly between the two real file BPMs. Previously the smart
// fader doubled 72.5 to 145 and lerped [145, 128], and sync's automatic
// half/double cliff drove the decks to 68.25 / 136.5 at center.
TEST_F(SmartFaderControlTest, NoHalfDoubleNormalizationAcrossSqrt2) {
    setTrackBpm(m_pTrack1, 72.5);
    setTrackBpm(m_pTrack2, 128.0);
    setCrossfader(0.0);
    ProcessBuffer();

    setSmartFaderEnabled(true);
    ProcessBuffer();

    EXPECT_DOUBLE_EQ(1.0, getSmartFaderActive());
    // Captured BPMs are the raw file BPMs, not normalized.
    EXPECT_NEAR(72.5, getSmartFaderLeftBpm(), kBpmEpsilon);
    EXPECT_NEAR(128.0, getSmartFaderRightBpm(), kBpmEpsilon);

    // Center fader → midpoint of the two file BPMs.
    EXPECT_NEAR(100.25, getSmartFaderTargetBpm(), kBpmEpsilon);

    // Both decks play at the target BPM (no half/double doubling).
    EXPECT_NEAR(100.25, getDeckBpm(m_sGroup1), kBpmEpsilon);
    EXPECT_NEAR(100.25, getDeckBpm(m_sGroup2), kBpmEpsilon);

    // Full left: both decks at left's file BPM.
    setCrossfader(-1.0);
    ProcessBuffer();
    EXPECT_NEAR(72.5, getSmartFaderTargetBpm(), kBpmEpsilon);
    EXPECT_NEAR(72.5, getDeckBpm(m_sGroup1), kBpmEpsilon);
    EXPECT_NEAR(72.5, getDeckBpm(m_sGroup2), kBpmEpsilon);

    // Full right: both decks at right's file BPM.
    setCrossfader(1.0);
    ProcessBuffer();
    EXPECT_NEAR(128.0, getSmartFaderTargetBpm(), kBpmEpsilon);
    EXPECT_NEAR(128.0, getDeckBpm(m_sGroup1), kBpmEpsilon);
    EXPECT_NEAR(128.0, getDeckBpm(m_sGroup2), kBpmEpsilon);
}

// Regression: loading a new track after smart fader is active must not
// trigger sync's half/double cliff. Previously the new follower's
// m_leaderBpmAdjustFactor would be set to 0.5 or 2.0 based on whatever
// the leader BPM happened to be, doubling or halving the displayed BPM.
TEST_F(SmartFaderControlTest, TrackChangeWhileActiveDoesNotHalfDouble) {
    setTrackBpm(m_pTrack1, 120.0);
    setTrackBpm(m_pTrack2, 128.0);
    setCrossfader(0.0);
    ProcessBuffer();

    setSmartFaderEnabled(true);
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(1.0, getSmartFaderActive());

    // Simulate loading a new track on the left deck with a BPM that
    // would previously have triggered sync's half/double cliff
    // (90 vs leader ~124 → ratio² ≈ 0.53; on the boundary).
    // Use 70 to be solidly inside the cliff (ratio² ≈ 0.32 < 0.5).
    setTrackBpm(m_pTrack1, 70.0);
    ProcessBuffer();

    // The captured left BPM is the raw file BPM, not multiplier-adjusted.
    EXPECT_NEAR(70.0, getSmartFaderLeftBpm(), kBpmEpsilon);
    EXPECT_NEAR(128.0, getSmartFaderRightBpm(), kBpmEpsilon);

    // Center fader → midpoint.
    EXPECT_NEAR(99.0, getSmartFaderTargetBpm(), kBpmEpsilon);
    EXPECT_NEAR(99.0, getDeckBpm(m_sGroup1), kBpmEpsilon);
    EXPECT_NEAR(99.0, getDeckBpm(m_sGroup2), kBpmEpsilon);
}
