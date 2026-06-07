import { AppRegistry } from "react-native";

import App from "./App";
import { relay } from "./relay";

AppRegistry.registerComponent("HonchRelayExample", () => App);

AppRegistry.registerHeadlessTask("HonchRelayUpload", () => async () => {
  await relay.drainUploads();
});
