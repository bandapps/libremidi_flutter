// CocoaPods compilation unit for macOS.
//
// The actual implementation is shared with iOS and lives in
// apple/Classes/libremidi_flutter.cpp. CocoaPods silently drops `source_files`
// patterns that resolve outside the pod root (the podspec's directory), so
// pointing `source_files` at ../apple/Classes/*.cpp produced a pod with zero
// compiled sources — it built fine and then failed at runtime with
// "Failed to lookup symbol 'lrm_...'". This forwarder lives inside the pod
// root so CocoaPods picks it up; the #include is resolved by the preprocessor,
// which has no such restriction.
//
// SwiftPM does not use this file — it compiles apple/Classes/libremidi_flutter.cpp
// directly through the symlink in Sources/libremidi_flutter/.
#include "../../apple/Classes/libremidi_flutter.cpp"
