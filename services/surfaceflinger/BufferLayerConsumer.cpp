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

#undef LOG_TAG
#define LOG_TAG "BufferLayerConsumer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#include "BufferLayerConsumer.h"

#include <inttypes.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cutils/compiler.h>

#include <hardware/hardware.h>

#include <math/mat4.h>

#include <gui/BufferItem.h>
#include <gui/GLConsumer.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>

#include <private/gui/ComposerService.h>
#include <private/gui/SyncFeatures.h>

#include <utils/Log.h>
#include <utils/String8.h>
#include <utils/Trace.h>

extern "C" EGLAPI const char* eglQueryStringImplementationANDROID(EGLDisplay dpy, EGLint name);
#define CROP_EXT_STR "EGL_ANDROID_image_crop"
#define PROT_CONTENT_EXT_STR "EGL_EXT_protected_content"
#define EGL_PROTECTED_CONTENT_EXT 0x32C0

namespace android {

// Macros for including the BufferLayerConsumer name in log messages
#define BLC_LOGV(x, ...) ALOGV("[%s] " x, mName.string(), ##__VA_ARGS__)
#define BLC_LOGD(x, ...) ALOGD("[%s] " x, mName.string(), ##__VA_ARGS__)
//#define BLC_LOGI(x, ...) ALOGI("[%s] " x, mName.string(), ##__VA_ARGS__)
#define BLC_LOGW(x, ...) ALOGW("[%s] " x, mName.string(), ##__VA_ARGS__)
#define BLC_LOGE(x, ...) ALOGE("[%s] " x, mName.string(), ##__VA_ARGS__)

static const mat4 mtxIdentity;

static bool hasEglAndroidImageCropImpl() {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    const char* exts = eglQueryStringImplementationANDROID(dpy, EGL_EXTENSIONS);
    size_t cropExtLen = strlen(CROP_EXT_STR);
    size_t extsLen = strlen(exts);
    bool equal = !strcmp(CROP_EXT_STR, exts);
    bool atStart = !strncmp(CROP_EXT_STR " ", exts, cropExtLen + 1);
    bool atEnd = (cropExtLen + 1) < extsLen &&
            !strcmp(" " CROP_EXT_STR, exts + extsLen - (cropExtLen + 1));
    bool inMiddle = strstr(exts, " " CROP_EXT_STR " ");
    return equal || atStart || atEnd || inMiddle;
}

static bool hasEglAndroidImageCrop() {
    // Only compute whether the extension is present once the first time this
    // function is called.
    static bool hasIt = hasEglAndroidImageCropImpl();
    return hasIt;
}

static bool hasEglProtectedContentImpl() {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    const char* exts = eglQueryString(dpy, EGL_EXTENSIONS);
    size_t cropExtLen = strlen(PROT_CONTENT_EXT_STR);
    size_t extsLen = strlen(exts);
    bool equal = !strcmp(PROT_CONTENT_EXT_STR, exts);
    bool atStart = !strncmp(PROT_CONTENT_EXT_STR " ", exts, cropExtLen + 1);
    bool atEnd = (cropExtLen + 1) < extsLen &&
            !strcmp(" " PROT_CONTENT_EXT_STR, exts + extsLen - (cropExtLen + 1));
    bool inMiddle = strstr(exts, " " PROT_CONTENT_EXT_STR " ");
    return equal || atStart || atEnd || inMiddle;
}

static bool hasEglProtectedContent() {
    // Only compute whether the extension is present once the first time this
    // function is called.
    static bool hasIt = hasEglProtectedContentImpl();
    return hasIt;
}

static bool isEglImageCroppable(const Rect& crop) {
    return hasEglAndroidImageCrop() && (crop.left == 0 && crop.top == 0);
}

BufferLayerConsumer::BufferLayerConsumer(const sp<IGraphicBufferConsumer>& bq, uint32_t tex)
      : ConsumerBase(bq, false),
        mCurrentCrop(Rect::EMPTY_RECT),
        mCurrentTransform(0),
        mCurrentScalingMode(NATIVE_WINDOW_SCALING_MODE_FREEZE),
        mCurrentFence(Fence::NO_FENCE),
        mCurrentTimestamp(0),
        mCurrentDataSpace(HAL_DATASPACE_UNKNOWN),
        mCurrentFrameNumber(0),
        mDefaultWidth(1),
        mDefaultHeight(1),
        mFilteringEnabled(true),
        mTexName(tex),
        mEglDisplay(EGL_NO_DISPLAY),
        mEglContext(EGL_NO_CONTEXT),
        mCurrentTexture(BufferQueue::INVALID_BUFFER_SLOT) {
    BLC_LOGV("BufferLayerConsumer");

    memcpy(mCurrentTransformMatrix, mtxIdentity.asArray(), sizeof(mCurrentTransformMatrix));

    mConsumer->setConsumerUsageBits(DEFAULT_USAGE_FLAGS);
}

status_t BufferLayerConsumer::setDefaultBufferSize(uint32_t w, uint32_t h) {
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        BLC_LOGE("setDefaultBufferSize: BufferLayerConsumer is abandoned!");
        return NO_INIT;
    }
    mDefaultWidth = w;
    mDefaultHeight = h;
    return mConsumer->setDefaultBufferSize(w, h);
}

status_t BufferLayerConsumer::updateTexImage() {
    ATRACE_CALL();
    BLC_LOGV("updateTexImage");
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        BLC_LOGE("updateTexImage: BufferLayerConsumer is abandoned!");
        return NO_INIT;
    }

    // Make sure the EGL state is the same as in previous calls.
    status_t err = checkAndUpdateEglStateLocked();
    if (err != NO_ERROR) {
        return err;
    }

    BufferItem item;

    // Acquire the next buffer.
    // In asynchronous mode the list is guaranteed to be one buffer
    // deep, while in synchronous mode we use the oldest buffer.
    err = acquireBufferLocked(&item, 0);
    if (err != NO_ERROR) {
        if (err == BufferQueue::NO_BUFFER_AVAILABLE) {
            // We always bind the texture even if we don't update its contents.
            BLC_LOGV("updateTexImage: no buffers were available");
            glBindTexture(sTexTarget, mTexName);
            err = NO_ERROR;
        } else {
            BLC_LOGE("updateTexImage: acquire failed: %s (%d)", strerror(-err), err);
        }
        return err;
    }

    // Release the previous buffer.
    err = updateAndReleaseLocked(item);
    if (err != NO_ERROR) {
        // We always bind the texture.
        glBindTexture(sTexTarget, mTexName);
        return err;
    }

    // Bind the new buffer to the GL texture, and wait until it's ready.
    return bindTextureImageLocked();
}

status_t BufferLayerConsumer::acquireBufferLocked(BufferItem* item, nsecs_t presentWhen,
                                                  uint64_t maxFrameNumber) {
    status_t err = ConsumerBase::acquireBufferLocked(item, presentWhen, maxFrameNumber);
    if (err != NO_ERROR) {
        return err;
    }

    // If item->mGraphicBuffer is not null, this buffer has not been acquired
    // before, so any prior EglImage created is using a stale buffer. This
    // replaces any old EglImage with a new one (using the new buffer).
    if (item->mGraphicBuffer != NULL) {
        int slot = item->mSlot;
        mEglSlots[slot].mEglImage = new EglImage(item->mGraphicBuffer);
    }

    return NO_ERROR;
}

status_t BufferLayerConsumer::updateAndReleaseLocked(const BufferItem& item,
                                                     PendingRelease* pendingRelease) {
    status_t err = NO_ERROR;

    int slot = item.mSlot;

    // Confirm state.
    err = checkAndUpdateEglStateLocked();
    if (err != NO_ERROR) {
        releaseBufferLocked(slot, mSlots[slot].mGraphicBuffer);
        return err;
    }

    // Ensure we have a valid EglImageKHR for the slot, creating an EglImage
    // if nessessary, for the gralloc buffer currently in the slot in
    // ConsumerBase.
    // We may have to do this even when item.mGraphicBuffer == NULL (which
    // means the buffer was previously acquired).
    err = mEglSlots[slot].mEglImage->createIfNeeded(mEglDisplay, item.mCrop);
    if (err != NO_ERROR) {
        BLC_LOGW("updateAndRelease: unable to createImage on display=%p slot=%d", mEglDisplay,
                 slot);
        releaseBufferLocked(slot, mSlots[slot].mGraphicBuffer);
        return UNKNOWN_ERROR;
    }

    // Do whatever sync ops we need to do before releasing the old slot.
    if (slot != mCurrentTexture) {
        err = syncForReleaseLocked(mEglDisplay);
        if (err != NO_ERROR) {
            // Release the buffer we just acquired.  It's not safe to
            // release the old buffer, so instead we just drop the new frame.
            // As we are still under lock since acquireBuffer, it is safe to
            // release by slot.
            releaseBufferLocked(slot, mSlots[slot].mGraphicBuffer);
            return err;
        }
    }

    BLC_LOGV("updateAndRelease: (slot=%d buf=%p) -> (slot=%d buf=%p)", mCurrentTexture,
             mCurrentTextureImage != NULL ? mCurrentTextureImage->graphicBufferHandle() : 0, slot,
             mSlots[slot].mGraphicBuffer->handle);

    // Hang onto the pointer so that it isn't freed in the call to
    // releaseBufferLocked() if we're in shared buffer mode and both buffers are
    // the same.
    sp<EglImage> nextTextureImage = mEglSlots[slot].mEglImage;

    // release old buffer
    if (mCurrentTexture != BufferQueue::INVALID_BUFFER_SLOT) {
        if (pendingRelease == nullptr) {
            status_t status =
                    releaseBufferLocked(mCurrentTexture, mCurrentTextureImage->graphicBuffer());
            if (status < NO_ERROR) {
                BLC_LOGE("updateAndRelease: failed to release buffer: %s (%d)", strerror(-status),
                         status);
                err = status;
                // keep going, with error raised [?]
            }
        } else {
            pendingRelease->currentTexture = mCurrentTexture;
            pendingRelease->graphicBuffer = mCurrentTextureImage->graphicBuffer();
            pendingRelease->isPending = true;
        }
    }

    // Update the BufferLayerConsumer state.
    mCurrentTexture = slot;
    mCurrentTextureImage = nextTextureImage;
    mCurrentCrop = item.mCrop;
    mCurrentTransform = item.mTransform;
    mCurrentScalingMode = item.mScalingMode;
    mCurrentTimestamp = item.mTimestamp;
    mCurrentDataSpace = item.mDataSpace;
    mCurrentFence = item.mFence;
    mCurrentFenceTime = item.mFenceTime;
    mCurrentFrameNumber = item.mFrameNumber;

    computeCurrentTransformMatrixLocked();

    return err;
}

status_t BufferLayerConsumer::bindTextureImageLocked() {
    if (mEglDisplay == EGL_NO_DISPLAY) {
        ALOGE("bindTextureImage: invalid display");
        return INVALID_OPERATION;
    }

    GLenum error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        BLC_LOGW("bindTextureImage: clearing GL error: %#04x", error);
    }

    glBindTexture(sTexTarget, mTexName);
    if (mCurrentTexture == BufferQueue::INVALID_BUFFER_SLOT && mCurrentTextureImage == NULL) {
        BLC_LOGE("bindTextureImage: no currently-bound texture");
        return NO_INIT;
    }

    status_t err = mCurrentTextureImage->createIfNeeded(mEglDisplay, mCurrentCrop);
    if (err != NO_ERROR) {
        BLC_LOGW("bindTextureImage: can't create image on display=%p slot=%d", mEglDisplay,
                 mCurrentTexture);
        return UNKNOWN_ERROR;
    }
    mCurrentTextureImage->bindToTextureTarget(sTexTarget);

    // Wait for the new buffer to be ready.
    return doGLFenceWaitLocked();
}

status_t BufferLayerConsumer::checkAndUpdateEglStateLocked() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();

    // if this is the first time we're called, mEglDisplay/mEglContext have
    // never been set, so don't error out (below).
    if (mEglDisplay == EGL_NO_DISPLAY) {
        mEglDisplay = dpy;
    }
    if (mEglContext == EGL_NO_CONTEXT) {
        mEglContext = ctx;
    }

    if (mEglDisplay != dpy || dpy == EGL_NO_DISPLAY) {
        BLC_LOGE("checkAndUpdateEglState: invalid current EGLDisplay");
        return INVALID_OPERATION;
    }

    if (mEglContext != ctx || ctx == EGL_NO_CONTEXT) {
        BLC_LOGE("checkAndUpdateEglState: invalid current EGLContext");
        return INVALID_OPERATION;
    }

    return NO_ERROR;
}

void BufferLayerConsumer::setReleaseFence(const sp<Fence>& fence) {
    if (fence->isValid() && mCurrentTexture != BufferQueue::INVALID_BUFFER_SLOT) {
        status_t err =
                addReleaseFence(mCurrentTexture, mCurrentTextureImage->graphicBuffer(), fence);
        if (err != OK) {
            BLC_LOGE("setReleaseFence: failed to add the fence: %s (%d)", strerror(-err), err);
        }
    }
}

status_t BufferLayerConsumer::syncForReleaseLocked(EGLDisplay dpy) {
    BLC_LOGV("syncForReleaseLocked");

    if (mCurrentTexture != BufferQueue::INVALID_BUFFER_SLOT) {
        if (SyncFeatures::getInstance().useNativeFenceSync()) {
            EGLSyncKHR sync = eglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
            if (sync == EGL_NO_SYNC_KHR) {
                BLC_LOGE("syncForReleaseLocked: error creating EGL fence: %#x", eglGetError());
                return UNKNOWN_ERROR;
            }
            glFlush();
            int fenceFd = eglDupNativeFenceFDANDROID(dpy, sync);
            eglDestroySyncKHR(dpy, sync);
            if (fenceFd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
                BLC_LOGE("syncForReleaseLocked: error dup'ing native fence "
                         "fd: %#x",
                         eglGetError());
                return UNKNOWN_ERROR;
            }
            sp<Fence> fence(new Fence(fenceFd));
            status_t err = addReleaseFenceLocked(mCurrentTexture,
                                                 mCurrentTextureImage->graphicBuffer(), fence);
            if (err != OK) {
                BLC_LOGE("syncForReleaseLocked: error adding release fence: "
                         "%s (%d)",
                         strerror(-err), err);
                return err;
            }
        }
    }

    return OK;
}

void BufferLayerConsumer::getTransformMatrix(float mtx[16]) {
    Mutex::Autolock lock(mMutex);
    memcpy(mtx, mCurrentTransformMatrix, sizeof(mCurrentTransformMatrix));
}

void BufferLayerConsumer::setFilteringEnabled(bool enabled) {
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        BLC_LOGE("setFilteringEnabled: BufferLayerConsumer is abandoned!");
        return;
    }
    bool needsRecompute = mFilteringEnabled != enabled;
    mFilteringEnabled = enabled;

    if (needsRecompute && mCurrentTextureImage == NULL) {
        BLC_LOGD("setFilteringEnabled called with mCurrentTextureImage == NULL");
    }

    if (needsRecompute && mCurrentTextureImage != NULL) {
        computeCurrentTransformMatrixLocked();
    }
}

void BufferLayerConsumer::computeCurrentTransformMatrixLocked() {
    BLC_LOGV("computeCurrentTransformMatrixLocked");
    sp<GraphicBuffer> buf =
            (mCurrentTextureImage == nullptr) ? nullptr : mCurrentTextureImage->graphicBuffer();
    if (buf == nullptr) {
        BLC_LOGD("computeCurrentTransformMatrixLocked: "
                 "mCurrentTextureImage is NULL");
    }
    computeTransformMatrix(mCurrentTransformMatrix, buf,
                           isEglImageCroppable(mCurrentCrop) ? Rect::EMPTY_RECT : mCurrentCrop,
                           mCurrentTransform, mFilteringEnabled);
}

void BufferLayerConsumer::computeTransformMatrix(float outTransform[16],
                                                 const sp<GraphicBuffer>& buf, const Rect& cropRect,
                                                 uint32_t transform, bool filtering) {
    // Transform matrices
    static const mat4 mtxFlipH(-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1);
    static const mat4 mtxFlipV(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1);
    static const mat4 mtxRot90(0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1);

    mat4 xform;
    if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_H) {
        xform *= mtxFlipH;
    }
    if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_V) {
        xform *= mtxFlipV;
    }
    if (transform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        xform *= mtxRot90;
    }

    if (!cropRect.isEmpty()) {
        float tx = 0.0f, ty = 0.0f, sx = 1.0f, sy = 1.0f;
        float bufferWidth = buf->getWidth();
        float bufferHeight = buf->getHeight();
        float shrinkAmount = 0.0f;
        if (filtering) {
            // In order to prevent bilinear sampling beyond the edge of the
            // crop rectangle we may need to shrink it by 2 texels in each
            // dimension.  Normally this would just need to take 1/2 a texel
            // off each end, but because the chroma channels of YUV420 images
            // are subsampled we may need to shrink the crop region by a whole
            // texel on each side.
            switch (buf->getPixelFormat()) {
                case PIXEL_FORMAT_RGBA_8888:
                case PIXEL_FORMAT_RGBX_8888:
                case PIXEL_FORMAT_RGBA_FP16:
                case PIXEL_FORMAT_RGBA_1010102:
                case PIXEL_FORMAT_RGB_888:
                case PIXEL_FORMAT_RGB_565:
                case PIXEL_FORMAT_BGRA_8888:
                    // We know there's no subsampling of any channels, so we
                    // only need to shrink by a half a pixel.
                    shrinkAmount = 0.5;
                    break;

                default:
                    // If we don't recognize the format, we must assume the
                    // worst case (that we care about), which is YUV420.
                    shrinkAmount = 1.0;
                    break;
            }
        }

        // Only shrink the dimensions that are not the size of the buffer.
        if (cropRect.width() < bufferWidth) {
            tx = (float(cropRect.left) + shrinkAmount) / bufferWidth;
            sx = (float(cropRect.width()) - (2.0f * shrinkAmount)) / bufferWidth;
        }
        if (cropRect.height() < bufferHeight) {
            ty = (float(bufferHeight - cropRect.bottom) + shrinkAmount) / bufferHeight;
            sy = (float(cropRect.height()) - (2.0f * shrinkAmount)) / bufferHeight;
        }

        mat4 crop(sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, 1, 0, tx, ty, 0, 1);
        xform = crop * xform;
    }

    // SurfaceFlinger expects the top of its window textures to be at a Y
    // coordinate of 0, so BufferLayerConsumer must behave the same way.  We don't
    // want to expose this to applications, however, so we must add an
    // additional vertical flip to the transform after all the other transforms.
    xform = mtxFlipV * xform;

    memcpy(outTransform, xform.asArray(), sizeof(xform));
}

Rect BufferLayerConsumer::scaleDownCrop(const Rect& crop, uint32_t bufferWidth,
                                        uint32_t bufferHeight) {
    Rect outCrop = crop;

    uint32_t newWidth = static_cast<uint32_t>(crop.width());
    uint32_t newHeight = static_cast<uint32_t>(crop.height());

    if (newWidth * bufferHeight > newHeight * bufferWidth) {
        newWidth = newHeight * bufferWidth / bufferHeight;
        ALOGV("too wide: newWidth = %d", newWidth);
    } else if (newWidth * bufferHeight < newHeight * bufferWidth) {
        newHeight = newWidth * bufferHeight / bufferWidth;
        ALOGV("too tall: newHeight = %d", newHeight);
    }

    uint32_t currentWidth = static_cast<uint32_t>(crop.width());
    uint32_t currentHeight = static_cast<uint32_t>(crop.height());

    // The crop is too wide
    if (newWidth < currentWidth) {
        uint32_t dw = currentWidth - newWidth;
        auto halfdw = dw / 2;
        outCrop.left += halfdw;
        // Not halfdw because it would subtract 1 too few when dw is odd
        outCrop.right -= (dw - halfdw);
        // The crop is too tall
    } else if (newHeight < currentHeight) {
        uint32_t dh = currentHeight - newHeight;
        auto halfdh = dh / 2;
        outCrop.top += halfdh;
        // Not halfdh because it would subtract 1 too few when dh is odd
        outCrop.bottom -= (dh - halfdh);
    }

    ALOGV("getCurrentCrop final crop [%d,%d,%d,%d]", outCrop.left, outCrop.top, outCrop.right,
          outCrop.bottom);

    return outCrop;
}

nsecs_t BufferLayerConsumer::getTimestamp() {
    BLC_LOGV("getTimestamp");
    Mutex::Autolock lock(mMutex);
    return mCurrentTimestamp;
}

android_dataspace BufferLayerConsumer::getCurrentDataSpace() {
    BLC_LOGV("getCurrentDataSpace");
    Mutex::Autolock lock(mMutex);
    return mCurrentDataSpace;
}

uint64_t BufferLayerConsumer::getFrameNumber() {
    BLC_LOGV("getFrameNumber");
    Mutex::Autolock lock(mMutex);
    return mCurrentFrameNumber;
}

sp<GraphicBuffer> BufferLayerConsumer::getCurrentBuffer(int* outSlot) const {
    Mutex::Autolock lock(mMutex);

    if (outSlot != nullptr) {
        *outSlot = mCurrentTexture;
    }

    return (mCurrentTextureImage == nullptr) ? NULL : mCurrentTextureImage->graphicBuffer();
}

Rect BufferLayerConsumer::getCurrentCrop() const {
    Mutex::Autolock lock(mMutex);
    return (mCurrentScalingMode == NATIVE_WINDOW_SCALING_MODE_SCALE_CROP)
            ? scaleDownCrop(mCurrentCrop, mDefaultWidth, mDefaultHeight)
            : mCurrentCrop;
}

uint32_t BufferLayerConsumer::getCurrentTransform() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentTransform;
}

uint32_t BufferLayerConsumer::getCurrentScalingMode() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentScalingMode;
}

sp<Fence> BufferLayerConsumer::getCurrentFence() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentFence;
}

std::shared_ptr<FenceTime> BufferLayerConsumer::getCurrentFenceTime() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentFenceTime;
}

status_t BufferLayerConsumer::doGLFenceWaitLocked() const {
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();

    if (mEglDisplay != dpy || mEglDisplay == EGL_NO_DISPLAY) {
        BLC_LOGE("doGLFenceWait: invalid current EGLDisplay");
        return INVALID_OPERATION;
    }

    if (mEglContext != ctx || mEglContext == EGL_NO_CONTEXT) {
        BLC_LOGE("doGLFenceWait: invalid current EGLContext");
        return INVALID_OPERATION;
    }

    if (mCurrentFence->isValid()) {
        if (SyncFeatures::getInstance().useWaitSync()) {
            // Create an EGLSyncKHR from the current fence.
            int fenceFd = mCurrentFence->dup();
            if (fenceFd == -1) {
                BLC_LOGE("doGLFenceWait: error dup'ing fence fd: %d", errno);
                return -errno;
            }
            EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceFd, EGL_NONE};
            EGLSyncKHR sync = eglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
            if (sync == EGL_NO_SYNC_KHR) {
                close(fenceFd);
                BLC_LOGE("doGLFenceWait: error creating EGL fence: %#x", eglGetError());
                return UNKNOWN_ERROR;
            }

            // XXX: The spec draft is inconsistent as to whether this should
            // return an EGLint or void.  Ignore the return value for now, as
            // it's not strictly needed.
            eglWaitSyncKHR(dpy, sync, 0);
            EGLint eglErr = eglGetError();
            eglDestroySyncKHR(dpy, sync);
            if (eglErr != EGL_SUCCESS) {
                BLC_LOGE("doGLFenceWait: error waiting for EGL fence: %#x", eglErr);
                return UNKNOWN_ERROR;
            }
        } else {
            status_t err = mCurrentFence->waitForever("BufferLayerConsumer::doGLFenceWaitLocked");
            if (err != NO_ERROR) {
                BLC_LOGE("doGLFenceWait: error waiting for fence: %d", err);
                return err;
            }
        }
    }

    return NO_ERROR;
}

void BufferLayerConsumer::freeBufferLocked(int slotIndex) {
    BLC_LOGV("freeBufferLocked: slotIndex=%d", slotIndex);
    if (slotIndex == mCurrentTexture) {
        mCurrentTexture = BufferQueue::INVALID_BUFFER_SLOT;
    }
    mEglSlots[slotIndex].mEglImage.clear();
    ConsumerBase::freeBufferLocked(slotIndex);
}

void BufferLayerConsumer::abandonLocked() {
    BLC_LOGV("abandonLocked");
    mCurrentTextureImage.clear();
    ConsumerBase::abandonLocked();
}

status_t BufferLayerConsumer::setConsumerUsageBits(uint64_t usage) {
    return ConsumerBase::setConsumerUsageBits(usage | DEFAULT_USAGE_FLAGS);
}

void BufferLayerConsumer::dumpLocked(String8& result, const char* prefix) const {
    result.appendFormat("%smTexName=%d mCurrentTexture=%d\n"
                        "%smCurrentCrop=[%d,%d,%d,%d] mCurrentTransform=%#x\n",
                        prefix, mTexName, mCurrentTexture, prefix, mCurrentCrop.left,
                        mCurrentCrop.top, mCurrentCrop.right, mCurrentCrop.bottom,
                        mCurrentTransform);

    ConsumerBase::dumpLocked(result, prefix);
}

BufferLayerConsumer::EglImage::EglImage(sp<GraphicBuffer> graphicBuffer)
      : mGraphicBuffer(graphicBuffer),
        mEglImage(EGL_NO_IMAGE_KHR),
        mEglDisplay(EGL_NO_DISPLAY),
        mCropRect(Rect::EMPTY_RECT) {}

BufferLayerConsumer::EglImage::~EglImage() {
    if (mEglImage != EGL_NO_IMAGE_KHR) {
        if (!eglDestroyImageKHR(mEglDisplay, mEglImage)) {
            ALOGE("~EglImage: eglDestroyImageKHR failed");
        }
        eglTerminate(mEglDisplay);
    }
}

status_t BufferLayerConsumer::EglImage::createIfNeeded(EGLDisplay eglDisplay,
                                                       const Rect& cropRect) {
    // If there's an image and it's no longer valid, destroy it.
    bool haveImage = mEglImage != EGL_NO_IMAGE_KHR;
    bool displayInvalid = mEglDisplay != eglDisplay;
    bool cropInvalid = hasEglAndroidImageCrop() && mCropRect != cropRect;
    if (haveImage && (displayInvalid || cropInvalid)) {
        if (!eglDestroyImageKHR(mEglDisplay, mEglImage)) {
            ALOGE("createIfNeeded: eglDestroyImageKHR failed");
        }
        eglTerminate(mEglDisplay);
        mEglImage = EGL_NO_IMAGE_KHR;
        mEglDisplay = EGL_NO_DISPLAY;
    }

    // If there's no image, create one.
    if (mEglImage == EGL_NO_IMAGE_KHR) {
        mEglDisplay = eglDisplay;
        mCropRect = cropRect;
        mEglImage = createImage(mEglDisplay, mGraphicBuffer, mCropRect);
    }

    // Fail if we can't create a valid image.
    if (mEglImage == EGL_NO_IMAGE_KHR) {
        mEglDisplay = EGL_NO_DISPLAY;
        mCropRect.makeInvalid();
        const sp<GraphicBuffer>& buffer = mGraphicBuffer;
        ALOGE("Failed to create image. size=%ux%u st=%u usage=%#" PRIx64 " fmt=%d",
              buffer->getWidth(), buffer->getHeight(), buffer->getStride(), buffer->getUsage(),
              buffer->getPixelFormat());
        return UNKNOWN_ERROR;
    }

    return OK;
}

void BufferLayerConsumer::EglImage::bindToTextureTarget(uint32_t texTarget) {
    glEGLImageTargetTexture2DOES(texTarget, static_cast<GLeglImageOES>(mEglImage));
}

EGLImageKHR BufferLayerConsumer::EglImage::createImage(EGLDisplay dpy,
                                                       const sp<GraphicBuffer>& graphicBuffer,
                                                       const Rect& crop) {
    EGLClientBuffer cbuf = static_cast<EGLClientBuffer>(graphicBuffer->getNativeBuffer());
    const bool createProtectedImage =
            (graphicBuffer->getUsage() & GRALLOC_USAGE_PROTECTED) && hasEglProtectedContent();
    EGLint attrs[] = {
            EGL_IMAGE_PRESERVED_KHR,
            EGL_TRUE,
            EGL_IMAGE_CROP_LEFT_ANDROID,
            crop.left,
            EGL_IMAGE_CROP_TOP_ANDROID,
            crop.top,
            EGL_IMAGE_CROP_RIGHT_ANDROID,
            crop.right,
            EGL_IMAGE_CROP_BOTTOM_ANDROID,
            crop.bottom,
            createProtectedImage ? EGL_PROTECTED_CONTENT_EXT : EGL_NONE,
            createProtectedImage ? EGL_TRUE : EGL_NONE,
            EGL_NONE,
    };
    if (!crop.isValid()) {
        // No crop rect to set, so leave the crop out of the attrib array. Make
        // sure to propagate the protected content attrs if they are set.
        attrs[2] = attrs[10];
        attrs[3] = attrs[11];
        attrs[4] = EGL_NONE;
    } else if (!isEglImageCroppable(crop)) {
        // The crop rect is not at the origin, so we can't set the crop on the
        // EGLImage because that's not allowed by the EGL_ANDROID_image_crop
        // extension.  In the future we can add a layered extension that
        // removes this restriction if there is hardware that can support it.
        attrs[2] = attrs[10];
        attrs[3] = attrs[11];
        attrs[4] = EGL_NONE;
    }
    eglInitialize(dpy, 0, 0);
    EGLImageKHR image =
            eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cbuf, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError();
        ALOGE("error creating EGLImage: %#x", error);
        eglTerminate(dpy);
    }
    return image;
}

}; // namespace android
