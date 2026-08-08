export type WifiStatus = {
  available: boolean;
  connected: boolean;
  connected_ssid: string | null;
  detail: string;
};

export type WifiNetwork = {
  ssid: string;
  signal: number;
  security: string;
  secured: boolean;
  connected: boolean;
};

const readError = async (response: Response) => {
  try {
    const payload = await response.json();
    if (payload && typeof payload.detail === "string") return payload.detail;
  } catch {}
  return response.statusText || `HTTP ${response.status}`;
};

const requestJson = async <T>(url: string, options?: RequestInit): Promise<T> => {
  let response: Response;
  try {
    response = await fetch(url, { cache: "no-store", ...options });
  } catch (error) {
    throw new Error(`设备网络接口连接失败：${error instanceof Error ? error.message : "网络异常"}`);
  }
  if (!response.ok) throw new Error(await readError(response));
  return await response.json() as T;
};

export const getWifiStatus = () => requestJson<WifiStatus>("/api/v1/network/wifi/status");

export const listWifiNetworks = async (rescan = false) => {
  const response = await requestJson<{ networks: WifiNetwork[] }>(
    rescan ? "/api/v1/network/wifi/rescan" : "/api/v1/network/wifi/networks",
    rescan ? { method: "POST" } : undefined,
  );
  return response.networks;
};

export const connectWifi = (ssid: string, password: string | null) =>
  requestJson<{ connected: true; connected_ssid: string }>("/api/v1/network/wifi/connect", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ssid, password }),
  });
