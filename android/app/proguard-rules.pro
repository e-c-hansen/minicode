# JNI looks up MiniCode's native methods, and the classes they return, by
# name, so nothing of MiniCode's own is renamed or removed.
-keep class org.minicode.editor.** { *; }
