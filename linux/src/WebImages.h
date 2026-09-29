// WebImages.h — pictures from https:// addresses for the Markdown preview.
// Fetched in the background with libsoup 3 (the HTTP library WebKitGTK 6 is
// built on), decoded on a worker thread the way a local picture is decoded,
// and kept in memory for the next render. Nothing is written to disk: the
// session has no cookie jar and no cache, and the kept pictures go with the
// process. The rules and limits are in WebImageRules.h.
//
// Built without libsoup (no MINICODE_ENABLE_WEB_IMAGES), available() is
// false and nothing is ever fetched; the preview shows alt text, as before.
//
// G_MESSAGES_DEBUG=minicode-web logs every request, redirect, failure and
// what the cache holds.
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <memory>
#include <string>

namespace WebImages {

// A decoded picture, shared by every preview showing it. It holds its own
// references; MdPicture takes another.
struct Picture {
    GdkTexture* texture = nullptr;             // a still picture
    GdkPixbufAnimation* animation = nullptr;   // or an animated GIF
    int width = 0, height = 0;                 // its size in pixels
    ~Picture();
};
using PicturePtr = std::shared_ptr<const Picture>;

// Built with libsoup.
bool available();

// An https:// address: the only kind fetched (WebImageRules::isFetchable).
bool isFetchable(const std::string& src);

// The picture kept for `url`, or null.
PicturePtr cached(const std::string& url);

// Fetching `url` failed a short while ago; it is not asked for again yet.
bool recentlyFailed(const std::string& url);

// Fetches `url` in the background, unless it is already being fetched, in
// which case `done` waits for that fetch. `done` runs once, on the main
// thread and never before fetch() returns, with the picture or null. A
// picture too big for the cache is still handed to `done`.
void fetch(const std::string& url, std::function<void(PicturePtr)> done);

}  // namespace WebImages
