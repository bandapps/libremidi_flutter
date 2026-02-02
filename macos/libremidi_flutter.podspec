#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint libremidi_flutter.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'libremidi_flutter'
  s.version          = '0.0.1'
  s.summary          = 'Cross-platform MIDI device access for Flutter using libremidi.'
  s.description      = <<-DESC
A Flutter plugin providing MIDI device listing, connection, and communication
using the libremidi library via FFI.
                       DESC
  s.homepage         = 'https://github.com/bandapps/libremidi_flutter'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'bandapps' => 'info@bandapps.de' }

  s.source           = { :path => '.' }

  # Source files
  s.source_files = [
    'Classes/**/*',
    '../src/*.{h,cpp}',
    '../third_party/libremidi/include/**/*.{hpp,h}'
  ]

  s.public_header_files = 'Classes/**/*.h'

  # Include paths
  s.xcconfig = {
    'HEADER_SEARCH_PATHS' => [
      '"$(PODS_TARGET_SRCROOT)/../third_party/libremidi/include"',
      '"$(PODS_TARGET_SRCROOT)/../src"'
    ].join(' '),
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++20',
    'GCC_PREPROCESSOR_DEFINITIONS' => 'LIBREMIDI_HEADER_ONLY=1',
    'OTHER_CPLUSPLUSFLAGS' => '-std=c++20'
  }

  # Link CoreMIDI frameworks
  s.frameworks = ['CoreMIDI', 'CoreFoundation', 'CoreAudio']

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.14'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
end