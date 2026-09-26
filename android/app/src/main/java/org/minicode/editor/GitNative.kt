package org.minicode.editor

/**
 * The Source Control panel's model, in app/src/main/cpp/git_jni.cpp: the
 * core's GitStatus and GitGraph, and the GTK port's GitModel, which already
 * decide everything the panel shows. Nothing here runs git; GitPanel does,
 * through GitRunner, and hands the bytes over.
 *
 * Argument vectors are byte arrays so paths go back to git exactly as git
 * printed them. Snapshot and graph handles are freed by whoever made them.
 */
object GitNative {
    init { System.loadLibrary("minicode") }

    /** NAME=value for every variable git runs with (GIT_OPTIONAL_LOCKS=0 ...). */
    @JvmStatic external fun environment(): Array<String>

    @JvmStatic external fun topLevelArgs(): Array<ByteArray>
    @JvmStatic external fun statusArgs(): Array<ByteArray>
    @JvmStatic external fun refArgs(): Array<ByteArray>
    @JvmStatic external fun logArgs(limit: Int, all: Boolean, compare: Boolean): Array<ByteArray>
    @JvmStatic external fun leftRightArgs(): Array<ByteArray>
    @JvmStatic external fun commitArgs(message: String): Array<ByteArray>
    @JvmStatic external fun showArgs(hash: String): Array<ByteArray>

    @JvmStatic external fun failureText(status: Int, out: ByteArray, err: ByteArray): String
    @JvmStatic external fun firstLine(out: ByteArray): String
    /** {top level, .git directory}, or null. */
    @JvmStatic external fun parseTopLevel(out: ByteArray): Array<String>?

    @JvmStatic external fun snapshotEmpty(notice: String?, error: String?): Long
    @JvmStatic external fun snapshotOf(top: String, gitDir: String, status: ByteArray?,
                                       error: String?): Long
    @JvmStatic external fun snapshotFree(h: Long)
    /** {branch line, notice, error, summary line, top level}. */
    @JvmStatic external fun snapshotText(h: Long): Array<String>
    /** {in repository, no commit yet, graph wanted, compare, ahead, behind}. */
    @JvmStatic external fun snapshotFlags(h: Long): IntArray
    @JvmStatic external fun snapshotRows(h: Long): Array<Any>
    @JvmStatic external fun rowDescriptions(h: Long): Array<String>
    @JvmStatic external fun nextFileRow(h: Long, from: Int, step: Int): Int
    @JvmStatic external fun lastFileRow(h: Long): Int
    @JvmStatic external fun pickAfterRefresh(old: Long, sel: Int, now: Long): Int
    @JvmStatic external fun stageArgs(h: Long, row: Int): Array<ByteArray>?
    @JvmStatic external fun diffArgs(h: Long, row: Int): Array<ByteArray>?

    @JvmStatic external fun graphSameKey(previous: Long, snap: Long, limit: Int, all: Boolean,
                                         refs: ByteArray): Boolean
    @JvmStatic external fun graphBuild(snap: Long, limit: Int, all: Boolean, refs: ByteArray,
                                       log: ByteArray?, error: String?): Long
    @JvmStatic external fun graphSetLeftRight(h: Long, out: ByteArray)
    @JvmStatic external fun graphFree(h: Long)
    @JvmStatic external fun graphError(h: Long): String
    @JvmStatic external fun graphRows(h: Long): Array<Any>
    @JvmStatic external fun graphToolTip(h: Long, row: Int, now: Long): String
    @JvmStatic external fun graphDescriptions(h: Long): Array<String>
    /** {hash, "a1b2c3d Subject"}, or null past the last commit. */
    @JvmStatic external fun graphCommit(h: Long, row: Int): Array<String>?

    /** {text, int[] of (start, length, style)} for a diff or `git show`. */
    @JvmStatic external fun styledText(bytes: ByteArray, commit: Boolean, now: Long): Array<Any>

    @JvmStatic external fun laneColor(i: Int): Int
    @JvmStatic external fun pillColor(kind: Int): Int
    @JvmStatic external fun laneWidth(rowWidth: Double, lanes: Int): Double

    // Styles, in GitUi::Style's order.
    const val PLAIN = 0
    const val ADDED = 1
    const val REMOVED = 2
    const val MUTED = 3
    const val FILE_HEADER = 4
    const val HASH = 5
    const val BOLD = 6

    /** How many commits the graph loads at a time (GitUi::kGraphBatch). */
    const val GRAPH_BATCH = 200
}
