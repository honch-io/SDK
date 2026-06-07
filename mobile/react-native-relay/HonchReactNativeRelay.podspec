require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "HonchReactNativeRelay"
  s.version      = package["version"]
  s.summary      = "React Native relay for Honch BLE gateway uploads"
  s.license      = "Apache-2.0"
  s.authors      = { "Honch" => "dev@honch.io" }
  s.homepage     = "https://honch.io"
  s.platforms    = { :ios => "13.0" }
  s.source       = { :git => "https://example.invalid/honch-sdk.git", :tag => "#{s.version}" }
  s.source_files = "ios/**/*.{h,m,mm,swift}"
  s.frameworks   = "CoreBluetooth"
  s.dependency "React-Core"
end
