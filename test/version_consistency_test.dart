import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// Pins the package version across every place it is hard-coded:
///   - pubspec.yaml            `version:`
///   - src/libremidi_flutter.cpp           `#define LRM_VERSION "x.y.z"`  (Win/Linux/Android)
///   - apple/Classes/libremidi_flutter.cpp `lrm_get_version()` return literal (iOS + macOS)
///
/// Regression: when bumping the package version (e.g. 0.8.4 -> 0.8.5) it is
/// easy to update pubspec but forget one of the native copies — the Apple file
/// even hard-codes the literal instead of reusing LRM_VERSION. That ships a
/// wrong `lrm_get_version()` on the forgotten platform. This test goes red the
/// moment the three drift apart.
///
/// Pure Dart: reads the source files directly, no native FFI. Runs from the
/// package root (where `flutter test` executes).
void main() {
  String readFromRoot(String path) {
    final file = File(path);
    if (!file.existsSync()) {
      fail('Expected file not found (run from package root): $path');
    }
    return file.readAsStringSync();
  }

  test('package version is consistent across pubspec and native sources', () {
    // pubspec.yaml — semver only, ignoring any +build suffix.
    final pubspecVersion = RegExp(r'^version:\s*([0-9]+\.[0-9]+\.[0-9]+)',
            multiLine: true)
        .firstMatch(readFromRoot('pubspec.yaml'))
        ?.group(1);

    // src copy (Windows/Linux/Android): #define LRM_VERSION "x.y.z"
    final srcVersion =
        RegExp(r'#define\s+LRM_VERSION\s+"([^"]+)"')
            .firstMatch(readFromRoot('src/libremidi_flutter.cpp'))
            ?.group(1);

    // Apple copy (iOS + macOS): lrm_get_version() { return "x.y.z"; }
    final appleVersion =
        RegExp(r'lrm_get_version[^}]*?return\s*"([^"]+)"')
            .firstMatch(readFromRoot('apple/Classes/libremidi_flutter.cpp'))
            ?.group(1);

    // CocoaPods podspecs (iOS + macOS): s.version = 'x.y.z'
    final podspecVersionRe = RegExp(r"s\.version\s*=\s*'([^']+)'");
    final iosPodspecVersion = podspecVersionRe
        .firstMatch(readFromRoot('ios/libremidi_flutter.podspec'))
        ?.group(1);
    final macosPodspecVersion = podspecVersionRe
        .firstMatch(readFromRoot('macos/libremidi_flutter.podspec'))
        ?.group(1);

    // README install snippet: libremidi_flutter: ^x.y.z
    final readmeVersion = RegExp(
            r'libremidi_flutter:\s*\^([0-9]+\.[0-9]+\.[0-9]+)')
        .firstMatch(readFromRoot('README.md'))
        ?.group(1);

    expect(pubspecVersion, isNotNull,
        reason: 'could not parse version from pubspec.yaml');
    expect(srcVersion, isNotNull,
        reason: 'could not parse LRM_VERSION from src/libremidi_flutter.cpp');
    expect(appleVersion, isNotNull,
        reason: 'could not parse lrm_get_version() from '
            'apple/Classes/libremidi_flutter.cpp');

    expect(srcVersion, pubspecVersion,
        reason: 'version drift: src/libremidi_flutter.cpp LRM_VERSION '
            '($srcVersion) != pubspec ($pubspecVersion) — '
            'update LRM_VERSION when bumping the version.');
    expect(appleVersion, pubspecVersion,
        reason: 'version drift: apple/Classes/libremidi_flutter.cpp '
            'lrm_get_version() ($appleVersion) != pubspec ($pubspecVersion) — '
            'update the Apple return literal when bumping the version.');
    expect(iosPodspecVersion, pubspecVersion,
        reason: 'version drift: ios/libremidi_flutter.podspec '
            '($iosPodspecVersion) != pubspec ($pubspecVersion).');
    expect(macosPodspecVersion, pubspecVersion,
        reason: 'version drift: macos/libremidi_flutter.podspec '
            '($macosPodspecVersion) != pubspec ($pubspecVersion).');
    expect(readmeVersion, pubspecVersion,
        reason: 'version drift: README install snippet '
            '(^$readmeVersion) != pubspec ($pubspecVersion) — '
            'update the `dependencies:` example when bumping the version.');
  });
}
