/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define STATSD_DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "CountMetricProducer.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>

#include "guardrail/StatsdStats.h"
#include "metrics/parsing_utils/metrics_manager_util.h"
#include "stats_log_util.h"
#include "stats_util.h"

using android::util::FIELD_COUNT_REPEATED;
using android::util::FIELD_TYPE_BOOL;
using android::util::FIELD_TYPE_FLOAT;
using android::util::FIELD_TYPE_INT32;
using android::util::FIELD_TYPE_INT64;
using android::util::FIELD_TYPE_MESSAGE;
using android::util::FIELD_TYPE_STRING;
using android::util::ProtoOutputStream;
using std::map;
using std::string;
using std::unordered_map;
using std::vector;
using std::shared_ptr;

namespace android {
namespace os {
namespace statsd {

// for StatsLogReport
const int FIELD_ID_ID = 1;
const int FIELD_ID_COUNT_METRICS = 5;
const int FIELD_ID_TIME_BASE = 9;
const int FIELD_ID_BUCKET_SIZE = 10;
const int FIELD_ID_DIMENSION_PATH_IN_WHAT = 11;
const int FIELD_ID_IS_ACTIVE = 14;
const int FIELD_ID_DIMENSION_GUARDRAIL_HIT = 17;

// for CountMetricDataWrapper
const int FIELD_ID_DATA = 1;
// for CountMetricData
const int FIELD_ID_DIMENSION_IN_WHAT = 1;
const int FIELD_ID_SLICE_BY_STATE = 6;
const int FIELD_ID_BUCKET_INFO = 3;
const int FIELD_ID_DIMENSION_LEAF_IN_WHAT = 4;
// for CountBucketInfo
const int FIELD_ID_COUNT = 3;
const int FIELD_ID_BUCKET_NUM = 4;
const int FIELD_ID_START_BUCKET_ELAPSED_MILLIS = 5;
const int FIELD_ID_END_BUCKET_ELAPSED_MILLIS = 6;
const int FIELD_ID_CONDITION_TRUE_NS = 7;

CountMetricProducer::CountMetricProducer(
        const ConfigKey& key, const CountMetric& metric, const int conditionIndex,
        const vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const uint64_t protoHash, const int64_t timeBaseNs, const int64_t startTimeNs,
        const unordered_map<int, shared_ptr<Activation>>& eventActivationMap,
        const unordered_map<int, vector<shared_ptr<Activation>>>& eventDeactivationMap,
        const vector<int>& slicedStateAtoms,
        const unordered_map<int, unordered_map<int, int64_t>>& stateGroupMap)
    : MetricProducer(metric.id(), key, timeBaseNs, conditionIndex, initialConditionCache, wizard,
                     protoHash, eventActivationMap, eventDeactivationMap, slicedStateAtoms,
                     stateGroupMap, getAppUpgradeBucketSplit(metric)),
      mDimensionGuardrailHit(false),
      mDimensionHardLimit(
              StatsdStats::clampDimensionKeySizeLimit(metric.max_dimensions_per_bucket())) {
    if (metric.has_bucket()) {
        mBucketSizeNs =
                TimeUnitToBucketSizeInMillisGuardrailed(key.GetUid(), metric.bucket()) * 1000000;
    } else {
        mBucketSizeNs = LLONG_MAX;
    }

    if (metric.has_dimensions_in_what()) {
        translateFieldMatcher(metric.dimensions_in_what(), &mDimensionsInWhat);
        mContainANYPositionInDimensionsInWhat = HasPositionANY(metric.dimensions_in_what());
    }

    mShouldUseNestedDimensions = ShouldUseNestedDimensions(metric.dimensions_in_what());

    if (metric.links().size() > 0) {
        for (const auto& link : metric.links()) {
            Metric2Condition mc;
            mc.conditionId = link.condition();
            translateFieldMatcher(link.fields_in_what(), &mc.metricFields);
            translateFieldMatcher(link.fields_in_condition(), &mc.conditionFields);
            mMetric2ConditionLinks.push_back(mc);
        }
        mConditionSliced = true;
    }

    for (const auto& stateLink : metric.state_link()) {
        Metric2State ms;
        ms.stateAtomId = stateLink.state_atom_id();
        translateFieldMatcher(stateLink.fields_in_what(), &ms.metricFields);
        translateFieldMatcher(stateLink.fields_in_state(), &ms.stateFields);
        mMetric2StateLinks.push_back(ms);
    }

    if (metric.has_threshold()) {
        mUploadThreshold = metric.threshold();
    }

    flushIfNeededLocked(startTimeNs);
    // Adjust start for partial bucket
    mCurrentBucketStartTimeNs = startTimeNs;
    mConditionTimer.newBucketStart(mCurrentBucketStartTimeNs, mCurrentBucketStartTimeNs);
    mConditionTimer.onConditionChanged(mIsActive && mCondition == ConditionState::kTrue,
                                       mCurrentBucketStartTimeNs);

    VLOG("metric %lld created. bucket size %lld start_time: %lld", (long long)mMetricId,
         (long long)mBucketSizeNs, (long long)mTimeBaseNs);
}

CountMetricProducer::~CountMetricProducer() {
    VLOG("~CountMetricProducer() called");
}

optional<InvalidConfigReason> CountMetricProducer::onConfigUpdatedLocked(
        const StatsdConfig& config, const int configIndex, const int metricIndex,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
        const sp<EventMatcherWizard>& matcherWizard,
        const vector<sp<ConditionTracker>>& allConditionTrackers,
        const unordered_map<int64_t, int>& conditionTrackerMap, const sp<ConditionWizard>& wizard,
        const unordered_map<int64_t, int>& metricToActivationMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation) {
    optional<InvalidConfigReason> invalidConfigReason = MetricProducer::onConfigUpdatedLocked(
            config, configIndex, metricIndex, allAtomMatchingTrackers, oldAtomMatchingTrackerMap,
            newAtomMatchingTrackerMap, matcherWizard, allConditionTrackers, conditionTrackerMap,
            wizard, metricToActivationMap, trackerToMetricMap, conditionToMetricMap,
            activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
            metricsWithActivation);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    const CountMetric& metric = config.count_metric(configIndex);
    int trackerIndex;
    // Update appropriate indices, specifically mConditionIndex and MetricsManager maps.
    invalidConfigReason = handleMetricWithAtomMatchingTrackers(
            metric.what(), mMetricId, metricIndex, false, allAtomMatchingTrackers,
            newAtomMatchingTrackerMap, trackerToMetricMap, trackerIndex);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    if (metric.has_condition()) {
        invalidConfigReason = handleMetricWithConditions(
                metric.condition(), mMetricId, metricIndex, conditionTrackerMap, metric.links(),
                allConditionTrackers, mConditionTrackerIndex, conditionToMetricMap);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    return nullopt;
}

void CountMetricProducer::onStateChanged(const int64_t eventTimeNs, const int32_t atomId,
                                         const HashableDimensionKey& primaryKey,
                                         const FieldValue& oldState, const FieldValue& newState) {
    VLOG("CountMetric %lld onStateChanged time %lld, State%d, key %s, %d -> %d",
         (long long)mMetricId, (long long)eventTimeNs, atomId, primaryKey.toString().c_str(),
         oldState.mValue.int_value, newState.mValue.int_value);
}

void CountMetricProducer::dumpStatesLocked(int out, bool verbose) const {
    if (mCurrentSlicedCounter == nullptr ||
        mCurrentSlicedCounter->size() == 0) {
        return;
    }

    dprintf(out, "CountMetric %lld dimension size %lu\n", (long long)mMetricId,
            (unsigned long)mCurrentSlicedCounter->size());
    if (verbose) {
        for (const auto& it : *mCurrentSlicedCounter) {
            dprintf(out, "\t(what)%s\t(state)%s  %lld\n",
                    it.first.getDimensionKeyInWhat().toString().c_str(),
                    it.first.getStateValuesKey().toString().c_str(), (unsigned long long)it.second);
        }
    }
}

void CountMetricProducer::onSlicedConditionMayChangeLocked(bool overallCondition,
                                                           const int64_t eventTime) {
    VLOG("Metric %lld onSlicedConditionMayChange", (long long)mMetricId);
}


void CountMetricProducer::clearPastBucketsLocked(const int64_t dumpTimeNs) {
    mPastBuckets.clear();
}

void CountMetricProducer::onDumpReportLocked(const int64_t dumpTimeNs,
                                             const bool include_current_partial_bucket,
                                             const bool erase_data, const DumpLatency dumpLatency,
                                             std::set<string>* str_set,
                                             ProtoOutputStream* protoOutput) {
    if (include_current_partial_bucket) {
        flushLocked(dumpTimeNs);
    } else {
        flushIfNeededLocked(dumpTimeNs);
    }

    protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_ID, (long long)mMetricId);
    protoOutput->write(FIELD_TYPE_BOOL | FIELD_ID_IS_ACTIVE, isActiveLocked());

    if (mPastBuckets.empty()) {
        return;
    }

    if (mDimensionGuardrailHit) {
        protoOutput->write(FIELD_TYPE_BOOL | FIELD_ID_DIMENSION_GUARDRAIL_HIT,
                           mDimensionGuardrailHit);
    }

    protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_TIME_BASE, (long long)mTimeBaseNs);
    protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_BUCKET_SIZE, (long long)mBucketSizeNs);

    // Fills the dimension path if not slicing by a primitive repeated field or position ALL.
    if (!mShouldUseNestedDimensions) {
        if (!mDimensionsInWhat.empty()) {
            uint64_t dimenPathToken = protoOutput->start(
                    FIELD_TYPE_MESSAGE | FIELD_ID_DIMENSION_PATH_IN_WHAT);
            writeDimensionPathToProto(mDimensionsInWhat, protoOutput);
            protoOutput->end(dimenPathToken);
        }
    }

    uint64_t protoToken = protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_ID_COUNT_METRICS);

    for (const auto& counter : mPastBuckets) {
        const MetricDimensionKey& dimensionKey = counter.first;
        VLOG("  dimension key %s", dimensionKey.toString().c_str());

        uint64_t wrapperToken =
                protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_DATA);

        // First fill dimension.
        if (mShouldUseNestedDimensions) {
            uint64_t dimensionToken = protoOutput->start(
                    FIELD_TYPE_MESSAGE | FIELD_ID_DIMENSION_IN_WHAT);
            writeDimensionToProto(dimensionKey.getDimensionKeyInWhat(), str_set, protoOutput);
            protoOutput->end(dimensionToken);
        } else {
            writeDimensionLeafNodesToProto(dimensionKey.getDimensionKeyInWhat(),
                                           FIELD_ID_DIMENSION_LEAF_IN_WHAT, str_set, protoOutput);
        }
        // Then fill slice_by_state.
        for (auto state : dimensionKey.getStateValuesKey().getValues()) {
            uint64_t stateToken = protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED |
                                                     FIELD_ID_SLICE_BY_STATE);
            writeStateToProto(state, protoOutput);
            protoOutput->end(stateToken);
        }
        // Then fill bucket_info (CountBucketInfo).
        for (const auto& bucket : counter.second) {
            uint64_t bucketInfoToken = protoOutput->start(
                    FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_BUCKET_INFO);
            // Partial bucket.
            if (bucket.mBucketEndNs - bucket.mBucketStartNs != mBucketSizeNs) {
                protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_START_BUCKET_ELAPSED_MILLIS,
                                   (long long)NanoToMillis(bucket.mBucketStartNs));
                protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_END_BUCKET_ELAPSED_MILLIS,
                                   (long long)NanoToMillis(bucket.mBucketEndNs));
            } else {
                protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_BUCKET_NUM,
                                   (long long)(getBucketNumFromEndTimeNs(bucket.mBucketEndNs)));
            }
            protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_COUNT, (long long)bucket.mCount);

            // We only write the condition timer value if the metric has a
            // condition and isn't sliced by state or condition.
            // TODO(b/268531179): Slice the condition timer by state and condition
            if (mConditionTrackerIndex >= 0 && mSlicedStateAtoms.empty() && !mConditionSliced) {
                protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_CONDITION_TRUE_NS,
                                   (long long)bucket.mConditionTrueNs);
            }

            protoOutput->end(bucketInfoToken);
            VLOG("\t bucket [%lld - %lld] count: %lld", (long long)bucket.mBucketStartNs,
                 (long long)bucket.mBucketEndNs, (long long)bucket.mCount);
        }
        protoOutput->end(wrapperToken);
    }

    protoOutput->end(protoToken);

    if (erase_data) {
        mPastBuckets.clear();
        mDimensionGuardrailHit = false;
    }
}

void CountMetricProducer::dropDataLocked(const int64_t dropTimeNs) {
    flushIfNeededLocked(dropTimeNs);
    StatsdStats::getInstance().noteBucketDropped(mMetricId);
    mPastBuckets.clear();
}

void CountMetricProducer::onConditionChangedLocked(const bool conditionMet,
                                                   const int64_t eventTime) {
    VLOG("Metric %lld onConditionChanged", (long long)mMetricId);
    mCondition = conditionMet ? ConditionState::kTrue : ConditionState::kFalse;

    if (!mIsActive) {
        return;
    }

    mConditionTimer.onConditionChanged(mCondition, eventTime);
}

bool CountMetricProducer::hitGuardRailLocked(const MetricDimensionKey& newKey) {
    if (mCurrentSlicedCounter->find(newKey) != mCurrentSlicedCounter->end()) {
        return false;
    }
    // ===========GuardRail==============
    // 1. Report the tuple count if the tuple count > soft limit
    if (mCurrentSlicedCounter->size() >= StatsdStats::kDimensionKeySizeSoftLimit) {
        size_t newTupleCount = mCurrentSlicedCounter->size() + 1;
        StatsdStats::getInstance().noteMetricDimensionSize(mConfigKey, mMetricId, newTupleCount);
        // 2. Don't add more tuples, we are above the allowed threshold. Drop the data.
        if (newTupleCount > mDimensionHardLimit) {
            if (!mHasHitGuardrail) {
                ALOGE("CountMetric %lld dropping data for dimension key %s", (long long)mMetricId,
                      newKey.toString().c_str());
                mHasHitGuardrail = true;
            }
            mDimensionGuardrailHit = true;
            StatsdStats::getInstance().noteHardDimensionLimitReached(mMetricId);
            return true;
        }
    }

    return false;
}

void CountMetricProducer::onMatchedLogEventInternalLocked(
        const size_t matcherIndex, const MetricDimensionKey& eventKey,
        const ConditionKey& conditionKey, bool condition, const LogEvent& event,
        const map<int, HashableDimensionKey>& statePrimaryKeys) {
    int64_t eventTimeNs = event.GetElapsedTimestampNs();
    flushIfNeededLocked(eventTimeNs);

    if (!condition) {
        return;
    }

    auto it = mCurrentSlicedCounter->find(eventKey);
    if (it == mCurrentSlicedCounter->end()) {
        // ===========GuardRail==============
        if (hitGuardRailLocked(eventKey)) {
            return;
        }
        // create a counter for the new key
        (*mCurrentSlicedCounter)[eventKey] = 1;
    } else {
        // increment the existing value
        auto& count = it->second;
        count++;
    }
    for (auto& tracker : mAnomalyTrackers) {
        int64_t countWholeBucket = mCurrentSlicedCounter->find(eventKey)->second;
        auto prev = mCurrentFullCounters->find(eventKey);
        if (prev != mCurrentFullCounters->end()) {
            countWholeBucket += prev->second;
        }
        tracker->detectAndDeclareAnomaly(eventTimeNs, mCurrentBucketNum, mMetricId, eventKey,
                                         countWholeBucket);
    }

    VLOG("metric %lld %s->%lld", (long long)mMetricId, eventKey.toString().c_str(),
         (long long)(*mCurrentSlicedCounter)[eventKey]);
}

// When a new matched event comes in, we check if event falls into the current
// bucket. If not, flush the old counter to past buckets and initialize the new bucket.
void CountMetricProducer::flushIfNeededLocked(const int64_t eventTimeNs) {
    int64_t currentBucketEndTimeNs = getCurrentBucketEndTimeNs();
    if (eventTimeNs < currentBucketEndTimeNs) {
        return;
    }

    // Setup the bucket start time and number.
    int64_t numBucketsForward = 1 + (eventTimeNs - currentBucketEndTimeNs) / mBucketSizeNs;
    int64_t nextBucketNs = currentBucketEndTimeNs + (numBucketsForward - 1) * mBucketSizeNs;
    flushCurrentBucketLocked(eventTimeNs, nextBucketNs);

    mCurrentBucketNum += numBucketsForward;
    VLOG("metric %lld: new bucket start time: %lld", (long long)mMetricId,
         (long long)mCurrentBucketStartTimeNs);
}

bool CountMetricProducer::countPassesThreshold(const int64_t count) {
    if (mUploadThreshold == nullopt) {
        return true;
    }

    switch (mUploadThreshold->value_comparison_case()) {
        case UploadThreshold::kLtInt:
            return count < mUploadThreshold->lt_int();
        case UploadThreshold::kGtInt:
            return count > mUploadThreshold->gt_int();
        case UploadThreshold::kLteInt:
            return count <= mUploadThreshold->lte_int();
        case UploadThreshold::kGteInt:
            return count >= mUploadThreshold->gte_int();
        default:
            ALOGE("Count metric incorrect upload threshold type used");
            return false;
    }
}

void CountMetricProducer::flushCurrentBucketLocked(const int64_t eventTimeNs,
                                                   const int64_t nextBucketStartTimeNs) {
    int64_t fullBucketEndTimeNs = getCurrentBucketEndTimeNs();
    CountBucket info;
    info.mBucketStartNs = mCurrentBucketStartTimeNs;
    if (eventTimeNs < fullBucketEndTimeNs) {
        info.mBucketEndNs = eventTimeNs;
    } else {
        info.mBucketEndNs = fullBucketEndTimeNs;
    }

    const auto [globalConditionTrueNs, globalConditionCorrectionNs] =
            mConditionTimer.newBucketStart(eventTimeNs, nextBucketStartTimeNs);
    info.mConditionTrueNs = globalConditionTrueNs;

    for (const auto& counter : *mCurrentSlicedCounter) {
        if (countPassesThreshold(counter.second)) {
            info.mCount = counter.second;
            auto& bucketList = mPastBuckets[counter.first];
            bucketList.push_back(info);
            VLOG("metric %lld, dump key value: %s -> %lld", (long long)mMetricId,
                 counter.first.toString().c_str(), (long long)counter.second);
        }
    }

    // Only update mCurrentFullCounters if any anomaly tackers are present.
    if (mAnomalyTrackers.size() > 0) {
        // If we have finished a full bucket, then send this to anomaly tracker.
        if (eventTimeNs > fullBucketEndTimeNs) {
            // Accumulate partial buckets with current value and then send to anomaly tracker.
            if (mCurrentFullCounters->size() > 0) {
                for (const auto& keyValuePair : *mCurrentSlicedCounter) {
                    (*mCurrentFullCounters)[keyValuePair.first] += keyValuePair.second;
                }
                for (auto& tracker : mAnomalyTrackers) {
                    tracker->addPastBucket(mCurrentFullCounters, mCurrentBucketNum);
                }
                mCurrentFullCounters = std::make_shared<DimToValMap>();
            } else {
                // Skip aggregating the partial buckets since there's no previous partial bucket.
                for (auto& tracker : mAnomalyTrackers) {
                    tracker->addPastBucket(mCurrentSlicedCounter, mCurrentBucketNum);
                }
            }
        } else {
            // Accumulate partial bucket.
            for (const auto& keyValuePair : *mCurrentSlicedCounter) {
                (*mCurrentFullCounters)[keyValuePair.first] += keyValuePair.second;
            }
        }
    }

    StatsdStats::getInstance().noteBucketCount(mMetricId);
    // Only resets the counters, but doesn't setup the times nor numbers.
    // (Do not clear since the old one is still referenced in mAnomalyTrackers).
    mCurrentSlicedCounter = std::make_shared<DimToValMap>();
    mCurrentBucketStartTimeNs = nextBucketStartTimeNs;
    // Reset mHasHitGuardrail boolean since bucket was reset
    mHasHitGuardrail = false;
}

// Rough estimate of CountMetricProducer buffer stored. This number will be
// greater than actual data size as it contains each dimension of
// CountMetricData is  duplicated.
size_t CountMetricProducer::byteSizeLocked() const {
    size_t totalSize = 0;
    for (const auto& pair : mPastBuckets) {
        totalSize += pair.second.size() * kBucketSize;
    }
    return totalSize;
}

void CountMetricProducer::onActiveStateChangedLocked(const int64_t eventTimeNs,
                                                     const bool isActive) {
    MetricProducer::onActiveStateChangedLocked(eventTimeNs, isActive);

    if (ConditionState::kTrue != mCondition) {
        return;
    }

    mConditionTimer.onConditionChanged(isActive, eventTimeNs);
}

}  // namespace statsd
}  // namespace os
}  // namespace android
