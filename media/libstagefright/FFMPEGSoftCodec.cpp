/*
 * Copyright (C) 2014 The CyanogenMod Project
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

#ifdef __LP64__
#define OMX_ANDROID_COMPILE_AS_32BIT_ON_64BIT_PLATFORMS
#endif

//#define LOG_NDEBUG 0
#define LOG_TAG "FFMPEGSoftCodec"
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ABitReader.h>

#include <media/stagefright/FFMPEGSoftCodec.h>

#include <media/stagefright/ACodec.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

#include <cutils/properties.h>

#include <OMX_Component.h>
#include <OMX_AudioExt.h>
#include <OMX_IndexExt.h>

#include <OMX_FFMPEG_Extn.h>

#include <cutils/properties.h>
#include <dlfcn.h>

#ifdef QCOM_HARDWARE
#include <OMX_QCOMExtns.h>
#endif

namespace android {

enum MetaKeyType{
    INT32, INT64, STRING, DATA, CSD
};

struct MetaKeyEntry{
    int MetaKey;
    const char* MsgKey;
    MetaKeyType KeyType;
};

static const MetaKeyEntry MetaKeyTable[] {
   {kKeyAACAOT               , "aac-profile"            , INT32},
   {kKeyArbitraryMode        , "use-arbitrary-mode"     , INT32},
   {kKeyBitsPerRawSample     , "bits-per-raw-sample"    , INT32},
   {kKeyBitRate              , "bitrate"                , INT32},
   {kKeyBlockAlign           , "block-align"            , INT32},
   {kKeyChannelCount         , "channel-count"          , INT32},
   {kKeyCodecId              , "codec-id"               , INT32},
   {kKeyCodedSampleBits      , "coded-sample-bits"      , INT32},
   {kKeyFileFormat           , "file-format"            , INT32},
   {kKeyRawCodecSpecificData , "raw-codec-specific-data", CSD},
   {kKeyPcmEncoding          , "pcm-encoding"           , INT32},
   {kKeyRVVersion            , "rv-version"             , INT32},
   {kKeySampleFormat         , "sample-format"          , INT32},
   {kKeySampleRate           , "sample-rate"            , INT32},
   {kKeyWMAVersion           , "wma-version"            , INT32},  // int32_t
   {kKeyWMVVersion           , "wmv-version"            , INT32},
   {kKeyDivXVersion          , "divx-version"           , INT32},
   {kKeyThumbnailTime        , "thumbnail-time"         , INT64},
};

const char* FFMPEGSoftCodec::getMsgKey(int key) {
    static const size_t numMetaKeys =
                     sizeof(MetaKeyTable) / sizeof(MetaKeyTable[0]);
    size_t i;
    for (i = 0; i < numMetaKeys; ++i) {
        if (key == MetaKeyTable[i].MetaKey) {
            return MetaKeyTable[i].MsgKey;
        }
    }
    return "unknown";
}

void FFMPEGSoftCodec::convertMetaDataToMessageFF(
        const sp<MetaData> &meta, sp<AMessage> *format) {
    const char * str_val;
    int32_t int32_val;
    int64_t int64_val;
    uint32_t data_type;
    const void * data;
    size_t size;
    static const size_t numMetaKeys =
                     sizeof(MetaKeyTable) / sizeof(MetaKeyTable[0]);
    size_t i;
    for (i = 0; i < numMetaKeys; ++i) {
        if (MetaKeyTable[i].KeyType == INT32 &&
            meta->findInt32(MetaKeyTable[i].MetaKey, &int32_val)) {
            ALOGV("found metakey %s of type int32", MetaKeyTable[i].MsgKey);
            format->get()->setInt32(MetaKeyTable[i].MsgKey, int32_val);
        } else if (MetaKeyTable[i].KeyType == INT64 &&
                 meta->findInt64(MetaKeyTable[i].MetaKey, &int64_val)) {
            ALOGV("found metakey %s of type int64", MetaKeyTable[i].MsgKey);
            format->get()->setInt64(MetaKeyTable[i].MsgKey, int64_val);
        } else if (MetaKeyTable[i].KeyType == STRING &&
                 meta->findCString(MetaKeyTable[i].MetaKey, &str_val)) {
            ALOGV("found metakey %s of type string", MetaKeyTable[i].MsgKey);
            format->get()->setString(MetaKeyTable[i].MsgKey, str_val);
        } else if ( (MetaKeyTable[i].KeyType == DATA ||
                   MetaKeyTable[i].KeyType == CSD) &&
                   meta->findData(MetaKeyTable[i].MetaKey, &data_type, &data, &size)) {
            ALOGV("found metakey %s of type data", MetaKeyTable[i].MsgKey);
            if (MetaKeyTable[i].KeyType == CSD) {
                const char *mime;
                CHECK(meta->findCString(kKeyMIMEType, &mime));
                if (strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC)) {
                    sp<ABuffer> buffer = new ABuffer(size);
                    memcpy(buffer->data(), data, size);
                    buffer->meta()->setInt32("csd", true);
                    buffer->meta()->setInt64("timeUs", 0);
                    format->get()->setBuffer("csd-0", buffer);
                } else {
                    const uint8_t *ptr = (const uint8_t *)data;
                    CHECK(size >= 8);
                    int seqLength = 0, picLength = 0;
                    for (size_t i = 4; i < (size - 4); i++)
                    {
                        if ((*(ptr + i) == 0) && (*(ptr + i + 1) == 0) &&
                           (*(ptr + i + 2) == 0) && (*(ptr + i + 3) == 1))
                            seqLength = i;
                    }
                    sp<ABuffer> buffer = new ABuffer(seqLength);
                    memcpy(buffer->data(), data, seqLength);
                    buffer->meta()->setInt32("csd", true);
                    buffer->meta()->setInt64("timeUs", 0);
                    format->get()->setBuffer("csd-0", buffer);
                    picLength=size-seqLength;
                    sp<ABuffer> buffer1 = new ABuffer(picLength);
                    memcpy(buffer1->data(), (const uint8_t *)data + seqLength, picLength);
                    buffer1->meta()->setInt32("csd", true);
                    buffer1->meta()->setInt64("timeUs", 0);
                    format->get()->setBuffer("csd-1", buffer1);
                }
            } else {
                sp<ABuffer> buffer = new ABuffer(size);
                memcpy(buffer->data(), data, size);
                format->get()->setBuffer(MetaKeyTable[i].MsgKey, buffer);
            }
        }
    }
}

void FFMPEGSoftCodec::convertMessageToMetaDataFF(
        const sp<AMessage> &msg, sp<MetaData> &meta) {
    AString str_val;
    int32_t int32_val;
    int64_t int64_val;
    static const size_t numMetaKeys =
                     sizeof(MetaKeyTable) / sizeof(MetaKeyTable[0]);
    size_t i;
    for (i = 0; i < numMetaKeys; ++i) {
        if (MetaKeyTable[i].KeyType == INT32 &&
                msg->findInt32(MetaKeyTable[i].MsgKey, &int32_val)) {
            ALOGV("found metakey %s of type int32", MetaKeyTable[i].MsgKey);
            meta->setInt32(MetaKeyTable[i].MetaKey, int32_val);
        } else if (MetaKeyTable[i].KeyType == INT64 &&
                msg->findInt64(MetaKeyTable[i].MsgKey, &int64_val)) {
            ALOGV("found metakey %s of type int64", MetaKeyTable[i].MsgKey);
            meta->setInt64(MetaKeyTable[i].MetaKey, int64_val);
        } else if (MetaKeyTable[i].KeyType == STRING &&
                msg->findString(MetaKeyTable[i].MsgKey, &str_val)) {
            ALOGV("found metakey %s of type string", MetaKeyTable[i].MsgKey);
            meta->setCString(MetaKeyTable[i].MetaKey, str_val.c_str());
        }
    }
}


template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

const char* FFMPEGSoftCodec::overrideComponentName(
        uint32_t /*quirks*/, const sp<MetaData> &meta, const char *mime, bool isEncoder) {
    const char* componentName = NULL;

    int32_t wmvVersion = 0;
    if (!strncasecmp(mime, MEDIA_MIMETYPE_VIDEO_WMV, strlen(MEDIA_MIMETYPE_VIDEO_WMV)) &&
            meta->findInt32(kKeyWMVVersion, &wmvVersion)) {
        ALOGD("Found WMV version key %d", wmvVersion);
        if (wmvVersion != 2) {
            ALOGD("Use FFMPEG for unsupported WMV track");
            componentName = "OMX.ffmpeg.wmv.decoder";
        }
    }

    int32_t encodeOptions = 0;
    if (!isEncoder && !strncasecmp(mime, MEDIA_MIMETYPE_AUDIO_WMA, strlen(MEDIA_MIMETYPE_AUDIO_WMA)) &&
            !meta->findInt32(kKeyWMAEncodeOpt, &encodeOptions)) {
        ALOGD("Use FFMPEG for unsupported WMA track");
        componentName = "OMX.ffmpeg.wma.decoder";
    }

    // Google's decoder doesn't support MAIN profile
    int32_t aacProfile = 0;
    if (!isEncoder && !strncasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC, strlen(MEDIA_MIMETYPE_AUDIO_AAC)) &&
            meta->findInt32(kKeyAACAOT, &aacProfile)) {
        if ((aacProfile == OMX_AUDIO_AACObjectMain) || (aacProfile == OMX_AUDIO_AACObjectLTP)) {
            ALOGD("Use FFMPEG for AAC Main/LTP profile");
            componentName = "OMX.ffmpeg.aac.decoder";
        }
    }

    // Use FFMPEG for high-res formats which other decoders can't handle
    AudioEncoding encoding = kAudioEncodingPcm16bit;
    if (!isEncoder && meta->findInt32(kKeyPcmEncoding, (int32_t*)&encoding)) {
        if (audioEncodingToBits(encoding) > 16) {
            if (!strncasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC, strlen(MEDIA_MIMETYPE_AUDIO_AAC))) {
                componentName = "OMX.ffmpeg.aac.decoder";
                ALOGD("Use FFMPEG for high-res AAC format");
            } else if (!strncasecmp(mime, MEDIA_MIMETYPE_AUDIO_FLAC, strlen(MEDIA_MIMETYPE_AUDIO_FLAC))) {
                componentName = "OMX.ffmpeg.flac.decoder";
                ALOGD("Use FFMPEG for high-res FLAC format");
            }
        }
    }

    return componentName;
}

void FFMPEGSoftCodec::overrideComponentName(
        uint32_t quirks, const sp<AMessage> &msg, AString* componentName, AString* mime, int32_t isEncoder) {

    sp<MetaData> meta = new MetaData;
    convertMessageToMetaData(msg, meta);
    const char *updated = overrideComponentName(
                quirks, meta, mime->c_str(), isEncoder);
    if (updated != NULL) {
        componentName->setTo(updated);
    }
}

status_t FFMPEGSoftCodec::setVideoFormat(
        status_t status,
        const sp<AMessage> &msg, const char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder,
        OMX_VIDEO_CODINGTYPE *compressionFormat,
        const char* componentName) {
    status_t err = OK;

    //ALOGD("setVideoFormat: %s", msg->debugString(0).c_str());

    /* status passed in is the result of the normal codec lookup */
    if (status != OK) {

        if (isEncoder) {
            ALOGE("Encoding not supported");
            err = BAD_VALUE;

        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_WMV, mime)) {
            if (strncmp(componentName, "OMX.ffmpeg.", 11) == 0) {
                err = setWMVFormat(msg, OMXhandle, nodeID);
                if (err != OK) {
                    ALOGE("setWMVFormat() failed (err = %d)", err);
                }
            }
            *compressionFormat = OMX_VIDEO_CodingWMV;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_RV, mime)) {
            err = setRVFormat(msg, OMXhandle, nodeID);
            if (err != OK) {
                ALOGE("setRVFormat() failed (err = %d)", err);
            } else {
                *compressionFormat = OMX_VIDEO_CodingRV;
            }
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VC1, mime)) {
            *compressionFormat = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingVC1;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_FLV1, mime)) {
            *compressionFormat = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingFLV1;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime)) {
            *compressionFormat = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingDIVX;
#ifdef QCOM_HARDWARE
        // compressionFormat will be override later
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX4, mime)) {
            *compressionFormat = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingDIVX;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX311, mime)) {
            *compressionFormat = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingDIVX;
#endif
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_HEVC, mime)) {
            *compressionFormat = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingHEVC;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_FFMPEG, mime)) {
            ALOGV("Setting the OMX_VIDEO_PARAM_FFMPEGTYPE params");
            err = setFFmpegVideoFormat(msg, OMXhandle, nodeID);
            if (err != OK) {
                ALOGE("setFFmpegVideoFormat() failed (err = %d)", err);
            } else {
                *compressionFormat = OMX_VIDEO_CodingAutoDetect;
            }
        } else {
            err = BAD_TYPE;
        }
    }

#ifdef QCOM_HARDWARE
    // We need to do a few extra steps if FFMPEGExtractor is in control
    // and we want to talk to the hardware codecs. This logic is taken
    // from the CAF L release. It was unfortunately moved to a proprietary
    // blob and an architecture which is hellish for OEMs who wish to
    // customize the platform.
    if (err == OK && (!strncmp(componentName, "OMX.qcom.", 9)
        || !strncmp(componentName, "OMX.ittiam.", 11))) {
        status_t xerr = OK;


        int32_t mode = 0;
        OMX_QCOM_PARAM_PORTDEFINITIONTYPE portFmt;
        InitOMXParams(&portFmt);
        portFmt.nPortIndex = kPortIndexInput;

        if (msg->findInt32("use-arbitrary-mode", &mode) && mode) {
            ALOGI("Decoder will be in arbitrary mode");
            portFmt.nFramePackingFormat = OMX_QCOM_FramePacking_Arbitrary;
        } else {
            ALOGI("Decoder will be in frame by frame mode");
            portFmt.nFramePackingFormat = OMX_QCOM_FramePacking_OnlyOneCompleteFrame;
        }
        xerr = OMXhandle->setParameter(
                nodeID, (OMX_INDEXTYPE)OMX_QcomIndexPortDefn,
                (void *)&portFmt, sizeof(portFmt));
        if (xerr != OK) {
            ALOGW("Failed to set frame packing format on component");
        }

        if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime) ||
                !strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX4, mime) ||
                !strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX311, mime)) {
            // Override with QCOM specific compressionFormat
            *compressionFormat = (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingDivx;
            setQCDIVXFormat(msg, mime, OMXhandle, nodeID, kPortIndexOutput);
        }

        // Enable timestamp reordering for mpeg4 and vc1 codec types, the AVI file
        // type, and hevc content in the ts container
        AString container;
        const char * containerStr = NULL;
        if (msg->findString("file-format", &container)) {
            containerStr = container.c_str();
        }

        bool tsReorder = false;
        const char* roleVC1 = "OMX.qcom.video.decoder.vc1";
        const char* roleMPEG4 = "OMX.qcom.video.decoder.mpeg4";
        const char* roleHEVC = "OMX.qcom.video.decoder.hevc";
        if (!strncmp(componentName, roleVC1, strlen(roleVC1)) ||
                !strncmp(componentName, roleMPEG4, strlen(roleMPEG4))) {
            // The codec requires timestamp reordering
            tsReorder = true;
        } else if (containerStr != NULL) {
            if (!strncmp(containerStr, MEDIA_MIMETYPE_CONTAINER_AVI,
                    strlen(MEDIA_MIMETYPE_CONTAINER_AVI))) {
                tsReorder = true;
            } else if (!strncmp(containerStr, MEDIA_MIMETYPE_CONTAINER_MPEG2TS,
                        strlen(MEDIA_MIMETYPE_CONTAINER_MPEG2TS)) ||
                       !strncmp(componentName, roleHEVC, strlen(roleHEVC))) {
                tsReorder = true;
            }
        }

        if (tsReorder) {
            ALOGI("Enabling timestamp reordering");
            QOMX_INDEXTIMESTAMPREORDER reorder;
            InitOMXParams(&reorder);
            reorder.nPortIndex = kPortIndexOutput;
            reorder.bEnable = OMX_TRUE;
            xerr = OMXhandle->setParameter(nodeID,
                           (OMX_INDEXTYPE)OMX_QcomIndexParamEnableTimeStampReorder,
                           (void *)&reorder, sizeof(reorder));

            if (xerr != OK) {
                ALOGW("Failed to enable timestamp reordering");
            }
        }

        // Enable Sync-frame decode mode for thumbnails
        char board[PROPERTY_VALUE_MAX];
        property_get("ro.board.platform", board, NULL);
        int32_t thumbnailMode = 0;
        if (msg->findInt32("thumbnail-mode", &thumbnailMode) &&
                thumbnailMode > 0 &&
                !(!strcmp(board, "msm8996") || !strcmp(board, "msm8937") ||
                 !strcmp(board, "msm8953") || !strcmp(board, "msm8976"))) {
            ALOGV("Enabling thumbnail mode.");
            QOMX_ENABLETYPE enableType;
            OMX_INDEXTYPE indexType;

            status_t err = OMXhandle->getExtensionIndex(
                    nodeID, OMX_QCOM_INDEX_PARAM_VIDEO_SYNCFRAMEDECODINGMODE,
                    &indexType);
            if (err != OK) {
                ALOGW("Failed to get extension for SYNCFRAMEDECODINGMODE");
            } else {

                enableType.bEnable = OMX_TRUE;
                err = OMXhandle->setParameter(nodeID,indexType,
                           (void *)&enableType, sizeof(enableType));
                if (err != OK) {
                    ALOGW("Failed to get extension for SYNCFRAMEDECODINGMODE");
                } else {
                    ALOGI("Thumbnail mode enabled.");
                }
            }
        }

        // MediaCodec clients can request decoder extradata by setting
        // "enable-extradata-<type>" in MediaFormat.
        // Following <type>s are supported:
        //    "user" => user-extradata
        int extraDataRequested = 0;
        if (msg->findInt32("enable-extradata-user", &extraDataRequested) &&
                extraDataRequested == 1) {
            ALOGI("[%s] User-extradata requested", componentName);
            QOMX_ENABLETYPE enableType;
            enableType.bEnable = OMX_TRUE;

            xerr = OMXhandle->setParameter(
                    nodeID, (OMX_INDEXTYPE)OMX_QcomIndexEnableExtnUserData,
                    &enableType, sizeof(enableType));
            if (xerr != OK) {
                ALOGW("[%s] Failed to enable user-extradata", componentName);
            }
        }
    }
#endif
    return err;
}

#ifdef QCOM_HARDWARE
status_t FFMPEGSoftCodec::setQCDIVXFormat(
        const sp<AMessage> &msg, const char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, int port_index) {
    status_t err = OK;
    ALOGV("Setting the QOMX_VIDEO_PARAM_DIVXTYPE params ");
    QOMX_VIDEO_PARAM_DIVXTYPE paramDivX;
    InitOMXParams(&paramDivX);
    paramDivX.nPortIndex = port_index;
    int32_t DivxVersion = 0;
    if (!msg->findInt32(getMsgKey(kKeyDivXVersion), &DivxVersion)) {
        // Cannot find the key, the caller is skipping the container
        // and use codec directly, let determine divx version from
        // mime type
        DivxVersion = kTypeDivXVer_4;
        const char *v;
        if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime) ||
                !strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX4, mime)) {
            DivxVersion = kTypeDivXVer_4;
            v = "4";
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX311, mime)) {
            DivxVersion = kTypeDivXVer_3_11;
            v = "3.11";
        }
        ALOGW("Divx version key missing, initializing the version to %s", v);
    }
    ALOGV("Divx Version Type %d", DivxVersion);

    if (DivxVersion == kTypeDivXVer_4) {
        paramDivX.eFormat = QOMX_VIDEO_DIVXFormat4;
    } else if (DivxVersion == kTypeDivXVer_5) {
        paramDivX.eFormat = QOMX_VIDEO_DIVXFormat5;
    } else if (DivxVersion == kTypeDivXVer_6) {
        paramDivX.eFormat = QOMX_VIDEO_DIVXFormat6;
    } else if (DivxVersion == kTypeDivXVer_3_11 ) {
        paramDivX.eFormat = QOMX_VIDEO_DIVXFormat311;
    } else {
        paramDivX.eFormat = QOMX_VIDEO_DIVXFormatUnused;
    }
    paramDivX.eProfile = (QOMX_VIDEO_DIVXPROFILETYPE)0;    //Not used for now.

    err =  OMXhandle->setParameter(nodeID,
            (OMX_INDEXTYPE)OMX_QcomIndexParamVideoDivx,
            &paramDivX, sizeof(paramDivX));
    return err;
}
#endif

status_t FFMPEGSoftCodec::getVideoPortFormat(OMX_U32 portIndex, int coding,
        sp<AMessage> &notify, sp<IOMX> OMXHandle, IOMX::node_id nodeId) {

    status_t err = BAD_TYPE;
    switch (coding) {
        case OMX_VIDEO_CodingWMV:
        {
            OMX_VIDEO_PARAM_WMVTYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, OMX_IndexParamVideoWmv, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            int32_t version;
            if (params.eFormat == OMX_VIDEO_WMVFormat7) {
                version = kTypeWMVVer_7;
            } else if (params.eFormat == OMX_VIDEO_WMVFormat8) {
                version = kTypeWMVVer_8;
            } else {
                version = kTypeWMVVer_9;
            }
            notify->setString("mime", MEDIA_MIMETYPE_VIDEO_WMV);
            notify->setInt32("wmv-version", version);
            break;
        }
        case OMX_VIDEO_CodingAutoDetect:
        {
            OMX_VIDEO_PARAM_FFMPEGTYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, (OMX_INDEXTYPE)OMX_IndexParamVideoFFmpeg, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            notify->setString("mime", MEDIA_MIMETYPE_VIDEO_FFMPEG);
            notify->setInt32("codec-id", params.eCodecId);
            break;
        }
        case OMX_VIDEO_CodingRV:
        {
            OMX_VIDEO_PARAM_RVTYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, (OMX_INDEXTYPE)OMX_IndexParamVideoRv, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            int32_t version;
            if (params.eFormat == OMX_VIDEO_RVFormatG2) {
                version = kTypeRVVer_G2;
            } else if (params.eFormat == OMX_VIDEO_RVFormat8) {
                version = kTypeRVVer_8;
            } else {
                version = kTypeRVVer_9;
            }
            notify->setString("mime", MEDIA_MIMETYPE_VIDEO_RV);
            break;
        }
    }
    return err;
}

status_t FFMPEGSoftCodec::getAudioPortFormat(OMX_U32 portIndex, int coding,
        sp<AMessage> &notify, sp<IOMX> OMXHandle, IOMX::node_id nodeId) {

    status_t err = BAD_TYPE;
    switch (coding) {
        case OMX_AUDIO_CodingRA:
        {
            OMX_AUDIO_PARAM_RATYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, OMX_IndexParamAudioRa, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            notify->setString("mime", MEDIA_MIMETYPE_AUDIO_RA);
            notify->setInt32("channel-count", params.nChannels);
            notify->setInt32("sample-rate", params.nSamplingRate);
            break;
        }
        case OMX_AUDIO_CodingMP2:
        {
            OMX_AUDIO_PARAM_MP2TYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, (OMX_INDEXTYPE)OMX_IndexParamAudioMp2, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            notify->setString("mime", MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);
            notify->setInt32("channel-count", params.nChannels);
            notify->setInt32("sample-rate", params.nSampleRate);
            break;
        }
        case OMX_AUDIO_CodingWMA:
        {
            OMX_AUDIO_PARAM_WMATYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, OMX_IndexParamAudioWma, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            notify->setString("mime", MEDIA_MIMETYPE_AUDIO_WMA);
            notify->setInt32("channel-count", params.nChannels);
            notify->setInt32("sample-rate", params.nSamplingRate);
            break;
        }
        case OMX_AUDIO_CodingAPE:
        {
            OMX_AUDIO_PARAM_APETYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, (OMX_INDEXTYPE)OMX_IndexParamAudioApe, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            notify->setString("mime", MEDIA_MIMETYPE_AUDIO_APE);
            notify->setInt32("channel-count", params.nChannels);
            notify->setInt32("sample-rate", params.nSamplingRate);
            notify->setInt32("pcm-encoding",
                    bitsToAudioEncoding(params.nBitsPerSample));
            break;
        }
        case OMX_AUDIO_CodingFLAC:
        {
            OMX_AUDIO_PARAM_FLACTYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, (OMX_INDEXTYPE)OMX_IndexParamAudioFlac, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            notify->setString("mime", MEDIA_MIMETYPE_AUDIO_FLAC);
            notify->setInt32("channel-count", params.nChannels);
            notify->setInt32("sample-rate", params.nSampleRate);
            notify->setInt32("pcm-encoding",
                    bitsToAudioEncoding(params.nCompressionLevel)); // piggyback
            break;
        }

        case OMX_AUDIO_CodingDTS:
        {
            OMX_AUDIO_PARAM_DTSTYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, (OMX_INDEXTYPE)OMX_IndexParamAudioDts, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            notify->setString("mime", MEDIA_MIMETYPE_AUDIO_DTS);
            notify->setInt32("channel-count", params.nChannels);
            notify->setInt32("sample-rate", params.nSamplingRate);
            break;
        }
        case OMX_AUDIO_CodingAC3:
        {
            OMX_AUDIO_PARAM_ANDROID_AC3TYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            notify->setString("mime", MEDIA_MIMETYPE_AUDIO_AC3);
            notify->setInt32("channel-count", params.nChannels);
            notify->setInt32("sample-rate", params.nSampleRate);
            break;
        }

        case OMX_AUDIO_CodingAutoDetect:
        {
            OMX_AUDIO_PARAM_FFMPEGTYPE params;
            InitOMXParams(&params);
            params.nPortIndex = portIndex;

            err = OMXHandle->getParameter(
                    nodeId, (OMX_INDEXTYPE)OMX_IndexParamAudioFFmpeg, &params, sizeof(params));
            if (err != OK) {
                return err;
            }

            notify->setString("mime", MEDIA_MIMETYPE_AUDIO_FFMPEG);
            notify->setInt32("channel-count", params.nChannels);
            notify->setInt32("sample-rate", params.nSampleRate);
            break;
        }
    }
    return err;
}

status_t FFMPEGSoftCodec::setAudioFormat(
        const sp<AMessage> &msg, const char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID) {
    ALOGV("setAudioFormat called");
    status_t err = OK;

    ALOGV("setAudioFormat: %s", msg->debugString(0).c_str());

    if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_WMA, mime))  {
        err = setWMAFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setWMAFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_VORBIS, mime))  {
        err = setVORBISFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setVORBISFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_RA, mime))  {
        err = setRAFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setRAFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_FLAC, mime))  {
        err = setFLACFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setFLACFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II, mime))  {
        err = setMP2Format(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setMP2Format() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AC3, mime)) {
        err = setAC3Format(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setAC3Format() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_APE, mime))  {
        err = setAPEFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setAPEFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_DTS, mime))  {
        err = setDTSFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setDTSFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_FFMPEG, mime))  {
        err = setFFmpegAudioFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setFFmpegAudioFormat() failed (err = %d)", err);
        }
    }

    return err;
}

const char* FFMPEGSoftCodec::getComponentRole(
        bool isEncoder, const char *mime) {

    ALOGV("setSupportedRole Called %s", mime);

    struct MimeToRole {
        const char *mime;
        const char *decoderRole;
        const char *encoderRole;
    };

    static const MimeToRole kFFMPEGMimeToRole[] = {
        { MEDIA_MIMETYPE_AUDIO_AAC,
          "audio_decoder.aac", NULL },
        { MEDIA_MIMETYPE_AUDIO_MPEG,
          "audio_decoder.mp3", NULL },
        { MEDIA_MIMETYPE_AUDIO_VORBIS,
          "audio_decoder.vorbis", NULL },
        { MEDIA_MIMETYPE_AUDIO_WMA,
          "audio_decoder.wma", NULL },
        { MEDIA_MIMETYPE_AUDIO_RA,
          "audio_decoder.ra" , NULL },
        { MEDIA_MIMETYPE_AUDIO_FLAC,
          "audio_decoder.flac", NULL },
        { MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II,
          "audio_decoder.mp2", NULL },
        { MEDIA_MIMETYPE_AUDIO_AC3,
          "audio_decoder.ac3", NULL },
        { MEDIA_MIMETYPE_AUDIO_APE,
          "audio_decoder.ape", NULL },
        { MEDIA_MIMETYPE_AUDIO_DTS,
          "audio_decoder.dts", NULL },
        { MEDIA_MIMETYPE_VIDEO_MPEG2,
          "video_decoder.mpeg2", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX4,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX311,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_WMV,
          "video_decoder.vc1",  NULL }, // so we can still talk to hardware codec
        { MEDIA_MIMETYPE_VIDEO_VC1,
          "video_decoder.vc1", NULL },
        { MEDIA_MIMETYPE_VIDEO_RV,
          "video_decoder.rv", NULL },
        { MEDIA_MIMETYPE_VIDEO_FLV1,
          "video_decoder.flv1", NULL },
        { MEDIA_MIMETYPE_VIDEO_HEVC,
          "video_decoder.hevc", NULL },
        { MEDIA_MIMETYPE_AUDIO_FFMPEG,
          "audio_decoder.trial", NULL },
        { MEDIA_MIMETYPE_VIDEO_FFMPEG,
          "video_decoder.trial", NULL },
        };
    static const size_t kNumMimeToRole =
                     sizeof(kFFMPEGMimeToRole) / sizeof(kFFMPEGMimeToRole[0]);

    size_t i;
    for (i = 0; i < kNumMimeToRole; ++i) {
        if (!strcasecmp(mime, kFFMPEGMimeToRole[i].mime)) {
            break;
        }
    }

    if (i == kNumMimeToRole) {
        return NULL;
    }

    return isEncoder ? kFFMPEGMimeToRole[i].encoderRole
                     : kFFMPEGMimeToRole[i].decoderRole;
}

//video
status_t FFMPEGSoftCodec::setWMVFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t version = -1;
    OMX_VIDEO_PARAM_WMVTYPE paramWMV;

    if (!msg->findInt32(getMsgKey(kKeyWMVVersion), &version)) {
        ALOGE("WMV version not detected");
    }

    InitOMXParams(&paramWMV);
    paramWMV.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamVideoWmv, &paramWMV, sizeof(paramWMV));
    if (err != OK) {
        return err;
    }

    if (version == kTypeWMVVer_7) {
        paramWMV.eFormat = OMX_VIDEO_WMVFormat7;
    } else if (version == kTypeWMVVer_8) {
        paramWMV.eFormat = OMX_VIDEO_WMVFormat8;
    } else if (version == kTypeWMVVer_9) {
        paramWMV.eFormat = OMX_VIDEO_WMVFormat9;
    }

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamVideoWmv, &paramWMV, sizeof(paramWMV));
    return err;
}

status_t FFMPEGSoftCodec::setRVFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t version = kTypeRVVer_G2;
    OMX_VIDEO_PARAM_RVTYPE paramRV;

    if (!msg->findInt32(getMsgKey(kKeyRVVersion), &version)) {
        ALOGE("RV version not detected");
    }

    InitOMXParams(&paramRV);
    paramRV.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamVideoRv, &paramRV, sizeof(paramRV));
    if (err != OK)
        return err;

    if (version == kTypeRVVer_G2) {
        paramRV.eFormat = OMX_VIDEO_RVFormatG2;
    } else if (version == kTypeRVVer_8) {
        paramRV.eFormat = OMX_VIDEO_RVFormat8;
    } else if (version == kTypeRVVer_9) {
        paramRV.eFormat = OMX_VIDEO_RVFormat9;
    }

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamVideoRv, &paramRV, sizeof(paramRV));
    return err;
}

status_t FFMPEGSoftCodec::setFFmpegVideoFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t codec_id = 0;
    int32_t width = 0;
    int32_t height = 0;
    OMX_VIDEO_PARAM_FFMPEGTYPE param;

    ALOGD("setFFmpegVideoFormat");

    if (msg->findInt32(getMsgKey(kKeyWidth), &width)) {
        ALOGE("No video width specified");
    }
    if (msg->findInt32(getMsgKey(kKeyHeight), &height)) {
        ALOGE("No video height specified");
    }
    if (!msg->findInt32(getMsgKey(kKeyCodecId), &codec_id)) {
        ALOGE("No codec id sent for FFMPEG catch-all codec!");
    }

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamVideoFFmpeg, &param, sizeof(param));
    if (err != OK)
        return err;

    param.eCodecId = codec_id;
    param.nWidth   = width;
    param.nHeight  = height;

    err = OMXhandle->setParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamVideoFFmpeg, &param, sizeof(param));
    return err;
}

//audio
status_t FFMPEGSoftCodec::setRawAudioFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    AudioEncoding encoding = kAudioEncodingPcm16bit;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));
    if (!msg->findInt32(getMsgKey(kKeyPcmEncoding), (int32_t*)&encoding)) {
        ALOGD("No PCM format specified, using 16 bit");
    }

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    OMX_AUDIO_PARAM_PCMMODETYPE pcmParams;
    InitOMXParams(&pcmParams);
    pcmParams.nPortIndex = kPortIndexOutput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));

    if (err != OK) {
        return err;
    }

    pcmParams.nChannels = numChannels;
    switch (encoding) {
        case kAudioEncodingPcm8bit:
            pcmParams.eNumData = OMX_NumericalDataUnsigned;
            pcmParams.nBitPerSample = 8;
            break;
        case kAudioEncodingPcmFloat:
            pcmParams.eNumData = OMX_NumericalDataFloat;
            pcmParams.nBitPerSample = 32;
            break;
        case kAudioEncodingPcm16bit:
            pcmParams.eNumData = OMX_NumericalDataSigned;
            pcmParams.nBitPerSample = 16;
            break;
        case kAudioEncodingPcm32bit:
            pcmParams.eNumData = OMX_NumericalDataSigned;
            pcmParams.nBitPerSample = 24;
            break;
        default:
            return BAD_VALUE;
	}
    pcmParams.bInterleaved = OMX_TRUE;
    pcmParams.nSamplingRate = sampleRate;
    pcmParams.ePCMMode = OMX_AUDIO_PCMModeLinear;

    if (ACodec::getOMXChannelMapping(numChannels, pcmParams.eChannelMapping) != OK) {
        return OMX_ErrorNone;
    }

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));
    // if we could not set up raw format to non-16-bit, try with 16-bit
    // NOTE: we will also verify this via readback, in case codec ignores these fields
    if (err != OK && encoding != kAudioEncodingPcm16bit) {
        pcmParams.eNumData = OMX_NumericalDataSigned;
        pcmParams.nBitPerSample = 16;
        err = OMXhandle->setParameter(
                nodeID, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));
    }
    return err;
}

status_t FFMPEGSoftCodec::setWMAFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t version = 0;
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    int32_t bitsPerSample = 0;

    OMX_AUDIO_PARAM_WMATYPE paramWMA;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));
    CHECK(msg->findInt32(getMsgKey(kKeyBitRate), &bitRate));
    if (!msg->findInt32(getMsgKey(kKeyBlockAlign), &blockAlign)) {
        // we should be last on the codec list, but another sniffer may
        // have handled it and there is no hardware codec.
        if (!msg->findInt32(getMsgKey(kKeyWMABlockAlign), &blockAlign)) {
            return ERROR_UNSUPPORTED;
        }
    }

    // mm-parser may want a different bit depth
    if (msg->findInt32(getMsgKey(kKeyWMABitspersample), &bitsPerSample)) {
        msg->setInt32(getMsgKey(kKeyPcmEncoding), (int32_t)bitsToAudioEncoding(bitsPerSample));
    }

    ALOGV("Channels: %d, SampleRate: %d, BitRate: %d, blockAlign: %d",
            numChannels, sampleRate, bitRate, blockAlign);

    CHECK(msg->findInt32(getMsgKey(kKeyWMAVersion), &version));

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&paramWMA);
    paramWMA.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
    if (err != OK)
        return err;

    paramWMA.nChannels = numChannels;
    paramWMA.nSamplingRate = sampleRate;
    paramWMA.nBitRate = bitRate;
    paramWMA.nBlockAlign = blockAlign;

    // http://msdn.microsoft.com/en-us/library/ff819498(v=vs.85).aspx
    if (version == kTypeWMA) {
        paramWMA.eFormat = OMX_AUDIO_WMAFormat7;
    } else if (version == kTypeWMAPro) {
        paramWMA.eFormat = OMX_AUDIO_WMAFormat8;
    } else if (version == kTypeWMALossLess) {
        paramWMA.eFormat = OMX_AUDIO_WMAFormat9;
    }

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
}

status_t FFMPEGSoftCodec::setVORBISFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_VORBISTYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioVorbis, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioVorbis, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setRAFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    OMX_AUDIO_PARAM_RATYPE paramRA;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));
    msg->findInt32(getMsgKey(kKeyBitRate), &bitRate);
    CHECK(msg->findInt32(getMsgKey(kKeyBlockAlign), &blockAlign));

    ALOGV("Channels: %d, SampleRate: %d, BitRate: %d, blockAlign: %d",
            numChannels, sampleRate, bitRate, blockAlign);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&paramRA);
    paramRA.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioRa, &paramRA, sizeof(paramRA));
    if (err != OK)
        return err;

    paramRA.eFormat = OMX_AUDIO_RAFormatUnused; // FIXME, cook only???
    paramRA.nChannels = numChannels;
    paramRA.nSamplingRate = sampleRate;
    // FIXME, HACK!!!, I use the nNumRegions parameter pass blockAlign!!!
    // the cook audio codec need blockAlign!
    paramRA.nNumRegions = blockAlign;

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioRa, &paramRA, sizeof(paramRA));
}

status_t FFMPEGSoftCodec::setFLACFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    AudioEncoding encoding = kAudioEncodingPcm16bit;
    OMX_AUDIO_PARAM_FLACTYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));
    msg->findInt32(getMsgKey(kKeyPcmEncoding), (int32_t*)&encoding);

    ALOGV("Channels: %d, SampleRate: %d Encoding: %d",
            numChannels, sampleRate, encoding);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioFlac, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;
    param.nCompressionLevel = encoding; // piggyback hax!

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioFlac, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setMP2Format(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_MP2TYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioMp2, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    return OMXhandle->setParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioMp2, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setAC3Format(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_ANDROID_AC3TYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    return OMXhandle->setParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setAPEFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    AudioEncoding encoding = kAudioEncodingPcm16bit;
    OMX_AUDIO_PARAM_APETYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));
    CHECK(msg->findInt32(getMsgKey(kKeyPcmEncoding), (int32_t*)&encoding));

    ALOGV("Channels:%d, SampleRate:%d, Encoding:%d",
            numChannels, sampleRate, encoding);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioApe, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSamplingRate = sampleRate;
    param.nBitsPerSample = audioEncodingToBits(encoding);

    return OMXhandle->setParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioApe, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setDTSFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_DTSTYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioDts, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSamplingRate = sampleRate;

    return OMXhandle->setParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioDts, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setFFmpegAudioFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t codec_id = 0;
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    int32_t sampleFormat = 0;
    int32_t codedSampleBits = 0;
	AudioEncoding encoding = kAudioEncodingPcm16bit;
    OMX_AUDIO_PARAM_FFMPEGTYPE param;

    ALOGD("setFFmpegAudioFormat");

    CHECK(msg->findInt32(getMsgKey(kKeyCodecId), &codec_id));
    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleFormat), &sampleFormat));
    msg->findInt32(getMsgKey(kKeyBitRate), &bitRate);
    msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate);
    msg->findInt32(getMsgKey(kKeyBlockAlign), &blockAlign);
    msg->findInt32(getMsgKey(kKeyCodedSampleBits), &codedSampleBits);
    msg->findInt32(getMsgKey(kKeyPcmEncoding), (int32_t*)&encoding);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioFFmpeg, &param, sizeof(param));
    if (err != OK)
        return err;

    param.eCodecId       = codec_id;
    param.nChannels      = numChannels;
    param.nBitRate       = bitRate;
    param.nBitsPerSample = codedSampleBits;
    param.nSampleRate    = sampleRate;
    param.nBlockAlign    = blockAlign;
    param.eSampleFormat  = sampleFormat;

    return OMXhandle->setParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioFFmpeg, &param, sizeof(param));
}

void* FFMPEGSoftCodec::sLibHandle = NULL;

void FFMPEGSoftCodec::loadPlugin() {
    char lib[PROPERTY_VALUE_MAX];
    if (!sLibHandle && property_get("media.sf.extractor-plugin", lib, NULL)) {
        if ((sLibHandle = ::dlopen(lib, RTLD_LAZY)) != NULL) {
            sSnifferFunc = (DataSource::SnifferFunc)dlsym(sLibHandle, "SniffFFMPEG");
            sExtractorFunc = (CreateExtractorFunc)dlsym(sLibHandle, "CreateFFMPEGExtractor");
        }
        if (dlerror()) {
            sSnifferFunc = NULL;
            sExtractorFunc = NULL;
            dlclose(sLibHandle);
            ALOGE("Failed to load FFMPEG plugin: %s", dlerror());
        }
    }
}

DataSource::SnifferFunc FFMPEGSoftCodec::sSnifferFunc = NULL;

DataSource::SnifferFunc FFMPEGSoftCodec::getSniffer() {
    loadPlugin();
    return sSnifferFunc;
}

FFMPEGSoftCodec::CreateExtractorFunc FFMPEGSoftCodec::sExtractorFunc = NULL;

MediaExtractor* FFMPEGSoftCodec::createExtractor(const sp<DataSource> &source,
        const char *mime, const sp<AMessage> &meta) {
    loadPlugin();
    if (sLibHandle == NULL) {
        return NULL;
    }
    return sExtractorFunc(source, mime, meta);
}

}
