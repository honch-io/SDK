module.exports = {
  dependency: {
    platforms: {
      android: {
        sourceDir: "./android",
        packageImportPath: "import io.honch.reactnativerelay.HonchReactNativeRelayPackage;",
        packageInstance: "new HonchReactNativeRelayPackage()"
      },
      ios: {
        podspecPath: "./ios/HonchReactNativeRelay.podspec"
      }
    }
  }
};
