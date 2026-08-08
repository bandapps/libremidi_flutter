// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "libremidi_flutter",
    platforms: [
        .iOS(.v13)
    ],
    products: [
        .library(name: "libremidi-flutter", targets: ["libremidi_flutter"])
    ],
    dependencies: [
        .package(name: "FlutterFramework", path: "../FlutterFramework")
    ],
    targets: [
        .target(
            name: "libremidi_flutter",
            dependencies: [
                .product(name: "FlutterFramework", package: "FlutterFramework")
            ],
            path: "Sources/libremidi_flutter",
            sources: ["libremidi_flutter.cpp"],
            publicHeadersPath: "include/libremidi_flutter",
            cxxSettings: [
                .define("LIBREMIDI_HEADER_ONLY", to: "1"),
                .define("LIBREMIDI_COREMIDI", to: "1"),
                .headerSearchPath("include/libremidi_flutter"),
                // Before libremidi_headers: src carries patched copies of
                // libremidi headers that must win over the vendored originals
                // (see src/libremidi/backends/coremidi).
                .headerSearchPath("patched_libremidi"),
                .headerSearchPath("libremidi_headers"),
                // libremidi's CoreMIDI backend pulls in the coremidi_ump
                // (MIDI 2.0) backend, which references macOS 11 / iOS 14 APIs.
                // That backend is never instantiated here (we only use the
                // MIDI 1 CoreMIDI API), so the unguarded-availability warnings
                // are about dead code — silence them to keep the iOS 13 /
                // macOS 10.15 deployment targets.
                .unsafeFlags(["-std=c++20", "-Wno-unguarded-availability-new"])
            ],
            linkerSettings: [
                .linkedFramework("CoreMIDI"),
                .linkedFramework("CoreFoundation"),
                .linkedFramework("CoreAudio")
            ]
        )
    ],
    cxxLanguageStandard: .cxx20
)
