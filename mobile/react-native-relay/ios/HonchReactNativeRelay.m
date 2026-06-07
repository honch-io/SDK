#import "HonchReactNativeRelay.h"

static NSString * const HonchRelayServiceUUIDString = @"484f4e43-482d-5245-4c41-592d53445631";
static NSString * const HonchRelayFrameCharacteristicUUIDString = @"484f4e43-482d-5245-4c41-592d4652414d";
static NSString * const HonchRelayAckCharacteristicUUIDString = @"484f4e43-482d-5245-4c41-592d41434b31";
static NSString * const HonchRelayFrameEventName = @"HonchRelayFrame";
static NSTimeInterval const HonchRelayDefaultScanTimeoutSeconds = 30.0;
static void *HonchRelayBLEQueueKey = &HonchRelayBLEQueueKey;

@interface HonchReactNativeRelay ()
{
  CBCentralManager *_centralManager;
  dispatch_queue_t _bleQueue;
  NSMutableDictionary<NSString *, CBPeripheral *> *_peripheralsById;
  NSMutableDictionary<NSString *, NSDictionary *> *_discoveredDevicesById;
  NSMutableDictionary<NSString *, CBCharacteristic *> *_ackCharacteristicsById;
  RCTPromiseResolveBlock _pendingScanResolve;
  RCTPromiseRejectBlock _pendingScanReject;
  NSUInteger _scanGeneration;
  BOOL _hasListeners;
  BOOL _didTeardown;
}
@end

@implementation HonchReactNativeRelay

RCT_EXPORT_MODULE()

- (instancetype)init
{
  if ((self = [super init])) {
    _bleQueue = dispatch_queue_create("io.honch.relay.ble", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(_bleQueue, HonchRelayBLEQueueKey, HonchRelayBLEQueueKey, NULL);
    _peripheralsById = [NSMutableDictionary new];
    _discoveredDevicesById = [NSMutableDictionary new];
    _ackCharacteristicsById = [NSMutableDictionary new];
  }
  return self;
}

+ (BOOL)requiresMainQueueSetup
{
  return NO;
}

- (dispatch_queue_t)methodQueue
{
  return _bleQueue;
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
  if (_centralManager.state == CBManagerStateUnknown || _centralManager.state == CBManagerStateResetting) {
    _pendingScanResolve = resolve;
    _pendingScanReject = reject;
    return;
  }
  if (_centralManager.state != CBManagerStatePoweredOn) {
    reject(@"bluetooth_unavailable", @"Bluetooth is not powered on", nil);
    return;
  }

  [self beginScan];
  resolve(nil);
}

- (void)beginScan
{
  CBUUID *serviceUUID = [CBUUID UUIDWithString:HonchRelayServiceUUIDString];
  [_centralManager scanForPeripheralsWithServices:@[serviceUUID] options:nil];
  [self scheduleScanTimeout];
}

RCT_EXPORT_METHOD(stopScan:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  [self ensureCentralManager];
  [_centralManager stopScan];
  _scanGeneration += 1;
  resolve(nil);
}

RCT_EXPORT_METHOD(discoveredDevices:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  resolve(_discoveredDevicesById.allValues);
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
  reject(
    @"ios_background_upload_unsupported",
    @"iOS relay upload scheduling is foreground-only; call drainUploads from the host app foreground lifecycle.",
    nil
  );
}

RCT_EXPORT_METHOD(cancelUpload:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject)
{
  resolve(nil);
}

- (void)invalidate
{
  [self teardown];
  [super invalidate];
}

- (void)dealloc
{
  [self teardown];
}

- (void)ensureCentralManager
{
  if (_centralManager == nil) {
    _centralManager = [[CBCentralManager alloc] initWithDelegate:self queue:_bleQueue];
  }
}

- (void)performOnBLEQueueSync:(dispatch_block_t)block
{
  if (dispatch_get_specific(HonchRelayBLEQueueKey) == HonchRelayBLEQueueKey) {
    block();
    return;
  }
  dispatch_sync(_bleQueue, block);
}

- (void)teardown
{
  [self performOnBLEQueueSync:^{
    if (self->_didTeardown) {
      return;
    }
    self->_didTeardown = YES;
    self->_hasListeners = NO;
    self->_scanGeneration += 1;
    self->_pendingScanResolve = nil;
    self->_pendingScanReject = nil;

    if (self->_centralManager != nil) {
      [self->_centralManager stopScan];
      for (CBPeripheral *peripheral in self->_peripheralsById.allValues) {
        peripheral.delegate = nil;
        [self->_centralManager cancelPeripheralConnection:peripheral];
      }
      self->_centralManager.delegate = nil;
      self->_centralManager = nil;
    } else {
      for (CBPeripheral *peripheral in self->_peripheralsById.allValues) {
        peripheral.delegate = nil;
      }
    }

    [self->_peripheralsById removeAllObjects];
    [self->_discoveredDevicesById removeAllObjects];
    [self->_ackCharacteristicsById removeAllObjects];
  }];
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
  if (_pendingScanResolve == nil) {
    return;
  }
  RCTPromiseResolveBlock resolve = _pendingScanResolve;
  RCTPromiseRejectBlock reject = _pendingScanReject;
  _pendingScanResolve = nil;
  _pendingScanReject = nil;

  if (central.state == CBManagerStatePoweredOn) {
    [self beginScan];
    resolve(nil);
  } else if (central.state != CBManagerStateUnknown && central.state != CBManagerStateResetting) {
    reject(@"bluetooth_unavailable", @"Bluetooth is not powered on", nil);
  }
}

- (void)scheduleScanTimeout
{
  _scanGeneration += 1;
  NSUInteger generation = _scanGeneration;
  dispatch_after(
    dispatch_time(DISPATCH_TIME_NOW, (int64_t)(HonchRelayDefaultScanTimeoutSeconds * NSEC_PER_SEC)),
    _bleQueue,
    ^{
      if (self->_scanGeneration == generation && self->_centralManager != nil) {
        [self->_centralManager stopScan];
      }
    }
  );
}

- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData
                  RSSI:(NSNumber *)RSSI
{
  NSString *deviceId = peripheral.identifier.UUIDString;
  _peripheralsById[deviceId] = peripheral;
  NSMutableDictionary *device = [@{
    @"id": deviceId,
    @"rssi": RSSI
  } mutableCopy];
  if (peripheral.name != nil) {
    device[@"name"] = peripheral.name;
  }
  _discoveredDevicesById[deviceId] = device;
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
