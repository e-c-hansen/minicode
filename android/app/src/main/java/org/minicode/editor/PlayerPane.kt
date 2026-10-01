package org.minicode.editor

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.res.ColorStateList
import android.graphics.SurfaceTexture
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaPlayer
import android.util.AttributeSet
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.ViewConfiguration
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.documentfile.provider.DocumentFile
import java.util.Locale
import kotlin.math.abs

/**
 * Video and audio in the editor's slot, as the Mac plays them with AVKit and
 * Linux with GtkVideo: Android's own MediaPlayer drawing into a TextureView,
 * with a bar of controls along the bottom (play or pause, the place, the
 * length). Nothing here comes from outside the platform.
 *
 * - **Paused on the first frame.** Nothing plays until Space, the play button
 *   or Ctrl+Shift+Space. At the end it stops, and playing again starts over.
 * - **Plays only while it can be seen.** When the pane is hidden (the file
 *   list, the terminal, Back, the app going to the background) the player is
 *   released and its place kept; shown again, a new one opens paused at that
 *   place. So no sound comes from a pane that is not on screen.
 * - **Paused for 4 seconds, it is let go of too**, keeping the last frame on
 *   screen, and play or a seek opens a new one. Left paused longer, Android's
 *   player jumps back to a key frame by itself (releaseWhenIdle says why).
 * - **What the file holds decides.** The extension picks video or audio to
 *   begin with; once prepared, an .mp4 of sound alone shows as audio (its
 *   name over the controls) and anything with a video track as video.
 * - **A file Android cannot decode** gets a plain "Cannot play" message in
 *   the player's place, from MediaPlayer's error.
 * - **Keys**, while the pane has the keyboard: Space (and a headset's
 *   play/pause) plays or pauses, Left and Right go back or on 5 seconds.
 *   **Touch**: a tap on the picture shows the bar, which hides itself 3
 *   seconds into playing a video, or hides it again.
 *
 * Not VideoView and MediaController: the controller is a focusable window of
 * its own, so while it shows the leader key and the first Back would go to
 * it, and VideoView's SurfaceView loses its surface, and the player with it,
 * every time the pane is hidden.
 */
class PlayerPane @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : FrameLayout(context, attrs), TextureView.SurfaceTextureListener {

    /** Called when what the title shows changes: the size, the length, a failure. */
    var onChanged: () -> Unit = {}

    private val dp = resources.displayMetrics.density
    private fun px(v: Int) = (v * dp).toInt()

    private var picture = TextureView(context)
    private val name = TextView(context)        // audio: the file's name
    private val message = TextView(context)     // "Cannot play ..."
    private val bar = LinearLayout(context)
    private val button = ImageView(context)
    private val elapsed = TextView(context)
    private val seek = SeekBar(context)
    private val total = TextView(context)

    private var file: DocumentFile? = null
    private var player: MediaPlayer? = null
    private var prepared = false
    private var playing = false
    private var texture: SurfaceTexture? = null
    private var surface: Surface? = null
    /** Shown in the player's place when the file cannot be played. */
    private var failed: String? = null
    /** Where the next player starts, and whether it plays (a reload keeps both). */
    private var resumeAt = 0
    private var resumePlaying = false
    /** Played to the end: playing again starts over. */
    private var atEnd = false
    private var hasVideo = false
    private var videoWidth = 0
    private var videoHeight = 0
    private var duration = -1
    private var dragging = false
    private var downX = 0f
    private var downY = 0f

    private val audio = context.getSystemService(AudioManager::class.java)
    private var focus: AudioFocusRequest? = null
    private var noisyHeard = false

    /** Set last in init: View's constructor reports visibility before the fields exist. */
    private var ready = false

    init {
        setBackgroundColor(Palette.BACKGROUND)
        // The pane holds the keyboard itself; the bar's parts never take it.
        isFocusable = true
        isFocusableInTouchMode = true
        descendantFocusability = FOCUS_BLOCK_DESCENDANTS
        defaultFocusHighlightEnabled = false

        addPicture()
        for (t in listOf(name, message)) {
            t.setTextColor(Palette.TEXT)
            t.gravity = Gravity.CENTER
            t.setPadding(px(24), px(24), px(24), px(24))
            addView(t, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT,
                                    Gravity.CENTER))
        }
        name.textSize = 18f
        message.textSize = 15f

        button.setImageResource(android.R.drawable.ic_media_play)
        button.contentDescription = "Play"
        button.scaleType = ImageView.ScaleType.CENTER_INSIDE
        button.setOnClickListener { togglePlay() }
        for (t in listOf(elapsed, total)) {
            t.setTextColor(Palette.MUTED)
            t.textSize = 12f
        }
        total.setPadding(0, 0, px(8), 0)
        seek.progressTintList = ColorStateList.valueOf(Palette.ACCENT)
        seek.thumbTintList = ColorStateList.valueOf(Palette.ACCENT)
        seek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(s: SeekBar, value: Int, fromUser: Boolean) {
                if (!fromUser) return
                elapsed.text = clock(value)
                // To the nearest key frame while dragging, which is quick;
                // exactly where it was let go.
                seekTo(value, MediaPlayer.SEEK_CLOSEST_SYNC)
            }
            override fun onStartTrackingTouch(s: SeekBar) {
                dragging = true
                removeCallbacks(hideBar)
            }
            override fun onStopTrackingTouch(s: SeekBar) {
                dragging = false
                seekTo(s.progress, MediaPlayer.SEEK_CLOSEST)
                showBar()
            }
        })
        bar.orientation = LinearLayout.HORIZONTAL
        bar.gravity = Gravity.CENTER_VERTICAL
        bar.setBackgroundColor(BAR_COLOR)
        bar.isClickable = true   // a tap on the bar is not a tap on the picture
        bar.addView(button, LinearLayout.LayoutParams(px(48), px(48)))
        bar.addView(elapsed, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        bar.addView(seek, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        bar.addView(total, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        addView(bar, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT,
                                  Gravity.BOTTOM))
        // The bar sits on the bottom edge of the screen, whose corners are
        // rounded on a Titan 2, as the key rows do.
        CurvedEdges.keepClear(bar, bar, BAR_TEXT_DP)
        showParts()
        ready = true
    }

    /**
     * A new TextureView for each file, so the last frame of the one before
     * never shows while the next one opens.
     */
    private fun addPicture() {
        picture.surfaceTextureListener = this
        addView(picture, 0, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT,
                                         Gravity.CENTER))
    }

    // ------------------------------------------------------------ what it shows

    /**
     * Opens `f` paused at its start: a video on its first frame, audio as its
     * name over the controls. `video` is the guess from the extension.
     */
    fun open(f: DocumentFile, video: Boolean) {
        releasePlayer(keepPlace = false)
        removeView(picture)
        picture = TextureView(context)
        addPicture()
        file = f
        failed = null
        resumeAt = 0
        resumePlaying = false
        atEnd = false
        hasVideo = video
        videoWidth = 0
        videoHeight = 0
        duration = -1
        name.text = f.name
        seek.max = 0
        seek.progress = 0
        elapsed.text = clock(0)
        total.text = ""
        showParts()
        requestLayout()
        start()
    }

    /**
     * The file changed on disk: a new player for it, at the same place, and
     * playing if it was.
     */
    fun reload() {
        if (file == null) return
        val mp = player
        if (mp != null && prepared) {
            resumeAt = position(mp)
            resumePlaying = playing
        }
        log { "reload ${file?.name}: at $resumeAt ms, ${if (resumePlaying) "playing" else "paused"}" }
        releasePlayer(keepPlace = false)
        failed = null
        showParts()
        start()
    }

    /**
     * The file was moved (dragged in the file list). The pane is hidden
     * then, so the player is already released; it opens `f` when shown.
     */
    fun moved(f: DocumentFile) {
        file = f
        name.text = f.name
    }

    /** The file is gone from the disk. */
    fun gone() = fail("It is not there any more.")

    /** Lets go of the file: another one, a diff or nothing takes the slot. */
    fun close() {
        releasePlayer(keepPlace = false)
        file = null
        failed = null
        removeCallbacks(hideBar)
        showParts()
    }

    /** Pauses, for the window being paused. */
    fun pause() {
        val mp = player
        if (mp != null && prepared && playing) {
            mp.pause()
            log { "paused at ${position(mp)} ms" }
        }
        playing = false
        playingChanged()
    }

    /** Releases the player and keeps its place, for the window being stopped. */
    fun suspend() = releasePlayer(keepPlace = true)

    /**
     * Space, the play button, Ctrl+Shift+Space. A player let go of while
     * paused (releaseWhenIdle) is opened again, and plays once it is ready.
     */
    fun togglePlay() {
        if (file == null || failed != null || !onScreen()) return
        val mp = player
        when {
            mp == null -> {
                if (atEnd) resumeAt = 0
                resumePlaying = true
                start()
            }
            !prepared -> resumePlaying = !resumePlaying
            playing -> pause()
            else -> play()
        }
    }

    /** "640 × 360  0:05" for a video, "0:05" for audio, for the title. */
    fun describe(): String {
        if (file == null || failed != null) return ""
        val parts = mutableListOf<String>()
        if (hasVideo && videoWidth > 0 && videoHeight > 0) parts += "$videoWidth × $videoHeight"
        if (duration >= 0) parts += clock(duration, round = true)
        return parts.joinToString("  ")
    }

    private fun showParts() {
        val bad = failed != null
        message.text = failed.orEmpty()
        message.visibility = if (bad) VISIBLE else GONE
        picture.visibility = if (!bad && file != null && hasVideo) VISIBLE else GONE
        name.visibility = if (!bad && file != null && !hasVideo) VISIBLE else GONE
        bar.visibility = if (!bad && file != null) VISIBLE else GONE
    }

    /** The picture keeps its shape, as large as the pane allows. */
    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        super.onMeasure(widthSpec, heightSpec)
        if (!ready || videoWidth <= 0 || videoHeight <= 0) return
        val scale = minOf(measuredWidth.toFloat() / videoWidth, measuredHeight.toFloat() / videoHeight)
        picture.measure(
            MeasureSpec.makeMeasureSpec((videoWidth * scale).toInt().coerceAtLeast(1), MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec((videoHeight * scale).toInt().coerceAtLeast(1), MeasureSpec.EXACTLY))
    }

    // ------------------------------------------------------------ the player

    private fun onScreen() = isAttachedToWindow && isShown && windowVisibility == VISIBLE

    /** A player for the file, if the pane is on screen and has none. */
    private fun start() {
        val f = file ?: return
        if (player != null || failed != null || !onScreen()) return
        // A video waits for the picture's surface, which comes with its first draw.
        if (hasVideo && surface == null) return
        log { "open ${f.name}: at $resumeAt ms, ${if (resumePlaying) "playing" else "paused"}" }
        val mp = MediaPlayer()
        player = mp
        prepared = false
        mp.setAudioAttributes(attributes())
        mp.setOnPreparedListener { if (it === player) prepared(it) }
        mp.setOnVideoSizeChangedListener { p, w, h -> if (p === player) sized(w, h) }
        mp.setOnCompletionListener { if (it === player) finished() }
        mp.setOnSeekCompleteListener {
            if (it !== player) return@setOnSeekCompleteListener
            log { "seeked: at ${position(it)} ms" }
            if (!dragging) showPlace()
        }
        mp.setOnErrorListener { p, what, extra ->
            log { "error $what, $extra" }
            if (p === player) fail(why(what, extra))
            true
        }
        try {
            surface?.let(mp::setSurface)
            // Through a descriptor, which works the same for a path and for a
            // document from the picker; MediaPlayer keeps its own copy.
            val fd = context.contentResolver.openFileDescriptor(f.uri, "r")
                ?: throw java.io.IOException("no file descriptor")
            fd.use { mp.setDataSource(it.fileDescriptor) }
            mp.prepareAsync()
        } catch (e: Exception) {
            fail("It could not be read (${e.message ?: e.javaClass.simpleName}).")
        }
    }

    private fun prepared(mp: MediaPlayer) {
        prepared = true
        duration = mp.duration
        val tracks = try { mp.trackInfo } catch (e: Exception) { emptyArray() }
        hasVideo = tracks.any { it.trackType == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_VIDEO }
        if (mp.videoWidth > 0 && mp.videoHeight > 0) {
            videoWidth = mp.videoWidth
            videoHeight = mp.videoHeight
        }
        seek.max = duration.coerceAtLeast(0)
        total.text = if (duration >= 0) clock(duration, round = true) else ""
        log { "prepared ${file?.name}: ${if (hasVideo) "video ${videoWidth}x$videoHeight" else "audio"}, " +
              "$duration ms, seeking to $resumeAt" }
        showParts()
        requestLayout()
        // A seek before playing draws that frame, so the picture is not blank.
        mp.seekTo(resumeAt.toLong(), MediaPlayer.SEEK_CLOSEST)
        val play = resumePlaying
        resumePlaying = false
        if (play) play() else { showPlace(); releaseWhenIdle() }
        onChanged()
    }

    /** The size as shown: a phone's portrait clip comes rotated, 1080 × 1920. */
    private fun sized(w: Int, h: Int) {
        if (w <= 0 || h <= 0 || (w == videoWidth && h == videoHeight)) return
        videoWidth = w
        videoHeight = h
        requestLayout()
        onChanged()
    }

    private fun finished() {
        log { "finished" }
        playing = false
        atEnd = true
        playingChanged()
        seek.progress = seek.max
        elapsed.text = total.text
    }

    private fun fail(why: String) {
        log { "cannot play ${file?.name}: $why" }
        releasePlayer(keepPlace = true)
        resumePlaying = false
        failed = "Cannot play ${file?.name ?: "this file"}.\n\n$why"
        showParts()
        onChanged()
    }

    private fun why(what: Int, extra: Int): String = when {
        extra == MediaPlayer.MEDIA_ERROR_UNSUPPORTED ->
            "Android has no decoder for what it holds."
        extra == MediaPlayer.MEDIA_ERROR_MALFORMED ->
            "It is damaged, or not the kind of file its name says."
        extra == MediaPlayer.MEDIA_ERROR_IO -> "It could not be read."
        // MEDIA_ERROR_SYSTEM, which is not public: what a file of random
        // bytes, or one with no decoder on this phone, gets.
        extra == Int.MIN_VALUE -> "Android does not recognise what is in it."
        extra == MediaPlayer.MEDIA_ERROR_TIMED_OUT -> "Reading it took too long."
        what == MediaPlayer.MEDIA_ERROR_SERVER_DIED -> "Android's media service stopped."
        else -> "Android's player could not open it (error $what, $extra)."
    }

    private fun position(mp: MediaPlayer) =
        try { mp.currentPosition } catch (e: IllegalStateException) { resumeAt }

    /** Frees the decoder. With `keepPlace`, the next player starts where this one was. */
    private fun releasePlayer(keepPlace: Boolean) {
        val mp = player ?: return
        if (keepPlace && prepared) {
            resumeAt = position(mp)
            resumePlaying = false
        }
        log { "released ${file?.name}" + if (keepPlace) ", keeping $resumeAt ms" else "" }
        player = null
        prepared = false
        playing = false
        try { mp.release() } catch (e: Exception) {}
        playingChanged()
    }

    private fun play() {
        val mp = player ?: return
        if (!prepared) return
        // Refused during a call, say; then it stays paused.
        if (!takeFocus()) { log { "no audio focus: stays paused" }; return }
        // At the end it starts over. A player that finished does that by
        // itself; one opened again at the end has to be told.
        if (atEnd) mp.seekTo(0, MediaPlayer.SEEK_CLOSEST)
        atEnd = false
        log { "play from ${position(mp)} ms" }
        mp.start()
        playing = true
        playingChanged()
    }

    /**
     * A paused player is let go of after a few seconds, keeping its place
     * (the picture keeps the last frame), and a new one opens when play or
     * a seek asks for it. Kept paused longer, Android's player moves itself:
     * with the sound handed to the audio hardware ("offload", which the Titan
     * 2 uses even beside video), NuPlayer shuts that down after 10 seconds
     * paused and restarts it with a seek to the key frame before the place,
     * so the picture jumped back and playing resumed up to 8 seconds early.
     * No app can turn offload off for MediaPlayer.
     */
    private fun releaseWhenIdle() {
        removeCallbacks(idleRelease)
        if (!playing && player != null) postDelayed(idleRelease, IDLE_RELEASE_MS)
    }

    private val idleRelease = Runnable {
        if (!playing && player != null && prepared) releasePlayer(keepPlace = true)
    }

    /** Everything that follows playing or not: the button, focus, the screen, the bar. */
    private fun playingChanged() {
        button.setImageResource(if (playing) android.R.drawable.ic_media_pause
                                else android.R.drawable.ic_media_play)
        button.contentDescription = if (playing) "Pause" else "Play"
        keepScreenOn = playing && hasVideo
        removeCallbacks(tick)
        if (playing) {
            removeCallbacks(idleRelease)
            listenForNoise(true)
            post(tick)
        } else {
            listenForNoise(false)
            dropFocus()
            showPlace()
            releaseWhenIdle()
        }
        showBar()
    }

    /** A seek; with the player let go of, a new one opens there, paused. */
    private fun seekTo(ms: Int, mode: Int) {
        atEnd = false
        val mp = player
        if (mp == null || !prepared) {
            resumeAt = ms
            if (mp == null) start()
            return
        }
        mp.seekTo(ms.toLong(), mode)
        releaseWhenIdle()
    }

    private fun skip(by: Int) {
        if (file == null || failed != null || duration < 0) return
        val mp = player
        val from = if (mp != null && prepared) position(mp) else resumeAt
        val to = (from + by).coerceIn(0, duration)
        seekTo(to, MediaPlayer.SEEK_CLOSEST)
        seek.progress = to
        elapsed.text = clock(to)
        showBar()
    }

    /** The bar's place and time, from the player. */
    private fun showPlace() {
        val mp = player
        val at = if (mp != null && prepared) position(mp) else resumeAt
        if (!dragging) seek.progress = at
        elapsed.text = clock(at)
    }

    private val tick = object : Runnable {
        override fun run() {
            if (!playing) return
            showPlace()
            postDelayed(this, TICK_MS)
        }
    }

    // ------------------------------------------------------------ the bar

    private val hideBar = Runnable { if (playing && hasVideo && !dragging) bar.visibility = GONE }

    /** Shows the bar; while a video plays it hides itself after a moment. */
    private fun showBar() {
        if (failed != null || file == null) return
        bar.visibility = VISIBLE
        removeCallbacks(hideBar)
        if (playing && hasVideo) postDelayed(hideBar, BAR_TIMEOUT_MS)
    }

    private fun tapped() {
        requestFocus()
        if (bar.visibility == VISIBLE && playing && hasVideo) {
            removeCallbacks(hideBar)
            bar.visibility = GONE
        } else {
            showBar()
        }
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { downX = e.x; downY = e.y }
            MotionEvent.ACTION_UP -> {
                val slop = ViewConfiguration.get(context).scaledTouchSlop
                if (abs(e.x - downX) < slop && abs(e.y - downY) < slop) performClick()
            }
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        tapped()
        return true
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        when (keyCode) {
            KeyEvent.KEYCODE_SPACE, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
            KeyEvent.KEYCODE_HEADSETHOOK -> if (event.repeatCount == 0) togglePlay()
            KeyEvent.KEYCODE_MEDIA_PLAY -> if (!playing) togglePlay()
            KeyEvent.KEYCODE_MEDIA_PAUSE -> pause()
            KeyEvent.KEYCODE_DPAD_LEFT -> skip(-SKIP_MS)
            KeyEvent.KEYCODE_DPAD_RIGHT -> skip(SKIP_MS)
            else -> return super.onKeyDown(keyCode, event)
        }
        return true
    }

    // ------------------------------------------------------------ being seen

    override fun onVisibilityChanged(changedView: View, visibility: Int) {
        super.onVisibilityChanged(changedView, visibility)
        if (ready) followVisibility()
    }

    override fun onWindowVisibilityChanged(visibility: Int) {
        super.onWindowVisibilityChanged(visibility)
        if (ready) followVisibility()
    }

    override fun onDetachedFromWindow() {
        releasePlayer(keepPlace = true)
        super.onDetachedFromWindow()
    }

    /** Out of sight, the player goes (its place kept); in sight, it comes back paused. */
    private fun followVisibility() {
        if (onScreen()) start() else releasePlayer(keepPlace = true)
    }

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, width: Int, height: Int) {
        texture = st
        val s = Surface(st)
        surface = s
        val mp = player
        if (mp == null) { start(); return }
        mp.setSurface(s)
        // A file that turned out to hold video once prepared: draw its frame.
        if (prepared && !playing) mp.seekTo(position(mp).toLong(), MediaPlayer.SEEK_CLOSEST)
    }

    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, width: Int, height: Int) {}

    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        if (st === texture) {
            player?.setSurface(null)
            surface?.release()
            surface = null
            texture = null
        }
        return true
    }

    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}

    // ------------------------------------------------------------ sound

    private fun attributes(): AudioAttributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(if (hasVideo) AudioAttributes.CONTENT_TYPE_MOVIE
                        else AudioAttributes.CONTENT_TYPE_MUSIC)
        .build()

    /**
     * Audio focus while playing, so music in another app pauses for this and
     * this pauses for a call or another player.
     */
    private fun takeFocus(): Boolean {
        val request = focus ?: AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(attributes())
            .setOnAudioFocusChangeListener { change ->
                if (change == AudioManager.AUDIOFOCUS_LOSS ||
                    change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) pause()
            }
            .build()
        focus = request
        return audio.requestAudioFocus(request) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
    }

    private fun dropFocus() {
        focus?.let { audio.abandonAudioFocusRequest(it) }
        focus = null
    }

    /** Headphones pulled out: pause rather than carry on through the speaker. */
    private val noisy = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) = pause()
    }

    private fun listenForNoise(on: Boolean) {
        if (on == noisyHeard) return
        noisyHeard = on
        if (on) {
            ContextCompat.registerReceiver(context, noisy,
                IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY),
                ContextCompat.RECEIVER_NOT_EXPORTED)
        } else {
            try { context.unregisterReceiver(noisy) } catch (e: IllegalArgumentException) {}
        }
    }

    /** `adb shell setprop log.tag.MiniCodeMedia DEBUG` turns the log on, in any build. */
    private fun log(text: () -> String) {
        if (Log.isLoggable(TAG, Log.DEBUG)) Log.d(TAG, text())
    }

    companion object {
        const val TAG = "MiniCodeMedia"

        /** Opened as video, and as audio; what the file holds decides once prepared. */
        val VIDEO_EXTENSIONS = setOf("mp4", "m4v", "mov", "3gp", "webm", "mkv")
        val AUDIO_EXTENSIONS = setOf("mp3", "wav", "m4a", "aac", "flac", "ogg", "opus")

        private const val BAR_COLOR = 0xE6252526.toInt()
        /** How far the bar's text sits above its bottom, for CurvedEdges. */
        private const val BAR_TEXT_DP = 12f
        private const val BAR_TIMEOUT_MS = 3000L
        /** Well inside NuPlayer's 10 seconds; see releaseWhenIdle. */
        private const val IDLE_RELEASE_MS = 4000L
        private const val TICK_MS = 250L
        private const val SKIP_MS = 5000

        /** m:ss, or h:mm:ss from an hour up; a length is rounded, a place cut. */
        fun clock(ms: Int, round: Boolean = false): String {
            val t = if (round) (ms + 500L) / 1000 else ms / 1000L
            val h = t / 3600
            val m = (t / 60) % 60
            val s = t % 60
            return if (h > 0) String.format(Locale.ROOT, "%d:%02d:%02d", h, m, s)
                   else String.format(Locale.ROOT, "%d:%02d", m, s)
        }
    }
}
