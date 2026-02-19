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
    dependencies: [],
    targets: [
        .target(
            name: "libremidi_flutter",
            dependencies: [],
            cxxSettings: [
                .define("LIBREMIDI_HEADER_ONLY", to: "1"),
                .define("LIBREMIDI_COREMIDI", to: "1"),
                .headerSearchPath("../../../../../third_party/libremidi/include"),
                .headerSearchPath("../../../../../src"),
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
