import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// Guards against drift between the C header (the source of truth for the FFI
/// surface) and the ffigen-generated Dart bindings.
///
/// `lib/libremidi_flutter_bindings_generated.dart` is auto-generated via
/// `dart run ffigen --config ffigen.yaml`. This test fails if an exported
/// symbol in `src/libremidi_flutter.h` has no matching lookup in the bindings,
/// or vice versa — e.g. a new `lrm_*` export was added to the header but the
/// bindings were never regenerated (or a hand-edit added a binding for a symbol
/// that does not exist).
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

  test('every C export has a Dart binding and vice versa', () {
    final header = readFromRoot('src/libremidi_flutter.h');
    final bindings =
        readFromRoot('lib/libremidi_flutter_bindings_generated.dart');

    // Exported functions: FFI_PLUGIN_EXPORT <ret> lrm_name(...). [^;(] keeps the
    // match inside a single declaration (does not cross a ';' or '(').
    final exports = RegExp(r'FFI_PLUGIN_EXPORT\b[^;(]*?\b(lrm_\w+)\s*\(')
        .allMatches(header)
        .map((m) => m.group(1)!)
        .toSet();

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
}
