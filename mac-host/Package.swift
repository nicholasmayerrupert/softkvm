// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "SoftKVMHost",
    platforms: [.macOS(.v14)],
    targets: [
        // Thin Objective-C shim around Apple's private CGVirtualDisplay API.
        // Classes are looked up at runtime (NSClassFromString) so nothing links
        // against private symbols.
        .target(
            name: "CGVirtualDisplayPrivate",
            path: "Sources/CGVirtualDisplayPrivate",
            cSettings: [.unsafeFlags(["-fobjc-arc"])],
            linkerSettings: [.linkedFramework("CoreGraphics"), .linkedFramework("Foundation")]
        ),
        .executableTarget(
            name: "SoftKVMHost",
            dependencies: ["CGVirtualDisplayPrivate"],
            path: "Sources/SoftKVMHost",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("VideoToolbox"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("Network"),
                .linkedFramework("ServiceManagement"),
            ]
        ),
    ]
)
