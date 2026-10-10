import 'dart:convert' show utf8;
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// Guards the headers under `src/libremidi/` against two kinds of drift.
///
/// That directory is on the include path ahead of `third_party`, and holds two
/// different kinds of file:
///
///  * **patched copies** of vendored libremidi headers, carrying local fixes.
///    The build-time `#error` in both C++ entry points catches the case where
///    the search order is wrong and the copy is bypassed. It does not catch the
///    opposite: libremidi gets updated, the vendored original changes, and the
///    copy stays behind. Everything compiles, the guard macro is still there,
///    and the stale copy quietly shadows a newer file — reverting whatever
///    upstream changed in it. `cmidi2.hpp` alone is over 3000 lines, so there
///    is a lot to revert silently.
///
///  * **files of our own** that have no upstream counterpart and only live here
///    because they must be reachable under `<libremidi/...>`. For those the
///    risk runs the other way: if upstream later adds a file with the same
///    path, ours starts shadowing it without a word.
///
/// `src/libremidi/upstream_baseline.txt` classifies every file in the
/// directory, so neither case can pass unnoticed, and a third one — a patch
/// copy at a misspelled path, which would never be included at all — fails too.
///
/// Fingerprints are FNV-1a over content with line endings normalised, so a CRLF
/// checkout on Windows and an LF one on macOS agree. Hand-rolled for the same
/// reason the rest of this project avoids Dart's hashCode: it has to mean the
/// same thing in another process and another SDK.
///
/// Pure Dart, no native code, no hardware. Runs from the package root.
void main() {
  const patchDir = 'src/libremidi';
  const upstreamDir = 'third_party/libremidi/include/libremidi';
  const baselineFile = 'src/libremidi/upstream_baseline.txt';
  const baselineName = 'upstream_baseline.txt';

  /// Marks a file as ours, with no upstream counterpart.
  const noUpstream = '-';

  String fingerprint(File file) {
    final normalised = file.readAsStringSync().replaceAll('\r\n', '\n');
    var hash = 0xcbf29ce484222325;
    for (final byte in utf8.encode(normalised)) {
      hash = ((hash ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF;
    }
    // Dart ints are signed, so the top bit would print as a minus sign.
    final hi = (hash >>> 32) & 0xFFFFFFFF;
    final lo = hash & 0xFFFFFFFF;
    return hi.toRadixString(16).padLeft(8, '0') +
        lo.toRadixString(16).padLeft(8, '0');
  }

  List<String> filesInPatchDir() {
    final dir = Directory(patchDir);
    if (!dir.existsSync()) {
      fail('Expected $patchDir (run from the package root)');
    }
    return dir
        .listSync(recursive: true)
        .whereType<File>()
        .map((f) =>
            f.path.replaceAll(r'\', '/').substring('$patchDir/'.length))
        .where((p) => p != baselineName)
        .toList()
      ..sort();
  }

  Map<String, String> readBaseline() {
    final file = File(baselineFile);
    if (!file.existsSync()) return {};
    final entries = <String, String>{};
    for (final line in file.readAsLinesSync()) {
      final trimmed = line.trim();
      if (trimmed.isEmpty || trimmed.startsWith('#')) continue;
      final parts = trimmed.split(RegExp(r'\s+'));
      if (parts.length != 2) {
        fail('Malformed line in $baselineFile: "$line"\n'
            'Expected "<fingerprint|$noUpstream>  <path under $patchDir>".');
      }
      entries[parts[1]] = parts[0];
    }
    return entries;
  }

  test('every file under $patchDir is classified in the baseline', () {
    final baseline = readBaseline();
    final files = filesInPatchDir();
    expect(files, isNotEmpty, reason: 'expected headers under $patchDir');

    final unclassified = files.where((f) => !baseline.containsKey(f)).toList();
    final stale = baseline.keys.where((k) => !files.contains(k)).toList();

    expect(
      unclassified,
      isEmpty,
      reason: 'Not listed in $baselineFile. Add a line per file: the '
          'fingerprint of the vendored original for a patched copy, or '
          '"$noUpstream" for a file of ours with no upstream counterpart. '
          'Run this test to be told the fingerprint.',
    );
    expect(
      stale,
      isEmpty,
      reason: 'Listed in $baselineFile but no longer present under $patchDir — '
          'remove the entries.',
    );
  });

  test('no patched header drifted from the upstream it was based on', () {
    final baseline = readBaseline();
    final problems = <String>[];

    for (final relative in filesInPatchDir()) {
      final recorded = baseline[relative];
      if (recorded == null) continue; // reported by the test above
      final upstream = File('$upstreamDir/$relative');

      if (recorded == noUpstream) {
        if (upstream.existsSync()) {
          problems.add('  $relative: recorded as ours, but upstream now ships '
              '$upstreamDir/$relative.\n'
              '      Our file silently shadows it. Decide which one wins; if '
              'ours stays, it has become a patched copy and needs the '
              'fingerprint ${fingerprint(upstream)}.');
        }
        continue;
      }

      if (!upstream.existsSync()) {
        problems.add('  $relative: shadows nothing — there is no '
            '$upstreamDir/$relative.\n'
            '      Either the path is misspelled, in which case the copy is '
            'never included, or upstream dropped the file and the copy should '
            'go too. If the file is ours, record "$noUpstream".');
        continue;
      }

      final actual = fingerprint(upstream);
      if (recorded != actual) {
        problems.add('  $relative: the vendored original changed\n'
            '      recorded $recorded, now $actual');
      }
    }

    if (problems.isNotEmpty) {
      fail('Headers under $patchDir are out of sync with $upstreamDir:\n'
          '${problems.join('\n')}\n\n'
          'For a changed original: the patch copy now shadows a newer file and '
          'silently reverts whatever upstream changed in it. Re-apply the local '
          'fix on top of the new version, check that the patch notes at the top '
          'of the copy still describe reality, verify, and only then record the '
          'new fingerprint in $baselineFile. Updating the fingerprint alone '
          'defeats the point of this test.');
    }
  });
}
