// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "libremidi_flutter",
    platforms: [
        .macOS(.v10_14)
    ],
    products: [
        .library(name: "libremidi-flutter", targets: ["libremidi_flutter"])
    ],
    dependencies: [],
    targets: [
        .target(
            name: "libremidi_flutter",
            dependencies: [],
            sources: ["libremidi_flutter.cpp"],
            cxxSettings: [
                .define("LIBREMIDI_HEADER_ONLY", to: "1"),
                .define("LIBREMIDI_COREMIDI", to: "1"),
                .headerSearchPath("include/libremidi_flutter"),
                .headerSearchPath("libremidi_headers"),
                .unsafeFlags(["-std=c++20"])
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
