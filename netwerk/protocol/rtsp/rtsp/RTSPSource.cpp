/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include "RtspPrlog.h"
#include "RTSPSource.h"
#include "ARTPConnection.h"
#include "RTSPConnectionHandler.h"
#include "RtspMetaData.h"
#include <AnotherPacketSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>

#include "nsDebug.h"
#include "nsString.h"
#include "nsStringStream.h"
#include "nsAutoPtr.h"
#include "mozilla/DebugOnly.h"

using namespace mozilla;
using namespace mozilla::net;

namespace android {

RTSPSource::RTSPSource(
        nsIStreamingProtocolListener *aListener,
        const char *url,
        const char *userAgent,
        bool uidValid,
        uid_t uid)
    : mURL(url),
      mUserAgent(userAgent),
      mUIDValid(uidValid),
      mUID(uid),
      mState(DISCONNECTED),
      mFinalResult(OK),
#if ANDROID_VERSION >= 23
      mDisconnectReplyToken(nullptr),
#else
      mDisconnectReplyID(0),
#endif
      mLatestPausedUnit(0),
      mPlayPending(false),
      mSeekGeneration(0),
      mDisconnectedToPauseLiveStream(false),
      mPlayOnConnected(false)
{
    CHECK(aListener != NULL);

    // Use main thread pointer, but allow access off main thread.
    mListener =
      new nsMainThreadPtrHolder<nsIStreamingProtocolListener>(aListener, false);
    mPrintCount = 0;
}

RTSPSource::~RTSPSource()
{
    if (mLooper != NULL) {
        mLooper->stop();
    }
}

void RTSPSource::start()
{
    mDisconnectedToPauseLiveStream = false;

    if (mLooper == NULL) {
        mLooper = new ALooper;
        mLooper->setName("rtsp");
        mLooper->start();

        mReflector = new AHandlerReflector<RTSPSource>(this);
        mLooper->registerHandler(mReflector);
    }

    CHECK(mHandler == NULL);

#if ANDROID_VERSION >= 23
    sp<AMessage> notify = new AMessage(kWhatNotify, mReflector);
#else
    sp<AMessage> notify = new AMessage(kWhatNotify, mReflector->id());
#endif

    mHandler = new RtspConnectionHandler(mURL.c_str(), mUserAgent.c_str(),
                                         notify, mUIDValid, mUID);
    mLooper->registerHandler(mHandler);

    CHECK_EQ(mState, (int)DISCONNECTED);
    mState = CONNECTING;
    mHandler->connect();
}

void RTSPSource::stop()
{
    if (mState == DISCONNECTED) {
        return;
    }
#if ANDROID_VERSION >= 23
    sp<AMessage> msg = new AMessage(kWhatDisconnect, mReflector);
#else
    sp<AMessage> msg = new AMessage(kWhatDisconnect, mReflector->id());
#endif

    sp<AMessage> dummy;
    msg->postAndAwaitResponse(&dummy);
}

void RTSPSource::play()
{
    LOGI("RTSPSource::play()");
    uint64_t playTimeUs = 0;
#if ANDROID_VERSION >= 23
    sp<AMessage> msg = new AMessage(kWhatPerformPlay, mReflector);
#else
    sp<AMessage> msg = new AMessage(kWhatPerformPlay, mReflector->id());
#endif
    msg->setInt64("timeUs", playTimeUs);
    msg->post();
}

void RTSPSource::pause()
{
    LOGI("RTSPSource::pause()");

    // Live streams can't be paused, so we have to disconnect now.
    if (isLiveStream()) {
        mDisconnectedToPauseLiveStream = true;
        stop();
        return;
    }

#if ANDROID_VERSION >= 23
    sp<AMessage> msg = new AMessage(kWhatPerformPause, mReflector);
#else
    sp<AMessage> msg = new AMessage(kWhatPerformPause, mReflector->id());
#endif
    msg->post();
}

void RTSPSource::resume()
{
    LOGI("RTSPSource::resume()");
#if ANDROID_VERSION >= 23
    sp<AMessage> msg = new AMessage(kWhatPerformResume, mReflector);
#else
    sp<AMessage> msg = new AMessage(kWhatPerformResume, mReflector->id());
#endif
    msg->post();
}

void RTSPSource::suspend()
{
    LOGI("RTSPSource::suspend()");
#if ANDROID_VERSION >= 23
    sp<AMessage> msg = new AMessage(kWhatPerformSuspend, mReflector);
#else
    sp<AMessage> msg = new AMessage(kWhatPerformSuspend, mReflector->id());
#endif
    msg->post();
}

void RTSPSource::seek(uint64_t timeUs)
{
    LOGI("RTSPSource::seek() %llu", timeUs);
    seekTo(timeUs);
}

void RTSPSource::playbackEnded()
{
    LOGI("RTSPSource::playbackEnded()");
#if ANDROID_VERSION >= 23
    sp<AMessage> msg = new AMessage(kWhatPerformPlaybackEnded, mReflector);
#else
    sp<AMessage> msg = new AMessage(kWhatPerformPlaybackEnded, mReflector->id());
#endif
    msg->post();
}

status_t RTSPSource::feedMoreTSData() {
    return mFinalResult;
}

sp<MetaData> RTSPSource::getFormat(bool audio) {
    sp<AnotherPacketSource> source = getSource(audio);

    if (source == NULL) {
        return NULL;
    }

    return source->getFormat();
}

status_t RTSPSource::dequeueAccessUnit(
        bool audio, sp<ABuffer> *accessUnit) {
    sp<AnotherPacketSource> source = getSource(audio);

    if (source == NULL) {
        return -EWOULDBLOCK;
    }

    status_t finalResult;
    if (!source->hasBufferAvailable(&finalResult)) {
        return finalResult == OK ? -EWOULDBLOCK : finalResult;
    }

    return source->dequeueAccessUnit(accessUnit);
}

sp<AnotherPacketSource> RTSPSource::getSource(bool audio) {
    return audio ? mAudioTrack : mVideoTrack;
}

status_t RTSPSource::getDuration(int64_t *durationUs) {
    *durationUs = 0ll;

    int64_t audioDurationUs;
    if (mAudioTrack != NULL
            && mAudioTrack->getFormat()->findInt64(
                kKeyDuration, &audioDurationUs)
            && audioDurationUs > *durationUs) {
        *durationUs = audioDurationUs;
    }

    int64_t videoDurationUs;
    if (mVideoTrack != NULL
            && mVideoTrack->getFormat()->findInt64(
                kKeyDuration, &videoDurationUs)
            && videoDurationUs > *durationUs) {
        *durationUs = videoDurationUs;
    }

    return OK;
}

status_t RTSPSource::seekTo(int64_t seekTimeUs) {
#if ANDROID_VERSION >= 23
    sp<AMessage> msg = new AMessage(kWhatPerformSeek, mReflector);
#else
    sp<AMessage> msg = new AMessage(kWhatPerformSeek, mReflector->id());
#endif
    msg->setInt32("generation", ++mSeekGeneration);
    msg->setInt64("timeUs", seekTimeUs);
    // The original code in Android posts this message for 200ms delay in order
    // to avoid performing multiple seeks in a short period of time. This is not
    // necessary for us because MediaDecoderStateMachine already circumvents
    // that situation.
    msg->post();

    return OK;
}

void RTSPSource::performPlay(int64_t playTimeUs) {
    if (mState == DISCONNECTED) {
        LOGI("We are in a idle state, restart play");
        mPlayOnConnected = true;
        start();
        return;
    }
    // If state is PAUSING, which means a previous PAUSE request is still being
    // processed, put off the PLAY request until PausedDone.
    if (mState == PAUSING) {
        mPlayPending = true;
        return;
    }
    // Reject invalid state transition.
    if (mState != CONNECTED && mState != PAUSED) {
        return;
    }
    // Use the latest received frame time if we were paused.
    if (mState == PAUSED) {
        playTimeUs = mLatestPausedUnit;
    }

    int64_t duration = 0;
    getDuration(&duration);
    MOZ_ASSERT(duration == 0 || playTimeUs < duration,
               "Should never receive an out of bounds play time!");
    if (duration > 0 && playTimeUs >= duration) {
        // if not a live stream and play time out of bounds
        return;
    }

    LOGI("performPlay : duration=%lld playTimeUs=%lld", duration, playTimeUs);
    mState = PLAYING;
    mHandler->play(playTimeUs);
}

void RTSPSource::performPause() {
    // Reject invalid state transition.
    if (mState != PLAYING) {
        return;
    }
    LOGI("performPause :");
    for (size_t i = 0; i < mTracks.size(); ++i) {
        TrackInfo *info = &mTracks.editItemAt(i);
        info->mLatestPausedUnit = 0;
    }
    mLatestPausedUnit = 0;

    mState = PAUSING;
    mHandler->pause();
}

void RTSPSource::performResume() {
// TODO, Bug 895753.
}

void RTSPSource::performSuspend() {
// TODO, Bug 895753.
}

void RTSPSource::performPlaybackEnded() {
    // Reject invalid state transition.
    if (mState != PLAYING) {
        return;
    }
    // Transition from PLAYING to CONNECTED state so that we are ready to
    // perform an another play operation.
    mState = CONNECTED;
}

void RTSPSource::performSeek(int64_t seekTimeUs) {
    if (mState != CONNECTED && mState != PLAYING && mState != PAUSED) {
        return;
    }

    int64_t duration = 0;
    getDuration(&duration);
    MOZ_ASSERT(seekTimeUs < duration,
               "Should never receive an out of bounds seek time!");
    if (seekTimeUs >= duration) {
        return;
    }

    for (size_t i = 0; i < mTracks.size(); ++i) {
        TrackInfo *info = &mTracks.editItemAt(i);
        info->mLatestPausedUnit = 0;
    }
    mLatestPausedUnit = 0;

    LOGI("performSeek: %llu", seekTimeUs);
    mState = SEEKING;
    mHandler->seek(seekTimeUs);
}

bool RTSPSource::isSeekable() {
    return true;
}

void RTSPSource::onMessageReceived(const sp<AMessage> &msg) {
    if (msg->what() == kWhatDisconnect) {
#if ANDROID_VERSION >= 23
        android::sp<android::AReplyToken> replyToken;
        CHECK(msg->senderAwaitsResponse(&replyToken));

        mDisconnectReplyToken = replyToken;
#else
        uint32_t replyID;
        CHECK(msg->senderAwaitsResponse(&replyID));

        mDisconnectReplyID = replyID;
#endif
        finishDisconnectIfPossible();
        return;
    } else if (msg->what() == kWhatPerformSeek) {
        int32_t generation;
        CHECK(msg->findInt32("generation", &generation));

        if (generation != mSeekGeneration) {
            // obsolete.
            return;
        }

        int64_t seekTimeUs;
        CHECK(msg->findInt64("timeUs", &seekTimeUs));

        performSeek(seekTimeUs);
        return;
    } else if (msg->what() == kWhatPerformPlay) {
        int64_t playTimeUs;
        CHECK(msg->findInt64("timeUs", &playTimeUs));
        performPlay(playTimeUs);
        return;
    } else if (msg->what() == kWhatPerformPause) {
        performPause();
        return;
    } else if (msg->what() == kWhatPerformResume) {
        performResume();
        return;
    } else if (msg->what() == kWhatPerformSuspend) {
        performSuspend();
        return;
    } else if (msg->what() == kWhatPerformPlaybackEnded) {
        performPlaybackEnded();
        return;
    }

    CHECK_EQ(msg->what(), (uint32_t)kWhatNotify);

    int32_t what;
    int32_t isSeekable = 0;
    CHECK(msg->findInt32("what", &what));

    switch (what) {
        case RtspConnectionHandler::kWhatConnected:
            CHECK(msg->findInt32("isSeekable", &isSeekable));
            onConnected((isSeekable ? true:false));
            break;

        case RtspConnectionHandler::kWhatDisconnected:
            onDisconnected(msg);
            break;

        case RtspConnectionHandler::kWhatSeekDone:
        {
            mState = PLAYING;
            // Even if we have reset mLatestPausedUnit in performSeek(),
            // it's still possible that kWhatPausedDone event may arrive
            // because of previous performPause() command.
            for (size_t i = 0; i < mTracks.size(); ++i) {
                TrackInfo *info = &mTracks.editItemAt(i);
                info->mLatestPausedUnit = 0;
            }
            mLatestPausedUnit = 0;
            break;
        }

        case RtspConnectionHandler::kWhatPausedDone:
        {
            // Reject invalid state transition.
            if (mState != PAUSING) {
                return;
            }
            mState = PAUSED;

            for (size_t i = 0; i < mTracks.size(); ++i) {
                TrackInfo *info = &mTracks.editItemAt(i);
                info->mLatestPausedUnit = info->mLatestReceivedUnit;
            }

            // The timestamp after a 'Pause' is done is the earliest
            // timestamp among all of the latest received units.
            TrackInfo *info = &mTracks.editItemAt(0);
            mLatestPausedUnit = info->mLatestReceivedUnit;
            for (size_t i = 1; i < mTracks.size(); ++i) {
                TrackInfo *info = &mTracks.editItemAt(i);
                if (mLatestPausedUnit > info->mLatestReceivedUnit) {
                    mLatestPausedUnit = info->mLatestReceivedUnit;
                }
            }

            if (mPlayPending) {
                mPlayPending = false;
                performPlay(mLatestPausedUnit);
            }
            break;
        }

        case RtspConnectionHandler::kWhatAccessUnitComplete:
        {
            if (!isValidState()) {
                LOGI("We're disconnected, dropping access unit.");
                break;
            }
            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));
            CHECK_LT(trackIndex, mTracks.size());

            sp<RefBase> obj;
            CHECK(msg->findObject("accessUnit", &obj));

            sp<ABuffer> accessUnit = static_cast<ABuffer *>(obj.get());

            int32_t damaged;
            if (accessUnit->meta()->findInt32("damaged", &damaged)
                    && damaged) {
                LOGI("dropping damaged access unit.");
                break;
            }

            TrackInfo *info = &mTracks.editItemAt(trackIndex);

            sp<AnotherPacketSource> source = info->mSource;
            if (source != NULL) {
                uint32_t rtpTime;
                CHECK(accessUnit->meta()->findInt32("rtp-time", (int32_t *)&rtpTime));

                if (!info->mNPTMappingValid) {
                    // This is a live stream, we didn't receive any normal
                    // playtime mapping. Assume the first packets correspond
                    // to time 0.

                    LOGV("This is a live stream, assuming time = 0");

                    info->mRTPTime = rtpTime;
                    info->mNormalPlaytimeUs = 0ll;
                    info->mNPTMappingValid = true;
                }

                int64_t nptUs =
                    ((double)rtpTime - (double)info->mRTPTime)
                        / info->mTimeScale
                        * 1000000ll
                        + info->mNormalPlaytimeUs;

                accessUnit->meta()->setInt64("timeUs", nptUs);
                info->mLatestReceivedUnit = nptUs;
                // Drop the frames that are older than the frames in the queue.
                if (info->mLatestPausedUnit && (int64_t)info->mLatestPausedUnit > nptUs) {
                    break;
                }
                source->queueAccessUnit(accessUnit);
            }

            onTrackDataAvailable(trackIndex);
            break;
        }

        case RtspConnectionHandler::kWhatEOS:
        {
            if (!isValidState()) {
                LOGI("We're disconnected, dropping end-of-stream message.");
                break;
            }
            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));
            CHECK_LT(trackIndex, mTracks.size());

            int32_t finalResult;
            CHECK(msg->findInt32("finalResult", &finalResult));
            CHECK_NE(finalResult, (status_t)OK);

            TrackInfo *info = &mTracks.editItemAt(trackIndex);
            sp<AnotherPacketSource> source = info->mSource;
            if (source != NULL) {
                source->signalEOS(finalResult);
            }

            onTrackEndOfStream(trackIndex);
            break;
        }

        case RtspConnectionHandler::kWhatSeekDiscontinuity:
        {
            if (!isValidState()) {
                LOGI("We're disconnected, dropping seek discontinuity message.");
                break;
            }
            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));
            CHECK_LT(trackIndex, mTracks.size());

            TrackInfo *info = &mTracks.editItemAt(trackIndex);
            sp<AnotherPacketSource> source = info->mSource;
            if (source != NULL) {
#if ANDROID_VERSION >= 21
                source->queueDiscontinuity(ATSParser::DISCONTINUITY_TIME, NULL,
                                           true /* discard */);
#else
                source->queueDiscontinuity(ATSParser::DISCONTINUITY_SEEK, NULL);
#endif
            }

            break;
        }

        case RtspConnectionHandler::kWhatNormalPlayTimeMapping:
        {
            if (!isValidState()) {
                LOGI("We're disconnected, dropping normal play time mapping "
                     "message.");
                break;
            }
            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));
            CHECK_LT(trackIndex, mTracks.size());

            uint32_t rtpTime;
            CHECK(msg->findInt32("rtpTime", (int32_t *)&rtpTime));

            int64_t nptUs;
            CHECK(msg->findInt64("nptUs", &nptUs));

            TrackInfo *info = &mTracks.editItemAt(trackIndex);
            info->mRTPTime = rtpTime;
            info->mNormalPlaytimeUs = nptUs;
            info->mNPTMappingValid = true;
            break;
        }

        case RtspConnectionHandler::kWhatTryTCPInterleaving:
        {
            // By default, we will request to deliver RTP over UDP. If the play
            // request timed out and we didn't receive any RTP packet, we will
            // fail back to use RTP interleaved in the existing RTSP/TCP
            // connection. And in this case, we have to explicitly perform
            // another play event to request the server to start streaming
            // again.
            int64_t playTimeUs;
            if (!msg->findInt64("timeUs", &playTimeUs)) {
                playTimeUs = 0;
            }
            performPlay(playTimeUs);
            break;
        }

        default:
            TRESPASS();
    }
}

void RTSPSource::onConnected(bool isSeekable)
{
    CHECK(mHandler != NULL);
    CHECK(mListener != NULL);

    // Clean up audio and video tracks.
    // In the case of RTSP reconnect, audio/video tracks might be created by
    // previous onConnected event.
    mAudioTrack = NULL;
    mVideoTrack = NULL;
    mTracks.clear();

    size_t numTracks = mHandler->countTracks();
    for (size_t i = 0; i < numTracks; ++i) {
        int32_t timeScale;
        sp<MetaData> format = mHandler->getTrackFormat(i, &timeScale);
        CHECK(format != NULL);

        const char *mime;
        CHECK(format->findCString(kKeyMIMEType, &mime));
        nsAutoCString mimeType(mime, strlen(mime));

        bool isAudio = !strncasecmp(mime, "audio/", 6);
        bool isVideo = !strncasecmp(mime, "video/", 6);

        TrackInfo info;
        info.mTimeScale          = timeScale;
        info.mRTPTime            = 0;
        info.mNormalPlaytimeUs   = 0ll;
        info.mNPTMappingValid    = false;
        info.mIsAudio            = false;
        info.mLatestReceivedUnit = 0;
        info.mLatestPausedUnit   = 0;

        if ((isAudio && mAudioTrack == NULL)
                || (isVideo && mVideoTrack == NULL)) {
            sp<AnotherPacketSource> source = new AnotherPacketSource(format);

            if (isAudio) {
                info.mIsAudio = true;
                mAudioTrack = source;
            } else {
                info.mIsAudio = false;
                mVideoTrack = source;
            }

            info.mSource = source;
        }

        RefPtr<nsIStreamingProtocolMetaData> meta;
        int32_t int32Value;
        int64_t int64Value;

        meta = new mozilla::net::RtspMetaData();
        meta->SetTotalTracks(numTracks);
        meta->SetMimeType(mimeType);

        DebugOnly<bool> success;
        success = format->findInt64(kKeyDuration, &int64Value);
        MOZ_ASSERT(success);
        meta->SetDuration(int64Value);

        if (isAudio) {
            success = format->findInt32(kKeyChannelCount, &int32Value);
            MOZ_ASSERT(success);
            meta->SetChannelCount(int32Value);

            success = format->findInt32(kKeySampleRate, &int32Value);
            MOZ_ASSERT(success);
            meta->SetSampleRate(int32Value);
        } else {
            success = format->findInt32(kKeyWidth, &int32Value);
            MOZ_ASSERT(success);
            meta->SetWidth(int32Value);

            success = format->findInt32(kKeyHeight, &int32Value);
            MOZ_ASSERT(success);
            meta->SetHeight(int32Value);
        }

        // Optional meta data.
        const void *data;
        uint32_t type;
        size_t length = 0;

        if (format->findData(kKeyESDS, &type, &data, &length)) {
            nsCString esds;
            esds.Assign((const char *)data, length);
            meta->SetEsdsData(esds);
        }

        if (format->findData(kKeyAVCC, &type, &data, &length)) {
            nsCString avcc;
            avcc.Assign((const char *)data, length);
            meta->SetAvccData(avcc);
        }

        mListener->OnConnected(i, meta.get());
        mTracks.push(info);
    }

    mState = CONNECTED;

    if (mPlayOnConnected) {
        mPlayOnConnected = false;
        play();
    }
}

void RTSPSource::onDisconnected(const sp<AMessage> &msg) {
    status_t err;
    CHECK(msg != NULL);
    CHECK(msg->findInt32("result", &err));

    if ((mLooper != NULL) && (mHandler != NULL)) {
        mLooper->unregisterHandler(mHandler->id());
        mHandler.clear();
    }

    mState = DISCONNECTED;
    mFinalResult = err;

#if ANDROID_VERSION >= 23
    if (mDisconnectReplyToken.get()) {
        finishDisconnectIfPossible();
    }
#else
    if (mDisconnectReplyID != 0) {
        finishDisconnectIfPossible();
    }
#endif
    // If the disconnection is caused by pausing live stream,
    // do not report back to the controller.
    if (mListener && !mDisconnectedToPauseLiveStream) {
        nsresult reason = (err == OK) ? NS_OK : NS_ERROR_NET_TIMEOUT;
        mListener->OnDisconnected(0, reason);
        // Break the cycle reference between RtspController and us.
        mListener = nullptr;
    }
    mAudioTrack = NULL;
    mVideoTrack = NULL;
    mTracks.clear();
}

void RTSPSource::finishDisconnectIfPossible() {
    if (mState != DISCONNECTED) {
        CHECK(mHandler != NULL);
        mHandler->disconnect();
    }

#if ANDROID_VERSION >= 23
    (new AMessage)->postReply(mDisconnectReplyToken);
    mDisconnectReplyToken.clear();
#else
    (new AMessage)->postReply(mDisconnectReplyID);
    mDisconnectReplyID = 0;
#endif
}

void RTSPSource::onTrackDataAvailable(size_t trackIndex)
{
    nsCString data;
    sp<ABuffer> accessUnit;
    TrackInfo *info = &mTracks.editItemAt(trackIndex);

    if (mState != PLAYING) {
        return;
    }

    status_t err = dequeueAccessUnit(info->mIsAudio, &accessUnit);

    if (err == -EWOULDBLOCK || err == ERROR_END_OF_STREAM) {
        return;
    } else if (err == INFO_DISCONTINUITY) {
        RefPtr<nsIStreamingProtocolMetaData> meta;

        meta = new mozilla::net::RtspMetaData();
        meta->SetFrameType(MEDIASTREAM_FRAMETYPE_DISCONTINUITY);

        data.AssignLiteral("DISCONTINUITY");
        if (mListener) {
            mListener->OnMediaDataAvailable(trackIndex, data, data.Length(), 0, meta.get());
        }
        LOGV("err trackIndex[%d]:INFO_DISCONTINUITY", trackIndex);
        return;
    }

    RefPtr<nsIStreamingProtocolMetaData> meta;
    int64_t int64Value;

    meta = new mozilla::net::RtspMetaData();

    MOZ_ASSERT(accessUnit != NULL);
    DebugOnly<bool> success = accessUnit->meta()->findInt64("timeUs", &int64Value);
    MOZ_ASSERT(success);
    meta->SetTimeStamp(int64Value);

    meta->SetFrameType(MEDIASTREAM_FRAMETYPE_NORMAL);
    data.Assign((const char *) accessUnit->data(), accessUnit->size());

    if (mListener) {
        mListener->OnMediaDataAvailable(trackIndex, data, data.Length(), 0, meta.get());
    }
}

void RTSPSource::onTrackEndOfStream(size_t trackIndex)
{
    // If we are disconnecting to pretend pausing a live stream,
    // do not report the end of stream.
    if (!mListener || mDisconnectedToPauseLiveStream) {
        return;
    }

    RefPtr<nsIStreamingProtocolMetaData> meta;
    meta = new mozilla::net::RtspMetaData();
    meta->SetFrameType(MEDIASTREAM_FRAMETYPE_END_OF_STREAM);

    nsCString data;
    data.AssignLiteral("END_OF_STREAM");

    mListener->OnMediaDataAvailable(trackIndex, data, data.Length(), 0, meta.get());
}

inline bool RTSPSource::isValidState()
{
    if (mState == DISCONNECTED || mTracks.size() == 0) {
        return false;
    }
    return true;
}



bool RTSPSource::isLiveStream() {
    int64_t duration = 0;
    getDuration(&duration);
    return duration == 0;
}

}  // namespace android
