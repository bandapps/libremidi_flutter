import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// Guards against drift between the C header (the source of truth for the FFI
/// surface), the ffigen-generated Dart bindings, and the two native
/// implementations.
///
/// `lib/libremidi_flutter_bindings_generated.dart` is auto-generated via
/// `dart run ffigen --config ffigen.yaml`. This test fails if an exported
/// symbol in `src/libremidi_flutter.h` has no matching lookup in the bindings,
/// or vice versa — e.g. a new `lrm_*` export was added to the header but the
/// bindings were never regenerated (or a hand-edit added a binding for a symbol
/// that does not exist).
///
/// It also fails if an export is missing from either native implementation.
/// The package carries two of them (`src` for Windows/Linux/Android,
/// `apple/Classes` for iOS+macOS); adding an export to the header, the bindings
/// and only one .cpp compiles and links everywhere, and then fails at runtime
/// on the forgotten platform group when Dart looks the symbol up.
///
/// Known limitation: this compares names only, not signatures.
///
/// Pure Dart, no native FFI. Runs from the package root (where `flutter test`
/// executes).
void main() {
  String readFromRoot(String path) {
    final file = File(path);
    if (!file.existsSync()) {
      fail('Expected file not found (run from package root): $path');
    }
    return file.readAsStringSync();
  }

  // Exported functions: FFI_PLUGIN_EXPORT <ret> lrm_name(...). [^;(] keeps the
  // match inside a single declaration (does not cross a ';' or '(').
  final exportPattern = RegExp(r'FFI_PLUGIN_EXPORT\b[^;(]*?\b(lrm_\w+)\s*\(');

  Set<String> exportsIn(String source) =>
      exportPattern.allMatches(source).map((m) => m.group(1)!).toSet();

  test('every C export has a Dart binding and vice versa', () {
    final header = readFromRoot('src/libremidi_flutter.h');
    final bindings =
        readFromRoot('lib/libremidi_flutter_bindings_generated.dart');

    final exports = exportsIn(header);

    // Bound symbols: every 'lrm_*' string literal looked up in the bindings.
    final boundSymbols = RegExp(r"'(lrm_\w+)'")
        .allMatches(bindings)
        .map((m) => m.group(1)!)
        .toSet();

    expect(exports, isNotEmpty,
        reason: 'parsed no FFI_PLUGIN_EXPORT lrm_* functions from the header');
    expect(boundSymbols, isNotEmpty,
        reason: 'parsed no lrm_* symbol lookups from the bindings');

    final missingBindings = exports.difference(boundSymbols);
    final orphanBindings = boundSymbols.difference(exports);

    expect(missingBindings, isEmpty,
        reason: 'header exports without a Dart binding — regenerate with '
            '`dart run ffigen --config ffigen.yaml`: $missingBindings');
    expect(orphanBindings, isEmpty,
        reason: 'Dart bindings with no matching header export (stale binding '
            'or hand-edit?): $orphanBindings');
  });

  test('every C export is implemented in both native copies', () {
    final exports = exportsIn(readFromRoot('src/libremidi_flutter.h'));
    expect(exports, isNotEmpty,
        reason: 'parsed no FFI_PLUGIN_EXPORT lrm_* functions from the header');

    // Regression: 0.8.6 shipped after an export had been added to the header,
    // the bindings and src/ only. iOS and macOS compile apple/Classes instead,
    // so they linked fine and then failed at runtime on the first lookup.
    final implementations = {
      'src/libremidi_flutter.cpp': exportsIn(
        readFromRoot('src/libremidi_flutter.cpp'),
      ),
      'apple/Classes/libremidi_flutter.cpp': exportsIn(
        readFromRoot('apple/Classes/libremidi_flutter.cpp'),
      ),
    };

    for (final entry in implementations.entries) {
      expect(entry.value, isNotEmpty,
          reason: 'parsed no exported definitions from ${entry.key}');
      expect(exports.difference(entry.value), isEmpty,
          reason: 'header exports missing from ${entry.key} — the platforms '
              'built from this file would fail at runtime with '
              '"Failed to lookup symbol"');
      expect(entry.value.difference(exports), isEmpty,
          reason: '${entry.key} exports symbols the header does not declare');
    }
  });
}
