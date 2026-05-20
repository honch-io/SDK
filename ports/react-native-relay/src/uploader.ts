export type RelayUploadRequest = {
  endpointUrl: string;
  apiKey: string;
  body: Uint8Array;
  contentEncoding?: string;
};

export type RelayUploadResult =
  | { accepted: true }
  | { accepted: false; retryable: true; status?: number }
  | { accepted: false; retryable: false; status?: number };

export type RelayUploader = {
  upload(request: RelayUploadRequest): Promise<RelayUploadResult>;
};
