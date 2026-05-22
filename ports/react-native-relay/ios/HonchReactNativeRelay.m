#import "HonchReactNativeRelay.h"

static NSString * const HonchRelayServiceUUIDString = @"484f4e43-482d-5245-4c41-592d53445631";
static NSString * const HonchRelayFrameCharacteristicUUIDString = @"484f4e43-482d-5245-4c41-592d4652414d";
static NSString * const HonchRelayAckCharacteristicUUIDString = @"484f4e43-482d-5245-4c41-592d41434b31";
static NSString * const HonchRelayFrameEventName = @"HonchRelayFrame";

@interface HonchReactNativeRelay ()
{
  CBCentralManager *_centralManager;
  NSMutableDictionary<NSString *, CBPeripheral *> *_peripheralsById;
  NSMutableDictionary<NSString *, CBCharacteristic *> *_ackCharacteristicsById;
  BOOL _hasListeners;
}
@end

@implementation HonchReactNativeRelay

RCT_EXPORT_MODULE()

- (instancetype)init
{
  if ((self = [super init])) {
    _peripheralsById = [NSMutableDictionary new];
    _ackCharacteristicsById = [NSMutableDictionary new];
  }
  return self;
}

+ (BOOL)requiresMainQueueSetup
{
  return NO;
}

- (NSArray<NSString *> *)supportedEvents
{
  return @[HonchRelayFrameEventName];
}

- (void)startObserving
{
  _hasListeners = YES;
}

- (void)stopObserving
{
  _hasListeners = NO;
}

RCT_EXPORT_METHOD(startScan:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  [self ensureCentralManager];
  if (_centralManager.state != CBManagerStatePoweredOn) {
    reject(@"bluetooth_unavailable", @"Bluetooth is not powered on", nil);
    return;
  }

  CBUUID *serviceUUID = [CBUUID UUIDWithString:HonchRelayServiceUUIDString];
  [_centralManager scanForPeripheralsWithServices:@[serviceUUID] options:nil];
  resolve(nil);
}

RCT_EXPORT_METHOD(stopScan:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  [self ensureCentralManager];
  [_centralManager stopScan];
  resolve(nil);
}

RCT_EXPORT_METHOD(connect:(NSString *)deviceId resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  [self ensureCentralManager];
  CBPeripheral *peripheral = _peripheralsById[deviceId];
  if (peripheral == nil) {
    reject(@"unknown_peripheral", @"No discovered relay peripheral matches the device id", nil);
    return;
  }

  peripheral.delegate = self;
  [_centralManager connectPeripheral:peripheral options:nil];
  resolve(deviceId);
}

RCT_EXPORT_METHOD(disconnect:(NSString *)deviceId resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  [self ensureCentralManager];
  CBPeripheral *peripheral = _peripheralsById[deviceId];
  if (peripheral != nil) {
    [_centralManager cancelPeripheralConnection:peripheral];
  }
  resolve(deviceId);
}

RCT_EXPORT_METHOD(subscribeFrames:(NSString *)deviceId resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  CBPeripheral *peripheral = _peripheralsById[deviceId];
  if (peripheral == nil) {
    reject(@"unknown_peripheral", @"No discovered relay peripheral matches the device id", nil);
    return;
  }

  CBUUID *serviceUUID = [CBUUID UUIDWithString:HonchRelayServiceUUIDString];
  [peripheral discoverServices:@[serviceUUID]];
  resolve(deviceId);
}

RCT_EXPORT_METHOD(acknowledgeMessage:(NSString *)deviceId sequence:(NSString *)sequence resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  CBPeripheral *peripheral = _peripheralsById[deviceId];
  CBCharacteristic *ackCharacteristic = _ackCharacteristicsById[deviceId];
  if (peripheral == nil || ackCharacteristic == nil) {
    reject(@"ack_unavailable", @"ACK characteristic is not available for the relay peripheral", nil);
    return;
  }

  NSData *ackData = [self ackPayloadForSequence:sequence];
  [peripheral writeValue:ackData forCharacteristic:ackCharacteristic type:CBCharacteristicWriteWithResponse];
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

- (void)ensureCentralManager
{
  if (_centralManager == nil) {
    _centralManager = [[CBCentralManager alloc] initWithDelegate:self queue:nil];
  }
}

- (NSData *)ackPayloadForSequence:(NSString *)sequence
{
  unsigned long long value = strtoull(sequence.UTF8String, NULL, 10);
  uint8_t bytes[9];
  bytes[0] = 1;
  for (NSInteger index = 8; index >= 1; index -= 1) {
    bytes[index] = (uint8_t)(value & 0xff);
    value >>= 8;
  }
  return [NSData dataWithBytes:bytes length:sizeof(bytes)];
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
}

- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData
                  RSSI:(NSNumber *)RSSI
{
  NSString *deviceId = peripheral.identifier.UUIDString;
  _peripheralsById[deviceId] = peripheral;
  peripheral.delegate = self;
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
  CBUUID *serviceUUID = [CBUUID UUIDWithString:HonchRelayServiceUUIDString];
  [peripheral discoverServices:@[serviceUUID]];
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
  if (error != nil) {
    return;
  }

  CBUUID *serviceUUID = [CBUUID UUIDWithString:HonchRelayServiceUUIDString];
  CBUUID *frameUUID = [CBUUID UUIDWithString:HonchRelayFrameCharacteristicUUIDString];
  CBUUID *ackUUID = [CBUUID UUIDWithString:HonchRelayAckCharacteristicUUIDString];
  for (CBService *service in peripheral.services) {
    if ([service.UUID isEqual:serviceUUID]) {
      [peripheral discoverCharacteristics:@[frameUUID, ackUUID] forService:service];
    }
  }
}

- (void)peripheral:(CBPeripheral *)peripheral
didDiscoverCharacteristicsForService:(CBService *)service
             error:(NSError *)error
{
  if (error != nil) {
    return;
  }

  CBUUID *frameUUID = [CBUUID UUIDWithString:HonchRelayFrameCharacteristicUUIDString];
  CBUUID *ackUUID = [CBUUID UUIDWithString:HonchRelayAckCharacteristicUUIDString];
  NSString *deviceId = peripheral.identifier.UUIDString;
  for (CBCharacteristic *characteristic in service.characteristics) {
    if ([characteristic.UUID isEqual:frameUUID]) {
      [peripheral setNotifyValue:YES forCharacteristic:characteristic];
    } else if ([characteristic.UUID isEqual:ackUUID]) {
      _ackCharacteristicsById[deviceId] = characteristic;
    }
  }
}

- (void)peripheral:(CBPeripheral *)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic
             error:(NSError *)error
{
  if (error != nil || characteristic.value == nil) {
    return;
  }

  CBUUID *frameUUID = [CBUUID UUIDWithString:HonchRelayFrameCharacteristicUUIDString];
  if (![characteristic.UUID isEqual:frameUUID]) {
    return;
  }

  if (_hasListeners) {
    [self sendEventWithName:HonchRelayFrameEventName body:@{
      @"deviceId": peripheral.identifier.UUIDString,
      @"frameBase64": [characteristic.value base64EncodedStringWithOptions:0]
    }];
  }
}

@end
