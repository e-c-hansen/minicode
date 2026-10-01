package org.minicode.editor

import android.content.Context
import android.provider.DocumentsContract
import androidx.documentfile.provider.DocumentFile
import java.io.File

/**
 * Moving a file or folder into another folder, for the file list's drag
 * and drop. A folder opened by path (Phone storage) moves with
 * `Files.move`; one from the picker asks its provider, through
 * `DocumentsContract.moveDocument`, and says so plainly when the provider
 * cannot move. Neither ever writes over something already there.
 */
object FileMove {

    /** Why a drop is refused. */
    enum class Refusal { SELF, INSIDE, ALREADY_THERE }

    /**
     * Why `source` (whose folder is `sourceParent`) cannot go into `target`,
     * or null when it can. Each is a key naming a place, as `keyOf` makes
     * them: a path for a folder opened by path, a document id for one from
     * the picker. Ids from the phone's own storage are paths too
     * ("primary:a/b"); other providers' ids are opaque, and for those only
     * "itself" and "already there" can be told, which is all a drop in the
     * list can reach anyway (the list never enters the folder being moved).
     */
    fun refusal(source: String, sourceParent: String, target: String): Refusal? = when {
        target == source -> Refusal.SELF
        target.startsWith("$source/") -> Refusal.INSIDE
        target == sourceParent -> Refusal.ALREADY_THERE
        else -> null
    }

    /** The key `refusal` compares: a path, or the provider's document id. */
    fun keyOf(file: DocumentFile): String {
        val uri = file.uri
        if (uri.scheme == "file") return uri.path ?: uri.toString()
        return try { DocumentsContract.getDocumentId(uri) } catch (e: Exception) { uri.toString() }
    }

    sealed class Result {
        /** Moved; `file` is the entry in its new folder. */
        class Moved(val file: DocumentFile) : Result()
        class Failed(val message: String) : Result()
    }

    /**
     * Moves `item`, which is in `from`, into `into`. `provider` names the
     * app a picked folder comes from, for the message when it cannot move.
     */
    fun move(context: Context, item: DocumentFile, from: DocumentFile, into: DocumentFile,
             provider: String): Result {
        val name = item.name ?: return Result.Failed("Could not move it: it has no name.")
        val dest = into.name ?: "that folder"
        if (item.uri.scheme == "file" && into.uri.scheme == "file") {
            val src = File(item.uri.path ?: return Result.Failed("Could not move $name."))
            val dir = File(into.uri.path ?: return Result.Failed("Could not move $name."))
            val target = File(dir, name)
            // exists() asks the file system, which on the phone's storage
            // ignores case, so "Notes.md" is refused beside "notes.md" too.
            if (target.exists()) return Result.Failed("$dest already has something called $name.")
            return try {
                // No REPLACE_EXISTING: Files.move refuses an existing target,
                // where File.renameTo (rename(2)) would replace a file.
                java.nio.file.Files.move(src.toPath(), target.toPath())
                Result.Moved(DocumentFile.fromFile(target))
            } catch (e: java.nio.file.FileAlreadyExistsException) {
                Result.Failed("$dest already has something called $name.")
            } catch (e: Exception) {
                Result.Failed("Could not move $name: ${e.message ?: e.javaClass.simpleName}")
            }
        }
        // A picked folder. A provider's names may or may not ignore case, so
        // any name that differs only in case is refused, to be safe.
        val clash = try {
            into.listFiles().firstOrNull { it.name?.equals(name, ignoreCase = true) == true }
        } catch (e: Exception) { null }
        if (clash != null) return Result.Failed("$dest already has something called ${clash.name}.")
        if (!supportsMove(context, item))
            return Result.Failed("$provider does not let apps move files. Move $name " +
                                 "in $provider itself, or keep the project on Phone storage.")
        val moved = try {
            DocumentsContract.moveDocument(context.contentResolver, item.uri, from.uri, into.uri)
        } catch (e: Exception) { null }
        if (moved == null) return Result.Failed("$provider would not move $name.")
        // Found again through the folder, so the result has a parent and a
        // place in the tree like every other listed entry.
        val found = into.findFile(name) ?: DocumentFile.fromSingleUri(context, moved)
        return if (found != null) Result.Moved(found) else Result.Failed("Moved $name, but cannot find it.")
    }

    private fun supportsMove(context: Context, item: DocumentFile): Boolean = try {
        context.contentResolver.query(
            item.uri, arrayOf(DocumentsContract.Document.COLUMN_FLAGS), null, null, null
        )?.use { c ->
            c.moveToFirst() && c.getInt(0) and DocumentsContract.Document.FLAG_SUPPORTS_MOVE != 0
        } ?: false
    } catch (e: Exception) { false }

    /**
     * The names leading from `item` down to `open`: empty when they are the
     * same entry, null when `open` is not inside `item`. Read before a move,
     * while the old entries can still be asked their names (a picked folder
     * on the phone's storage names its documents by path, so after the move
     * the old ones are gone).
     */
    fun trail(open: DocumentFile, item: DocumentFile): List<String>? {
        if (open.uri == item.uri) return emptyList()
        val o = open.uri.takeIf { it.scheme == "file" }?.path
        val i = item.uri.takeIf { it.scheme == "file" }?.path
        if (o != null && i != null) {
            // Compared resolved: a file opened from a terminal link may be
            // spelled /sdcard/... while the list says /storage/emulated/0/...
            val op = try { File(o).canonicalPath } catch (e: java.io.IOException) { o }
            val ip = try { File(i).canonicalPath } catch (e: java.io.IOException) { i }
            return when {
                op == ip -> emptyList()
                op.startsWith("$ip/") -> op.substring(ip.length + 1).split('/')
                else -> null
            }
        }
        if (o != null || i != null) return null   // one by path, one from the picker
        val chain = mutableListOf<DocumentFile>()
        var d: DocumentFile? = open
        while (d != null && d.uri != item.uri) { chain.add(0, d); d = d.parentFile }
        if (d == null) return null
        return chain.map { it.name ?: return null }
    }

    /** The entry `trail` names below `moved`, or null when one is missing. */
    fun follow(moved: DocumentFile, trail: List<String>): DocumentFile? {
        if (trail.isEmpty()) return moved
        moved.uri.takeIf { it.scheme == "file" }?.path?.let {
            return DocumentFile.fromFile(File(it, trail.joinToString("/")))
        }
        var f = moved
        for (name in trail) f = f.findFile(name) ?: return null
        return f
    }
}
