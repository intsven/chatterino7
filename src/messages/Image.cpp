#include "messages/Image.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "debug/Benchmark.hpp"
#include "singletons/helper/GifTimer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"
#include "util/GifDebugLog.hpp"
#include "util/PostToThread.hpp"

#include <boost/functional/hash.hpp>
#include <QBuffer>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <QSemaphore>

#include <atomic>

// Duration between each check of every Image instance
const auto IMAGE_POOL_CLEANUP_INTERVAL = std::chrono::minutes(1);
// Duration since last usage of Image pixmap before expiration of frames

// GIF load queue: limit concurrent giphy.com fetches to prevent Qt's
// connection pool from being exhausted (Qt default: 6 per host).
// Non-giphy loads (emotes, badges, etc.) are not throttled.
static QSemaphore s_giphyLoadSlots(3);
const auto IMAGE_POOL_IMAGE_LIFETIME = std::chrono::minutes(10);

using namespace chatterino::literals;

namespace chatterino::detail {

Frames::Frames()
{
    DebugCount::increase("images");
}

Frames::Frames(QList<Frame> &&frames)
    : items_(std::move(frames))
{
    assertInGuiThread();
    auto *app = tryGetApp();
    if (app == nullptr)
    {
        qCDebug(chatterinoImage)
            << "Frames constructor called while app is shutting down";
        return;
    }

    DebugCount::increase("images");
    if (!this->empty())
    {
        DebugCount::increase("loaded images");
    }

    if (this->animated())
    {
        DebugCount::increase("animated images");

        this->gifTimerConnection_ =
            app->getEmotes()->getGIFTimer()->signal.connect([this] {
                this->advance();
            });

        auto totalLength =
            std::accumulate(this->items_.begin(), this->items_.end(), 0UL,
                            [](auto init, auto &&frame) {
                                return init + frame.duration;
                            });

        if (totalLength == 0)
        {
            this->durationOffset_ = 0;
        }
        else
        {
            this->durationOffset_ = std::min<int>(
                int(app->getEmotes()->getGIFTimer()->position() % totalLength),
                60000);
        }
        this->processOffset();
    }

    DebugCount::increase("image bytes", this->memoryUsage());
    DebugCount::increase("image bytes (ever loaded)", this->memoryUsage());
}

Frames::~Frames()
{
    assertInGuiThread();
    DebugCount::decrease("images");
    if (!this->empty())
    {
        DebugCount::decrease("loaded images");
    }

    if (this->animated())
    {
        DebugCount::decrease("animated images");
    }
    DebugCount::decrease("image bytes", this->memoryUsage());
    DebugCount::increase("image bytes (ever unloaded)", this->memoryUsage());

    this->gifTimerConnection_.disconnect();
}

int64_t Frames::memoryUsage() const
{
    int64_t usage = 0;
    for (const auto &frame : this->items_)
    {
        auto sz = frame.image.size();
        auto area = sz.width() * sz.height();
        auto memory = area * frame.image.depth() / 8;

        usage += memory;
    }
    return usage;
}

void Frames::advance()
{
    this->durationOffset_ += GIF_FRAME_LENGTH;
    this->processOffset();
}

void Frames::processOffset()
{
    if (this->items_.isEmpty())
    {
        return;
    }

    while (true)
    {
        this->index_ %= this->items_.size();

        if (this->durationOffset_ > this->items_[this->index_].duration)
        {
            this->durationOffset_ -= this->items_[this->index_].duration;
            this->index_ = (this->index_ + 1) % this->items_.size();
        }
        else
        {
            break;
        }
    }
}

void Frames::clear()
{
    assertInGuiThread();
    if (!this->empty())
    {
        DebugCount::decrease("loaded images");
    }
    DebugCount::decrease("image bytes", this->memoryUsage());
    DebugCount::increase("image bytes (ever unloaded)", this->memoryUsage());

    this->items_.clear();
    this->index_ = 0;
    this->durationOffset_ = 0;
    this->gifTimerConnection_.disconnect();
}

bool Frames::empty() const
{
    return this->items_.empty();
}

bool Frames::animated() const
{
    return this->items_.size() > 1;
}

std::optional<QPixmap> Frames::current() const
{
    if (this->items_.empty())
    {
        return std::nullopt;
    }

    return this->items_[this->index_].image;
}

std::optional<QPixmap> Frames::first() const
{
    if (this->items_.empty())
    {
        return std::nullopt;
    }

    return this->items_.front().image;
}

QList<Frame> readFrames(QImageReader &reader, const Url &url)
{
    QList<Frame> frames;
    frames.reserve(reader.imageCount());

    for (int index = 0; index < reader.imageCount(); ++index)
    {
        auto pixmap = QPixmap::fromImageReader(&reader);
        if (!pixmap.isNull())
        {
            gifLog(QStringLiteral("[readFrames] Frame %1 decoded: %2x%3")
                       .arg(index)
                       .arg(pixmap.width())
                       .arg(pixmap.height()));
            int duration = reader.nextImageDelay();
            if (duration <= 10)
            {
                duration = 100;
            }
            duration = std::max(20, duration);
            frames.append(Frame{
                .image = std::move(pixmap),
                .duration = duration,
            });
        }
        else
        {
            gifLog(QStringLiteral("[readFrames] Frame %1 is NULL for %2, error=%3")
                       .arg(index)
                       .arg(url.string)
                       .arg(reader.errorString()));
        }
    }

    if (frames.empty())
    {
        qCDebug(chatterinoImage) << "Error while reading image" << url.string
                                 << ": '" << reader.errorString() << "'";
    }

    return frames;
}

void assignFrames(std::weak_ptr<Image> weak, QList<Frame> parsed)
{
    static bool isPushQueued;

    auto cb = [parsed = std::move(parsed), weak = std::move(weak)]() mutable {
        auto shared = weak.lock();
        if (!shared)
        {
            gifLog(QStringLiteral("[assignFrames] Image expired before frames assigned"));
            return;
        }
        shared->frames_ = std::make_unique<detail::Frames>(std::move(parsed));
        gifLog(QStringLiteral("[assignFrames] Frames assigned to %1, frames_empty=%2")
                   .arg(shared->url().string)
                   .arg(shared->frames_->empty()));

        // Avoid too many layouts in one event-loop iteration
        //
        // This callback is called for every image, so there might be multiple
        // callbacks queued on the event-loop in this iteration, but we only
        // want to generate one invalidation.
        if (!isPushQueued)
        {
            isPushQueued = true;
            // We don't use postToThread here, because that would run immediately.
            // We explicitly want to queue a callback after the current ones.
            QMetaObject::invokeMethod(
                qApp,
                [] {
                    isPushQueued = false;
                    auto *app = tryGetApp();
                    if (app != nullptr)
                    {
                        app->getWindows()->forceLayoutChannelViews();
                    }
                },
                Qt::QueuedConnection);
        }
    };

    postToGuiThread(cb);
}

void notifyImageFailed()
{
    // Trigger re-layout so the next image in the ImageSet can be tried.
    // Uses the same batching as assignFrames to avoid too many layouts.
    static bool isPushQueued;
    if (!isPushQueued)
    {
        isPushQueued = true;
        QMetaObject::invokeMethod(
            qApp,
            [] {
                isPushQueued = false;
                auto *app = tryGetApp();
                if (app != nullptr)
                {
                    app->getWindows()->forceLayoutChannelViews();
                }
            },
            Qt::QueuedConnection);
    }
}

}  // namespace chatterino::detail

namespace chatterino {

// IMAGE2
Image::~Image()
{
#ifndef DISABLE_IMAGE_EXPIRATION_POOL
    ImageExpirationPool::instance().removeImagePtr(this);
#endif

    if (this->empty_ && !this->frames_)
    {
        // No data in this image, don't bother trying to release it
        // The reason we do this check is that we keep a few (or one) static empty image around that are deconstructed at the end of the programs lifecycle, and we want to prevent the isGuiThread call to be called after the QApplication has been exited
        return;
    }

    if (isAppAboutToQuit())
    {
        if (this->frames_)
        {
            std::ignore = this->frames_.release();
        }
        return;
    }

    // Ensure the destructor for our frames is called in the GUI thread
    // If the Image destructor is called outside of the GUI thread, move the
    // ownership of the frames to the GUI thread, otherwise the frames will be
    // destructed as part as we go out of scope
    if (!isGuiThread())
    {
        postToThread([frames = this->frames_.release()]() {
            delete frames;
        });
    }
}

ImagePtr Image::fromUrl(const Url &url, qreal scale, QSize expectedSize)
{
    static std::unordered_map<Url, std::weak_ptr<Image>> cache;
    static std::mutex mutex;

    std::lock_guard<std::mutex> lock(mutex);

    auto shared = cache[url].lock();

    if (!shared)
    {
        cache[url] = shared = ImagePtr(new Image(url, scale, expectedSize));
    }

    return shared;
}

ImagePtr Image::fromResourcePixmap(const QPixmap &pixmap, qreal scale)
{
    using key_t = std::pair<const QPixmap *, qreal>;
    static std::unordered_map<key_t, std::weak_ptr<Image>, boost::hash<key_t>>
        cache;
    static std::mutex mutex;

    std::lock_guard<std::mutex> lock(mutex);

    auto it = cache.find({&pixmap, scale});
    if (it != cache.end())
    {
        auto shared = it->second.lock();
        if (shared)
        {
            return shared;
        }

        cache.erase(it);
    }

    auto newImage = ImagePtr(new Image(scale));

    newImage->setPixmap(pixmap);

    // store in cache
    cache.insert({{&pixmap, scale}, std::weak_ptr<Image>(newImage)});

    return newImage;
}

ImagePtr Image::getEmpty()
{
    static auto empty = ImagePtr(new Image);
    return empty;
}

ImagePtr getEmptyImagePtr()
{
    return Image::getEmpty();
}

Image::Image()
    : empty_(true)
{
}

Image::Image(const Url &url, qreal scale, QSize expectedSize)
    : url_(url)
    , scale_(scale)
    , expectedSize_(expectedSize.isValid() ? expectedSize
                                           : (QSize(16, 16) * scale))
    , shouldLoad_(true)
    , frames_(std::make_unique<detail::Frames>())
{
}

Image::Image(qreal scale)
    : scale_(scale)
    , frames_(std::make_unique<detail::Frames>())
{
}

void Image::setPixmap(const QPixmap &pixmap)
{
    auto setFrames = [shared = this->shared_from_this(), pixmap]() {
        shared->frames_ = std::make_unique<detail::Frames>(
            QList<detail::Frame>{detail::Frame{pixmap, 1}});
    };

    if (isGuiThread())
    {
        setFrames();
    }
    else
    {
        postToThread(setFrames);
    }
}

const Url &Image::url() const
{
    return this->url_;
}

bool Image::loaded() const
{
    assertInGuiThread();

    return this->frames_->current().has_value();
}

std::optional<QPixmap> Image::pixmapOrLoad() const
{
    assertInGuiThread();

    // Mark the image as just used.
    this->lastUsed_ = std::chrono::steady_clock::now();

    this->load();

    auto result = this->frames_->current();
    if (!result && !this->empty_)
    {
        // Image not yet loaded and not failed — will be painted blank this frame
    }
    return result;
}

void Image::load() const
{
    assertInGuiThread();

    if (this->shouldLoad_)
    {
        Image *this2 = const_cast<Image *>(this);
        this2->shouldLoad_ = false;
        this2->actuallyLoad();
#ifndef DISABLE_IMAGE_EXPIRATION_POOL
        ImageExpirationPool::instance().addImagePtr(this2->shared_from_this());
#endif
    }
}

qreal Image::scale() const
{
    return this->scale_;
}

bool Image::isEmpty() const
{
    return this->empty_;
}

bool Image::shouldLoad() const
{
    return this->shouldLoad_;
}

bool Image::hasFrames() const
{
    return !this->frames_->empty();
}

bool Image::animated() const
{
    assertInGuiThread();

    return this->frames_->animated();
}

int Image::width() const
{
    assertInGuiThread();

    if (auto pixmap = this->frames_->first())
    {
        return static_cast<int>(pixmap->width() * this->scale_);
    }

    // No frames loaded, use the expected size
    return static_cast<int>(this->expectedSize_.width() * this->scale_);
}

int Image::height() const
{
    assertInGuiThread();

    if (auto pixmap = this->frames_->first())
    {
        return static_cast<int>(pixmap->height() * this->scale_);
    }

    // No frames loaded, use the expected size
    return static_cast<int>(this->expectedSize_.height() * this->scale_);
}

QSizeF Image::size() const
{
    assertInGuiThread();

    if (auto pixmap = this->frames_->first())
    {
        auto pixmapSize = pixmap->size().toSizeF() * this->scale_;

        // If expectedSize is set and the loaded image is larger in BOTH
        // dimensions, scale down to expected size. This handles Kick emotes
        // which provide high-res images (up to 500x500) that should display at
        // standard size. Using && (not ||) preserves aspect ratio for wide or
        // tall emotes (e.g. BTTV) where only one dimension exceeds expected.
        if (this->expectedSize_.isValid())
        {
            auto expectedSize = this->expectedSize_.toSizeF() * this->scale_;
            if (pixmapSize.width() > expectedSize.width() &&
                pixmapSize.height() > expectedSize.height())
            {
                auto scaleW = expectedSize.width() / pixmapSize.width();
                auto scaleH = expectedSize.height() / pixmapSize.height();
                return pixmapSize * std::min(scaleW, scaleH);
            }
        }

        return pixmapSize;
    }

    // No frames loaded, use the expected size
    return this->expectedSize_.toSizeF() * this->scale_;
}

void Image::actuallyLoad()
{
    auto weak = weakOf(this);
    bool isGiphy = this->url().string.contains("giphy.com");

    // For giphy URLs, enforce a load queue to prevent Qt's connection pool
    // from being exhausted (Qt defaults to 6 per host).
    if (isGiphy && !s_giphyLoadSlots.tryAcquire())
    {
        qCDebug(chatterinoImage)
            << "[GIF] Queue full, deferring:" << this->url().string;
        // Retry after a short delay. The image stays in "loading" state
        // (shouldLoad_=false, no frames) so addToContainer shows the title.
        QTimer::singleShot(300, [weak]() {
            if (auto img = weak.lock())
            {
                img->actuallyLoad();
            }
        });
        return;
    }

    gifLog(QStringLiteral("[actuallyLoad] Starting fetch: %1").arg(this->url().string));
    NetworkRequest(this->url().string)
        .concurrent()
        .cache()
        .timeout(15000)
        .onSuccess([weak, isGiphy](auto result) {
            if (isGiphy)
            {
                s_giphyLoadSlots.release();
            }

            auto shared = weak.lock();
            if (!shared)
            {
                qCDebug(chatterinoImage)
                    << "[GIF] SUCCESS but Image expired";
                return;
            }

            assert(!isAppAboutToQuit());

            QBuffer buffer;
            buffer.setData(result.getData());
            QImageReader reader(&buffer);

            if (!reader.canRead())
            {
                gifLog(QStringLiteral("[actuallyLoad] canRead=FALSE url=%1 "
                       "error=%2 dataLen=%3")
                           .arg(shared->url().string,
                                reader.errorString())
                           .arg(result.getData().size()));
                qCDebug(chatterinoImage)
                    << "[GIF] canRead=FALSE url=" << shared->url().string
                    << "error=" << reader.errorString()
                    << "dataLen=" << result.getData().size();
                shared->empty_ = true;
                detail::notifyImageFailed();
                return;
            }

            const auto size = reader.size();
            if (size.isEmpty())
            {
                gifLog(QStringLiteral("[actuallyLoad] size isEmpty url=%1")
                           .arg(shared->url().string));
                qCDebug(chatterinoImage)
                    << "[GIF] size isEmpty url=" << shared->url().string;
                shared->empty_ = true;
                detail::notifyImageFailed();
                return;
            }

            // returns 1 for non-animated formats
            if (reader.imageCount() <= 0)
            {
                gifLog(QStringLiteral("[actuallyLoad] imageCount<=0 url=%1 "
                       "error=%2")
                           .arg(shared->url().string,
                                reader.errorString()));
                qCDebug(chatterinoImage)
                    << "[GIF] imageCount <= 0 url=" << shared->url().string
                    << "error=" << reader.errorString();
                shared->empty_ = true;
                detail::notifyImageFailed();
                return;
            }

            // For giphy GIFs, downsample at decode time to the display size
            // (same maxGifHeight / 2x-width caps as TwitchGifElement).
            // Full-res GIFs with many frames would otherwise exceed
            // maxBytesRam (e.g. 480x480x68 frames ~= 63MB) and be rejected
            // on all 3 URLs, leaving only fallback text.
            QSize decodeSize = size;
            if (isGiphy)
            {
                int maxH = getSettings()->maxGifHeight.getValue();
                int maxW = maxH * 2;
                double s = std::min(
                    {1.0, double(maxW) / double(size.width()),
                     double(maxH) / double(size.height())});
                if (s < 1.0)
                {
                    decodeSize = QSize(std::max(1, int(size.width() * s)),
                                       std::max(1, int(size.height() * s)));
                    if (reader.supportsOption(QImageIOHandler::ScaledSize))
                    {
                        reader.setScaledSize(decodeSize);
                        // size() reflects the scaled size when supported
                        decodeSize = reader.size();
                    }
                    gifLog(QStringLiteral(
                               "[actuallyLoad] downsampling giphy %1x%2 -> "
                               "%3x%4 url=%5")
                               .arg(size.width())
                               .arg(size.height())
                               .arg(decodeSize.width())
                               .arg(decodeSize.height())
                               .arg(shared->url().string));
                }
            }

            // use "double" to prevent int overflows
            double estBytes = double(decodeSize.width()) *
                              double(decodeSize.height()) *
                              double(reader.imageCount()) * 4.0;
            if (estBytes > double(Image::maxBytesRam))
            {
                gifLog(QStringLiteral(
                           "[actuallyLoad] image too large url=%1 %2x%3 "
                           "frames=%4 estBytes=%5 limit=%6 dataLen=%7")
                           .arg(shared->url().string)
                           .arg(decodeSize.width())
                           .arg(decodeSize.height())
                           .arg(reader.imageCount())
                           .arg(qint64(estBytes))
                           .arg(Image::maxBytesRam)
                           .arg(result.getData().size()));
                qCDebug(chatterinoImage)
                    << "[GIF] image too large in RAM url=" << shared->url().string;

                shared->empty_ = true;
                detail::notifyImageFailed();
                return;
            }

            qCDebug(chatterinoImage)
                << "[GIF] Decoding OK:" << shared->url().string
                << size.width() << "x" << size.height()
                << "decode=" << decodeSize.width() << "x"
                << decodeSize.height() << "frames=" << reader.imageCount()
                << "dataLen=" << result.getData().size();

            auto parsed = detail::readFrames(reader, shared->url());

            gifLog(QStringLiteral("[actuallyLoad] readFrames returned %1 frames for %2")
                       .arg(parsed.size())
                       .arg(shared->url().string));

            assignFrames(shared, parsed);
        })
        .onError([weak, isGiphy](auto result) {
            if (isGiphy)
            {
                s_giphyLoadSlots.release();
            }

            auto shared = weak.lock();
            if (!shared)
            {
                return false;
            }

            auto status = result.status().value_or(-1);
            gifLog(QStringLiteral("[actuallyLoad] NETWORK ERROR url=%1 status=%2")
                       .arg(shared->url().string)
                       .arg(status));

            qCDebug(chatterinoImage)
                << "[GIF] NETWORK ERROR url=" << shared->url().string
                << "status=" << status;

            // Retry giphy URLs once after a short delay — network errors
            // during startup are often caused by connection pool exhaustion
            // when many images load simultaneously.
            if (!shared->retried_ && isGiphy)
            {
                shared->retried_ = true;
                gifLog(QStringLiteral("[actuallyLoad] Retrying in 1s: %1")
                           .arg(shared->url().string));
                qCDebug(chatterinoImage)
                    << "[GIF] Retrying in 1s:" << shared->url().string;
                QTimer::singleShot(1000, [weak]() {
                    if (auto img = weak.lock())
                    {
                        img->actuallyLoad();
                    }
                });
                return true;
            }

            // Mark as empty so it shows as text instead of blank
            shared->empty_ = true;

            // Trigger re-layout so the next image in the ImageSet can be tried
            detail::notifyImageFailed();

            return true;
        })
        .execute();
}

void Image::expireFrames()
{
    assertInGuiThread();
    this->frames_->clear();
    this->shouldLoad_ = true;  // Mark as needing load again
}

#ifndef DISABLE_IMAGE_EXPIRATION_POOL

ImageExpirationPool::ImageExpirationPool()
    : freeTimer_(new QTimer)
{
    QObject::connect(this->freeTimer_, &QTimer::timeout, [this] {
        if (isGuiThread())
        {
            this->freeOld();
        }
        else
        {
            postToThread([this] {
                this->freeOld();
            });
        }
    });

    this->freeTimer_->start(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            IMAGE_POOL_CLEANUP_INTERVAL));

    // configure all debug counts used by images
    DebugCount::configure("image bytes", DebugCount::Flag::DataSize);
    DebugCount::configure("image bytes (ever loaded)",
                          DebugCount::Flag::DataSize);
    DebugCount::configure("image bytes (ever unloaded)",
                          DebugCount::Flag::DataSize);
}

ImageExpirationPool &ImageExpirationPool::instance()
{
    static auto *instance = new ImageExpirationPool;
    return *instance;
}

void ImageExpirationPool::addImagePtr(ImagePtr imgPtr)
{
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->allImages_.emplace(imgPtr.get(), std::weak_ptr<Image>(imgPtr));
}

void ImageExpirationPool::removeImagePtr(Image *rawPtr)
{
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->allImages_.erase(rawPtr);
}

void ImageExpirationPool::freeAll()
{
    {
        std::lock_guard<std::mutex> lock(this->mutex_);
        for (auto it = this->allImages_.begin(); it != this->allImages_.end();)
        {
            auto img = it->second.lock();
            img->expireFrames();
            it = this->allImages_.erase(it);
        }
    }
    this->freeOld();
}

void ImageExpirationPool::freeOld()
{
    std::lock_guard<std::mutex> lock(this->mutex_);

    size_t numExpired = 0;
    size_t eligible = 0;

    auto now = std::chrono::steady_clock::now();
    for (auto it = this->allImages_.begin(); it != this->allImages_.end();)
    {
        auto img = it->second.lock();
        if (!img)
        {
            // This can only really happen from a race condition because ~Image
            // should remove itself from the ImageExpirationPool automatically.
            it = this->allImages_.erase(it);
            continue;
        }

        if (img->frames_->empty())
        {
            // No frame data, nothing to do
            ++it;
            continue;
        }

        ++eligible;

        // Check if image has expired and, if so, expire its frame data
        auto diff = now - img->lastUsed_;
        if (diff > IMAGE_POOL_IMAGE_LIFETIME)
        {
            ++numExpired;
            img->expireFrames();
            // erase without mutex locking issue
            it = this->allImages_.erase(it);
            continue;
        }

        ++it;
    }

#    ifndef NDEBUG
    qCDebug(chatterinoImage) << "freed frame data for" << numExpired << "/"
                             << eligible << "eligible images";
#    endif
    DebugCount::set("last image gc: expired", numExpired);
    DebugCount::set("last image gc: eligible", eligible);
    DebugCount::set("last image gc: left after gc", this->allImages_.size());
}

#endif

}  // namespace chatterino
