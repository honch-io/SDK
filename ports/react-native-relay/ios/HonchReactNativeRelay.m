#import "HonchReactNativeRelay.h"

@implementation HonchReactNativeRelay

RCT_EXPORT_MODULE()

RCT_EXPORT_METHOD(startScan:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  resolve(nil);
}

RCT_EXPORT_METHOD(stopScan:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  resolve(nil);
}

RCT_EXPORT_METHOD(connect:(NSString *)deviceId resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  resolve(deviceId);
}

RCT_EXPORT_METHOD(disconnect:(NSString *)deviceId resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  resolve(deviceId);
}

RCT_EXPORT_METHOD(acknowledgeMessage:(NSString *)deviceId sequence:(NSString *)sequence resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  resolve([NSString stringWithFormat:@"%@:%@", deviceId, sequence]);
}

RCT_EXPORT_METHOD(scheduleUpload:(double)delayMs resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  resolve(nil);
}

RCT_EXPORT_METHOD(cancelUpload:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  resolve(nil);
}

@end
