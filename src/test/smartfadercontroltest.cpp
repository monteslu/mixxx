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

TEST_F(SmartFaderControlTest, HalfDoubleBpmNormalization) {
    // 70 BPM should be normalized to 140 for interpolation with a 140 BPM track
    setTrackBpm(m_pTrack1, 70.0);
    setTrackBpm(m_pTrack2, 140.0);
    setCrossfader(-1.0);
    ProcessBuffer();

    setSmartFaderEnabled(true);
    ProcessBuffer();

    // Left BPM should be doubled to 140
    EXPECT_NEAR(140.0, getSmartFaderLeftBpm(), kBpmEpsilon);
    EXPECT_NEAR(140.0, getSmartFaderRightBpm(), kBpmEpsilon);

    // Both at 140, so target should be 140 regardless of fader position
    EXPECT_NEAR(140.0, getSmartFaderTargetBpm(), kBpmEpsilon);

    setCrossfader(1.0);
    ProcessBuffer();
    EXPECT_NEAR(140.0, getSmartFaderTargetBpm(), kBpmEpsilon);
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
