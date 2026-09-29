package org.minicode.editor

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.util.LruCache
import java.io.ByteArrayOutputStream
import java.net.URL
import java.util.concurrent.Executors
import javax.net.ssl.HttpsURLConnection

/**
 * Pictures from https addresses, for the Markdown preview, as the Mac and
 * Linux previews fetch them (their `markdown.web-images` setting; here the
 * ⋮ menu's "Web images in Markdown").
 *
 * Only the platform: HttpsURLConnection on three background threads, with
 * the system's own certificate checks, a 15 second timeout on connecting and
 * on every read, no cookies (nothing in MiniCode installs a CookieHandler,
 * and the browser pane's cookies live in the web view, not here), no HTTP
 * cache and a User-Agent of "MiniCode/<version>". Plain http is never
 * fetched, and a redirect from https to http is not followed.
 *
 * What arrives is kept in memory only: the encoded bytes in an LruCache of
 * 32 MB keyed by address, so a re-render or reopening the file costs no
 * request. A picture over 20 MB is cut off where it passes that and counts
 * as a failure. Failures are remembered for a minute, so a page with a
 * missing picture does not ask again on every edit. SVG is refused from its
 * Content-Type without reading the body, since ImageDecoder cannot draw it.
 *
 * Everything but the download itself runs on the main thread.
 */
object WebImages {
    const val TAG = "MiniCodeWeb"
    const val MAX_BYTES = 20 * 1024 * 1024
    private const val CACHE_BYTES = 32 * 1024 * 1024
    private const val TIMEOUT_MS = 15_000
    /** A server that trickles bytes inside the read timeout still ends here. */
    private const val WHOLE_MS = 60_000L
    private const val FAILURE_MS = 60_000L

    /**
     * The preference, set by MainActivity. Read again when a queued fetch
     * starts, so turning it off stops what has not gone out yet.
     */
    @Volatile var enabled = true

    private val main = Handler(Looper.getMainLooper())
    private val pool = Executors.newFixedThreadPool(3) { r ->
        Thread(r, "MiniCodeWeb").apply { isDaemon = true }
    }
    private val cache = object : LruCache<String, ByteArray>(CACHE_BYTES) {
        override fun sizeOf(key: String, value: ByteArray) = value.size
    }
    /** Address -> when it failed (elapsedRealtime). */
    private val failures = HashMap<String, Long>()
    /** Addresses being fetched -> who is waiting for them. */
    private val waiting = HashMap<String, MutableList<(ByteArray?) -> Unit>>()
    private var agent: String? = null

    /** Whether `src` is an address this fetches: https only. */
    fun isWeb(src: String) = src.regionMatches(0, "https://", 0, 8, ignoreCase = true)

    /** The bytes of a picture that has arrived, if they are still kept. */
    fun cached(url: String): ByteArray? = cache.get(url)

    fun failedRecently(url: String): Boolean {
        val at = failures[url] ?: return false
        if (SystemClock.elapsedRealtime() - at < FAILURE_MS) return true
        failures.remove(url)
        return false
    }

    /** Bytes that arrived but would not decode: forgotten, and not asked for again soon. */
    fun undecodable(url: String) {
        cache.remove(url)
        failures[url] = SystemClock.elapsedRealtime()
        log { "not a picture Android can decode: $url" }
    }

    /**
     * Fetches `url` in the background and calls `done` on the main thread
     * with its bytes, or null when it failed or fetching is off. Asking for
     * an address already on its way adds to its waiters, never a request.
     */
    fun fetch(context: Context, url: String, done: (ByteArray?) -> Unit) {
        cache.get(url)?.let { bytes -> main.post { done(bytes) }; return }
        if (!enabled || failedRecently(url)) { main.post { done(null) }; return }
        waiting[url]?.let { it.add(done); return }
        waiting[url] = mutableListOf(done)
        val ua = userAgent(context)
        pool.execute {
            // Turned off while this waited in the queue: nothing goes out,
            // and it is not a failure, so turning it on again asks at once.
            val tried = enabled
            var bytes: ByteArray? = null
            try {
                if (tried) bytes = download(url, ua)
            } finally {
                // Always answered, or the address would wait forever.
                val result = bytes
                main.post {
                    if (result != null) cache.put(url, result)
                    else if (tried) failures[url] = SystemClock.elapsedRealtime()
                    waiting.remove(url)?.forEach { it(result) }
                }
            }
        }
    }

    private fun userAgent(context: Context): String = agent ?: run {
        val version = try {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName
        } catch (e: Exception) { null }
        "MiniCode/${version ?: "0"}".also { agent = it }
    }

    /** One GET, on a pool thread: the body, or null for anything but a picture under the cap. */
    private fun download(url: String, ua: String): ByteArray? {
        val started = SystemClock.elapsedRealtime()
        log { "GET $url" }
        var conn: HttpsURLConnection? = null
        try {
            conn = URL(url).openConnection() as? HttpsURLConnection ?: return null
            conn.connectTimeout = TIMEOUT_MS
            conn.readTimeout = TIMEOUT_MS
            conn.useCaches = false
            conn.instanceFollowRedirects = true
            conn.setRequestProperty("User-Agent", ua)
            conn.setRequestProperty("Accept", "image/*,*/*;q=0.8")
            val code = conn.responseCode
            if (code != 200) { log { "$code for $url" }; return null }
            // Redirects are followed only within https, but say so plainly.
            if (!conn.url.protocol.equals("https", ignoreCase = true)) return null
            val type = conn.contentType.orEmpty().lowercase()
            if (type.startsWith("image/svg") || type.startsWith("text/html")) {
                log { "$type is not drawn: $url" }
                return null
            }
            val length = conn.contentLengthLong
            if (length > MAX_BYTES) { log { "$length bytes, over the cap: $url" }; return null }
            val out = ByteArrayOutputStream(if (length > 0) length.toInt() else 64 * 1024)
            conn.inputStream.use { input ->
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    if (out.size() + n > MAX_BYTES) {
                        log { "passed ${MAX_BYTES / (1024 * 1024)} MB, cut off: $url" }
                        return null
                    }
                    if (SystemClock.elapsedRealtime() - started > WHOLE_MS) {
                        log { "too slow, cut off: $url" }
                        return null
                    }
                    out.write(buffer, 0, n)
                }
            }
            log { "${out.size()} bytes in ${SystemClock.elapsedRealtime() - started} ms: $url" }
            return out.toByteArray()
        } catch (e: Exception) {
            log { "${e.javaClass.simpleName}: ${e.message} for $url" }
            return null
        } catch (e: OutOfMemoryError) {
            return null
        } finally {
            conn?.disconnect()
        }
    }

    /** `adb shell setprop log.tag.MiniCodeWeb DEBUG` turns the log on, in any build. */
    private inline fun log(text: () -> String) {
        if (Log.isLoggable(TAG, Log.DEBUG)) Log.d(TAG, text())
    }
}
