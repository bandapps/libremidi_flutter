#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint libremidi_flutter.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'libremidi_flutter'
  s.version          = '0.8.5'
  s.summary          = 'Cross-platform MIDI device access for Flutter using libremidi.'
  s.description      = <<-DESC
A Flutter plugin providing MIDI device listing, connection, and communication
using the libremidi library via FFI.
                       DESC
  s.homepage         = 'https://github.com/bandapps/libremidi_flutter'
  s.license          = { :type => 'BSD-2-Clause', :file => '../LICENSE' }
  s.author           = { 'bandapps' => 'info@bandapps.de' }

  s.source           = { :path => '.' }

  # Source files. iOS and macOS share one CoreMIDI implementation in
  # apple/Classes (the same file SwiftPM compiles via its symlink); the C
  # header lives in src. Only the shared Apple .cpp is compiled here — the
  # src/*.cpp implementation is for the CMake-built platforms, not CocoaPods.
  s.source_files = [
    '../apple/Classes/*.cpp',
    '../src/*.h',
    '../third_party/libremidi/include/**/*.{hpp,h}'
  ]

  # No public headers - this is an FFI plugin (all symbols accessed via dart:ffi)

  # Include paths
  s.xcconfig = {
    'HEADER_SEARCH_PATHS' => [
      '"$(PODS_TARGET_SRCROOT)/../third_party/libremidi/include"',
      '"$(PODS_TARGET_SRCROOT)/../src"'
    ].join(' '),
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++20',
    'GCC_PREPROCESSOR_DEFINITIONS' => 'LIBREMIDI_HEADER_ONLY=1',
    # libremidi's CoreMIDI backend pulls in the (never-instantiated) coremidi_ump
    # backend, which references macOS 11 APIs; silence the dead-code availability
    # warnings to keep the 10.15 deployment target.
    'OTHER_CPLUSPLUSFLAGS' => '-std=c++20 -Wno-unguarded-availability-new'
  }

  # Link CoreMIDI frameworks
  s.frameworks = ['CoreMIDI', 'CoreFoundation', 'CoreAudio']

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.15'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
end
