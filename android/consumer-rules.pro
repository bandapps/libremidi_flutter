# Applied to every app that depends on this plugin.
#
# MidiBridge is reached from native code: JNI_OnLoad looks the class and its
# static methods up by name and binds its native method with RegisterNatives.
# R8 sees no Java callers for most of it and would strip or rename it in
# release builds, which breaks Android MIDI at runtime.
-keep class dev.celtera.libremidi.** { *; }
