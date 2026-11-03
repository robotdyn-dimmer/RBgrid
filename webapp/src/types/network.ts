/**
 * Network/WiFi runtime status — mirror of GET /wifi/status (FLAT, not BaseResponse;
 * confirmed by root 2026-06-20). Note the STA IP field is `sta_ip` (not `ip`).
 */
export interface WifiStatus {
  /** AP | STA | APSTA | NULL */
  mode: string;
  // STA fields
  ssid?: string;
  rssi?: number;
  connected?: boolean;
  sta_ip?: string;
  gateway?: string;
  netmask?: string;
  // AP fields
  ap_ssid?: string;
  connected_stations?: number;
  ap_ip?: string;
}

/** Body of POST /wifi/save (device reboots ~1s after). */
export interface WifiSaveRequest {
  ssid: string;
  password: string;
}
