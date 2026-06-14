// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "HonchSwiftRelay",
    platforms: [
        .iOS(.v15),
        .macOS(.v12)
    ],
    products: [
        .library(name: "HonchSwiftRelay", targets: ["HonchSwiftRelay"])
    ],
    targets: [
        .target(name: "HonchSwiftRelay"),
        .executableTarget(
            name: "HonchSwiftRelayContractTests",
            dependencies: ["HonchSwiftRelay"],
            path: "ContractTests"
        )
    ]
)
