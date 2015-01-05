/*
 * Copyright (c) 2013 - 2016, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "AVUtils"
#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <cutils/properties.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/MediaCodec.h>

#include "QComOMXMetadata.h"

#include <binder/IPCThreadState.h>
#include <camera/CameraParameters.h>
#include <inttypes.h>

#include "common/ExtensionsLoader.hpp"
#include "stagefright/AVExtensions.h"

namespace android {

status_t AVUtils::convertMetaDataToMessage(
        const sp<MetaData> &, sp<AMessage> *) {
    return OK;
}

status_t AVUtils::convertMessageToMetaData(
        const sp<AMessage> &, sp<MetaData> &) {
    return OK;
}

status_t AVUtils::mapMimeToAudioFormat(
        audio_format_t&, const char* ) {
    return OK;
}

status_t AVUtils::sendMetaDataToHal(
        const sp<MetaData>&, AudioParameter *){
    return OK;
}

bool AVUtils::hasAudioSampleBits(const sp<MetaData> &) {
    return false;
}

bool AVUtils::hasAudioSampleBits(const sp<AMessage> &) {
    return false;
}

int AVUtils::getAudioSampleBits(const sp<MetaData> &) {
    return 16;
}

int AVUtils::getAudioSampleBits(const sp<AMessage> &format) {
    AudioEncoding encoding = kAudioEncodingPcm16bit;
    format->findInt32("pcm-encoding", (int32_t*)&encoding);
    return audioEncodingToBits(encoding);
}

audio_format_t AVUtils::updateAudioFormat(audio_format_t audioFormat,
        const sp<MetaData> &){
    return audioFormat;
}

audio_format_t AVUtils::updateAudioFormat(audio_format_t audioFormat,
        const sp<AMessage> &){
    return audioFormat;
}

static bool dumbSniffer(
        const sp<DataSource> &, String8 *,
        float *, sp<AMessage> *) {
    return false;
}

DataSource::SnifferFunc AVUtils::getExtendedSniffer() {
    return dumbSniffer;
}

sp<MediaCodec> AVUtils::createCustomComponentByName(
           const sp<ALooper> &, const char* , bool, const sp<AMessage> &) {
               return NULL;
}

int32_t AVUtils::getAudioMaxInputBufferSize(audio_format_t, const sp<AMessage> &) {
    return 0;
}

bool AVUtils::mapAACProfileToAudioFormat(const sp<MetaData> &, audio_format_t &,
                 uint64_t  /*eAacProfile*/) {
    return false ;
}

bool AVUtils::mapAACProfileToAudioFormat(const sp<AMessage> &,  audio_format_t &,
                 uint64_t  /*eAacProfile*/) {
    return false ;
}

bool AVUtils::canOffloadAPE(const sp<MetaData> &) {
   return true;
}

bool AVUtils::isEnhancedExtension(const char *) {
    return false;
}


bool AVUtils::isAudioMuxFormatSupported(const char * mime) {
    if (mime == NULL) {
        ALOGE("NULL audio mime type");
        return false;
    }

    if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_NB, mime)
            || !strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_WB, mime)
            || !strcasecmp(MEDIA_MIMETYPE_AUDIO_AAC, mime)) {
        return true;
    }
    return false;
}

void AVUtils::cacheCaptureBuffers(sp<hardware::ICamera> camera, video_encoder encoder) {
    if (camera != NULL) {
        char mDeviceName[PROPERTY_VALUE_MAX];
        property_get("ro.board.platform", mDeviceName, "0");
        if (!strncmp(mDeviceName, "msm8909", 7)) {
            int64_t token = IPCThreadState::self()->clearCallingIdentity();
            String8 s = camera->getParameters();
            CameraParameters params(s);
            const char *enable;
            if (encoder == VIDEO_ENCODER_H263 ||
                encoder == VIDEO_ENCODER_MPEG_4_SP) {
                enable = "1";
            } else {
                enable = "0";
            }
            params.set("cache-video-buffers", enable);
            if (camera->setParameters(params.flatten()) != OK) {
                ALOGE("Failed to enabled cached camera buffers");
            }
            IPCThreadState::self()->restoreCallingIdentity(token);
        }
    }
}

const char *AVUtils::getCustomCodecsLocation() {
    return "/etc/media_codecs.xml";
}

void AVUtils::setIntraPeriod(
        int, int, const sp<IOMX>,
        IOMX::node_id) {
    return;
}

const char *AVUtils::getCustomCodecsPerformanceLocation() {
    return "/etc/media_codecs_performance.xml";
}

bool AVUtils::IsHevcIDR(const sp<ABuffer> &) {
   return false;
}

void AVUtils::addDecodingTimesFromBatch(MediaBuffer *buf,
        List<int64_t>& decodeTimeQueue) {
#ifdef QCOM_HARDWARE
    if (buf == NULL || (buf->size() != sizeof(encoder_media_buffer_type))) {
        return;
    }
    encoder_media_buffer_type *metaBuf = (encoder_media_buffer_type*)(buf->data());
    if (!metaBuf || metaBuf->buffer_type != kMetadataBufferTypeCameraSource) {
        return;
    }
    native_handle_t *hnd = (native_handle_t *)metaBuf->meta_handle;
    if (hnd) {
        int numBufs = hnd->numFds;
        // Only 1 fd => either the batch has single buffer or is not a batch
        // do nothing in this case as first timestamp is already added to TS queue
        if (numBufs < 2) {
            return;
        }
        ALOGV("Found batch of %d bufs", numBufs);
        // There must be at-least '3 x fd' ints in the handle
        // Payload format for batch-of-2 -> |fd0|fd1|off0|off1|len0|len1|dTS0|dTS1|
        if (hnd->numInts < hnd->numFds * 3) {
            ALOGE("HFR: batch-of-%d has insufficient ints %d vs %d",
                    hnd->numFds, hnd->numInts, hnd->numFds * 3);
            return;
        }
        int64_t baseTimeUs = 0ll;
        int64_t timeUs = 0ll;
        int tsStartIndex = numBufs * 3;
        buf->meta_data()->findInt64(kKeyTime, &baseTimeUs);
        // starting from 2nd buffer (i = 1) since TS for first buffer is already added
        for (int i = 1; i < numBufs; ++i) {
            // timestamp differences from Camera are in nano-seconds
            timeUs = baseTimeUs + hnd->data[tsStartIndex + i] / 1E3;
            ALOGV("dTs=%f Ts=%" PRId64, hnd->data[tsStartIndex + i] / 1E3, timeUs);
            decodeTimeQueue.push_back(timeUs);
        }
    }
#else
    (void)buf;
    (void)decodeTimeQueue;
#endif
}

// ----- NO TRESSPASSING BEYOND THIS LINE ------
AVUtils::AVUtils() {}

AVUtils::~AVUtils() {}

//static
AVUtils *AVUtils::sInst =
        ExtensionsLoader<AVUtils>::createInstance("createExtendedUtils");

} //namespace android

