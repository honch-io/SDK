export type RelayPermissionRequestResult = {
  granted: boolean;
  requested: string[];
  denied: string[];
};

type RelayPermissionDeps = {
  platformOS?: string;
  permissions?: {
    PERMISSIONS?: Record<string, string | undefined>;
    RESULTS?: Record<string, string | undefined>;
    requestMultiple(permissions: string[]): Promise<Record<string, string>>;
  };
};

const ANDROID_BLE_PERMISSION_NAMES = [
  "BLUETOOTH_SCAN",
  "BLUETOOTH_CONNECT",
  "ACCESS_FINE_LOCATION"
] as const;

export async function requestRelayAndroidPermissions(
  deps: RelayPermissionDeps = {}
): Promise<RelayPermissionRequestResult> {
  const resolved = await resolvePermissionDeps(deps);
  if (resolved.platformOS !== "android") {
    return { granted: true, requested: [], denied: [] };
  }

  const requested = ANDROID_BLE_PERMISSION_NAMES.map(
    (name) => resolved.permissions.PERMISSIONS?.[name] ?? `android.permission.${name}`
  );
  const grantedValue = resolved.permissions.RESULTS?.GRANTED ?? "granted";
  const results = await resolved.permissions.requestMultiple(requested);
  const denied = requested.filter((permission) => results[permission] !== grantedValue);

  return {
    granted: denied.length === 0,
    requested,
    denied
  };
}

async function resolvePermissionDeps(deps: RelayPermissionDeps) {
  if (deps.platformOS !== undefined && deps.permissions !== undefined) {
    return {
      platformOS: deps.platformOS,
      permissions: deps.permissions
    };
  }

  const reactNative = require("react-native") as {
    Platform: { OS: string };
    PermissionsAndroid?: RelayPermissionDeps["permissions"];
  };
  if (reactNative.PermissionsAndroid === undefined) {
    throw new Error("React Native PermissionsAndroid is unavailable");
  }

  return {
    platformOS: deps.platformOS ?? reactNative.Platform.OS,
    permissions: deps.permissions ?? reactNative.PermissionsAndroid
  };
}
