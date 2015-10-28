/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkBitmap.h"
#include "SkBitmapCache.h"
#include "SkImage_Base.h"
#include "SkImageCacherator.h"
#include "SkMallocPixelRef.h"
#include "SkNextID.h"
#include "SkPixelRef.h"
#include "SkResourceCache.h"

#if SK_SUPPORT_GPU
#include "GrContext.h"
#include "GrGpuResourcePriv.h"
#include "GrResourceKey.h"
#include "GrTextureParams.h"
#include "GrYUVProvider.h"
#include "SkGr.h"
#include "SkGrPriv.h"
#endif

SkImageCacherator* SkImageCacherator::NewFromGenerator(SkImageGenerator* gen,
                                                       const SkIRect* subset) {
    if (!gen) {
        return nullptr;
    }

    // We are required to take ownership of gen, regardless of if we return a cacherator or not
    SkAutoTDelete<SkImageGenerator> genHolder(gen);

    const SkImageInfo& info = gen->getInfo();
    if (info.isEmpty()) {
        return nullptr;
    }

    uint32_t uniqueID = gen->uniqueID();
    const SkIRect bounds = SkIRect::MakeWH(info.width(), info.height());
    if (subset) {
        if (!bounds.contains(*subset)) {
            return nullptr;
        }
        if (*subset != bounds) {
            // we need a different uniqueID since we really are a subset of the raw generator
            uniqueID = SkNextID::ImageID();
        }
    } else {
        subset = &bounds;
    }

    // Now that we know we can hand-off the generator (to be owned by the cacherator) we can
    // release our holder. (we DONT want to delete it here anymore)
    genHolder.detach();

    return new SkImageCacherator(gen, gen->getInfo().makeWH(subset->width(), subset->height()),
                                 SkIPoint::Make(subset->x(), subset->y()), uniqueID);
}

SkImageCacherator::SkImageCacherator(SkImageGenerator* gen, const SkImageInfo& info,
                                     const SkIPoint& origin, uint32_t uniqueID)
    : fNotThreadSafeGenerator(gen)
    , fInfo(info)
    , fOrigin(origin)
    , fUniqueID(uniqueID)
{}

SkData* SkImageCacherator::refEncoded() {
    ScopedGenerator generator(this);
    return generator->refEncodedData();
}

static bool check_output_bitmap(const SkBitmap& bitmap, uint32_t expectedID) {
    SkASSERT(bitmap.getGenerationID() == expectedID);
    SkASSERT(bitmap.isImmutable());
    SkASSERT(bitmap.getPixels());
    return true;
}

// Note, this returns a new, mutable, bitmap, with a new genID.
// If you want the immutable bitmap with the same ID as our cacherator, call tryLockAsBitmap()
//
bool SkImageCacherator::generateBitmap(SkBitmap* bitmap) {
    SkBitmap::Allocator* allocator = SkResourceCache::GetAllocator();

    ScopedGenerator generator(this);
    const SkImageInfo& genInfo = generator->getInfo();
    if (fInfo.dimensions() == genInfo.dimensions()) {
        SkASSERT(fOrigin.x() == 0 && fOrigin.y() == 0);
        // fast-case, no copy needed
        return generator->tryGenerateBitmap(bitmap, fInfo, allocator);
    } else {
        // need to handle subsetting, so we first generate the full size version, and then
        // "read" from it to get our subset. See skbug.com/4213

        SkBitmap full;
        if (!generator->tryGenerateBitmap(&full, genInfo, allocator)) {
            return false;
        }
        if (!bitmap->tryAllocPixels(fInfo, nullptr, full.getColorTable())) {
            return false;
        }
        return full.readPixels(bitmap->info(), bitmap->getPixels(), bitmap->rowBytes(),
                               fOrigin.x(), fOrigin.y());
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////

bool SkImageCacherator::tryLockAsBitmap(SkBitmap* bitmap, const SkImage* client) {
    if (SkBitmapCache::Find(fUniqueID, bitmap)) {
        return check_output_bitmap(*bitmap, fUniqueID);
    }

    if (!this->generateBitmap(bitmap)) {
        return false;
    }

    bitmap->pixelRef()->setImmutableWithID(fUniqueID);
    SkBitmapCache::Add(fUniqueID, *bitmap);
    if (client) {
        as_IB(client)->notifyAddedToCache();
    }

    return true;
}

bool SkImageCacherator::lockAsBitmap(SkBitmap* bitmap, const SkImage* client) {
    if (this->tryLockAsBitmap(bitmap, client)) {
        return check_output_bitmap(*bitmap, fUniqueID);
    }

#if SK_SUPPORT_GPU
    // Try to get a texture and read it back to raster (and then cache that with our ID)
    SkAutoTUnref<GrTexture> tex;

    {
        ScopedGenerator generator(this);
        SkIRect subset = SkIRect::MakeXYWH(fOrigin.x(), fOrigin.y(), fInfo.width(), fInfo.height());
        tex.reset(generator->generateTexture(nullptr, &subset));
    }
    if (!tex) {
        bitmap->reset();
        return false;
    }

    if (!bitmap->tryAllocPixels(fInfo)) {
        bitmap->reset();
        return false;
    }

    const uint32_t pixelOpsFlags = 0;
    if (!tex->readPixels(0, 0, bitmap->width(), bitmap->height(), SkImageInfo2GrPixelConfig(fInfo),
                         bitmap->getPixels(), bitmap->rowBytes(), pixelOpsFlags)) {
        bitmap->reset();
        return false;
    }

    bitmap->pixelRef()->setImmutableWithID(fUniqueID);
    SkBitmapCache::Add(fUniqueID, *bitmap);
    if (client) {
        as_IB(client)->notifyAddedToCache();
    }

    return check_output_bitmap(*bitmap, fUniqueID);
#else
    return false;
#endif
}

//////////////////////////////////////////////////////////////////////////////////////////////////

#if SK_SUPPORT_GPU

static GrTexture* load_compressed_into_texture(GrContext* ctx, SkData* data, GrSurfaceDesc desc) {
    const void* rawStart;
    GrPixelConfig config = GrIsCompressedTextureDataSupported(ctx, data, desc.fWidth, desc.fHeight,
                                                              &rawStart);
    if (kUnknown_GrPixelConfig == config) {
        return nullptr;
    }

    desc.fConfig = config;
    return ctx->textureProvider()->createTexture(desc, true, rawStart, 0);
}

class Generator_GrYUVProvider : public GrYUVProvider {
    SkImageGenerator* fGen;

public:
    Generator_GrYUVProvider(SkImageGenerator* gen) : fGen(gen) {}

    uint32_t onGetID() override { return fGen->uniqueID(); }
    bool onGetYUVSizes(SkISize sizes[3]) override {
        return fGen->getYUV8Planes(sizes, nullptr, nullptr, nullptr);
    }
    bool onGetYUVPlanes(SkISize sizes[3], void* planes[3], size_t rowBytes[3],
                        SkYUVColorSpace* space) override {
        return fGen->getYUV8Planes(sizes, planes, rowBytes, space);
    }
};

static GrTexture* set_key_and_return(GrTexture* tex, const GrUniqueKey& key) {
    if (key.isValid()) {
        tex->resourcePriv().setUniqueKey(key);
    }
    return tex;
}

/*
 *  We have a 5 ways to try to return a texture (in sorted order)
 *
 *  1. Check the cache for a pre-existing one
 *  2. Ask the generator to natively create one
 *  3. Ask the generator to return a compressed form that the GPU might support
 *  4. Ask the generator to return YUV planes, which the GPU can convert
 *  5. Ask the generator to return RGB(A) data, which the GPU can convert
 */
GrTexture* SkImageCacherator::lockTexture(GrContext* ctx, const GrUniqueKey& key,
                                          const SkImage* client) {
    // 1. Check the cache for a pre-existing one
    if (key.isValid()) {
        if (GrTexture* tex = ctx->textureProvider()->findAndRefTextureByUniqueKey(key)) {
            return tex;
        }
    }

    // 2. Ask the generator to natively create one
    {
        ScopedGenerator generator(this);
        SkIRect subset = SkIRect::MakeXYWH(fOrigin.x(), fOrigin.y(), fInfo.width(), fInfo.height());
        if (GrTexture* tex = generator->generateTexture(ctx, &subset)) {
            return set_key_and_return(tex, key);
        }
    }

    const GrSurfaceDesc desc = GrImageInfoToSurfaceDesc(fInfo);

    // 3. Ask the generator to return a compressed form that the GPU might support
    SkAutoTUnref<SkData> data(this->refEncoded());
    if (data) {
        GrTexture* tex = load_compressed_into_texture(ctx, data, desc);
        if (tex) {
            return set_key_and_return(tex, key);
        }
    }

    // 4. Ask the generator to return YUV planes, which the GPU can convert
    {
        ScopedGenerator generator(this);
        Generator_GrYUVProvider provider(generator);
        GrTexture* tex = provider.refAsTexture(ctx, desc, true);
        if (tex) {
            return set_key_and_return(tex, key);
        }
    }

    // 5. Ask the generator to return RGB(A) data, which the GPU can convert
    SkBitmap bitmap;
    if (this->tryLockAsBitmap(&bitmap, client)) {
        GrTexture* tex = GrUploadBitmapToTexture(ctx, bitmap);
        if (tex) {
            return set_key_and_return(tex, key);
        }
    }
    return nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

#include "GrTextureParamsAdjuster.h"

class Cacherator_GrTextureParamsAdjuster : public GrTextureParamsAdjuster {
public:
    Cacherator_GrTextureParamsAdjuster(SkImageCacherator* cacher, const SkImage* client)
        : INHERITED(cacher->info().width(), cacher->info().height())
        , fCacher(cacher)
        , fClient(client)
    {
        if (client) {
            GrMakeKeyFromImageID(&fOriginalKey, client->uniqueID(),
                                 SkIRect::MakeWH(this->width(), this->height()));
        }
    }

protected:
    GrTexture* peekOriginalTexture() override { return nullptr; }

    // TODO: consider overriding this, for the case where the underlying generator might be
    //       able to efficiently produce a "stretched" texture natively (e.g. picture-backed)
    //    GrTexture* generateTextureForParams(GrContext*, const SkGrStretch&) override;

    GrTexture* refOriginalTexture(GrContext* ctx) override {
        return fCacher->lockTexture(ctx, fOriginalKey, fClient);
    }

    void makeCopyKey(const CopyParams& stretch, GrUniqueKey* paramsCopyKey) override {
        if (fOriginalKey.isValid()) {
            MakeCopyKeyFromOrigKey(fOriginalKey, stretch, paramsCopyKey);
        }
    }

    void didCacheCopy(const GrUniqueKey& copyKey) override {
        if (fClient) {
            as_IB(fClient)->notifyAddedToCache();
        }
    }

    bool getROBitmap(SkBitmap* bitmap) override {
        return fCacher->lockAsBitmap(bitmap, fClient);
    }

private:
    SkImageCacherator*      fCacher;
    const SkImage*          fClient;
    GrUniqueKey             fOriginalKey;

    typedef GrTextureParamsAdjuster INHERITED;
};

GrTexture* SkImageCacherator::lockAsTexture(GrContext* ctx, const GrTextureParams& params,
                                            const SkImage* client) {
    if (!ctx) {
        return nullptr;
    }

    return Cacherator_GrTextureParamsAdjuster(this, client).refTextureForParams(ctx, params);
}

#else

GrTexture* SkImageCacherator::lockAsTexture(GrContext* ctx, const GrTextureParams&,
                                            const SkImage* client) {
    return nullptr;
}

#endif
