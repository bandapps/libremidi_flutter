# Applied to every app that depends on this plugin.
#
# The classes in dev.celtera.libremidi are only ever reached from native code:
# constructors and invalidate() are looked up by name via JNI, and the native
# methods are bound by name. R8 sees no Java callers and strips or renames
# them in release builds, which breaks Android MIDI at runtime.
-keep class dev.celtera.libremidi.** { *; }
