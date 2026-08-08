"use client";

import { useCallback, useEffect, useMemo, useState } from "react";

import { connectWifi, getWifiStatus, listWifiNetworks, type WifiNetwork, type WifiStatus } from "./wifiApi";

export default function WifiControl() {
  const [status, setStatus] = useState<WifiStatus | null>(null);
  const [open, setOpen] = useState(false);
  const [networks, setNetworks] = useState<WifiNetwork[]>([]);
  const [selectedSsid, setSelectedSsid] = useState<string | null>(null);
  const [password, setPassword] = useState("");
  const [loading, setLoading] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const refreshStatus = useCallback(async () => {
    try {
      const next = await getWifiStatus();
      setStatus(next);
    } catch (reason) {
      setStatus({ available: false, connected: false, connected_ssid: null, detail: reason instanceof Error ? reason.message : "Wi-Fi状态读取失败" });
    }
  }, []);

  useEffect(() => { void refreshStatus(); }, [refreshStatus]);

  const scan = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      setNetworks(await listWifiNetworks(true));
      await refreshStatus();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Wi-Fi扫描失败");
    } finally {
      setLoading(false);
    }
  }, [refreshStatus]);

  useEffect(() => {
    if (!open) return;
    void scan();
  }, [open, scan]);

  const selected = useMemo(
    () => networks.find((network) => network.ssid === selectedSsid) ?? null,
    [networks, selectedSsid],
  );

  const close = () => {
    if (connecting) return;
    setOpen(false);
    setSelectedSsid(null);
    setPassword("");
    setError(null);
  };

  const connectSelected = async () => {
    if (!selected) return;
    setConnecting(true);
    setError(null);
    try {
      const result = await connectWifi(selected.ssid, selected.secured ? password : null);
      setStatus({ available: true, connected: true, connected_ssid: result.connected_ssid, detail: "" });
      setNetworks((current) => current.map((network) => ({ ...network, connected: network.ssid === result.connected_ssid })));
      setSelectedSsid(null);
      setPassword("");
      setOpen(false);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Wi-Fi连接失败");
      await refreshStatus();
    } finally {
      setConnecting(false);
    }
  };

  const displayName = status?.connected_ssid ?? (status?.available === false ? "不可用" : "未连接");

  return <>
    <button type="button" className="wifi-sidebar-button" onClick={() => setOpen(true)} aria-label="配置Wi-Fi">
      <span>Wi-Fi</span><strong title={status?.connected_ssid ?? undefined}>{displayName}</strong><i>›</i>
    </button>
    {open && <div className="task-dialog-mask" role="dialog" aria-modal="true" aria-labelledby="wifi-dialog-title" onMouseDown={(event) => { if (event.target === event.currentTarget) close(); }}>
      <section className="task-dialog-panel wifi-dialog-panel">
        <header className="task-dialog-head"><div><h2 id="wifi-dialog-title">连接 Wi-Fi</h2><p>由设备端 NetworkManager 扫描并连接无线网络。切换网络后，当前浏览器连接可能暂时中断。</p></div><button type="button" disabled={connecting} onClick={close} aria-label="关闭Wi-Fi窗口">×</button></header>
        <div className="wifi-current"><span>当前连接</span><strong>{displayName}</strong></div>
        {status?.available === false && <div className="dev-message dev-message--error"><strong>Wi-Fi管理不可用</strong><span>{status.detail}</span></div>}
        {error && <div className="dev-message dev-message--error"><strong>{error}</strong></div>}
        <div className="wifi-dialog-toolbar"><strong>可用网络</strong><button type="button" className="button button--quiet" disabled={loading || connecting} onClick={() => void scan()}>{loading ? "正在扫描" : "重新扫描"}</button></div>
        <div className="wifi-network-list">
          {!loading && networks.length === 0 && <div className="wifi-network-empty">未发现可用 Wi-Fi</div>}
          {networks.map((network) => <button type="button" key={network.ssid} className={`${selectedSsid === network.ssid ? "active" : ""}${network.connected ? " connected" : ""}`} disabled={connecting} onClick={() => { setSelectedSsid(network.ssid); setPassword(""); setError(null); }}>
            <div><strong>{network.ssid}</strong><span>{network.secured ? "需要密码" : "开放网络"}{network.connected ? " · 已连接" : ""}</span></div><small>{network.signal}%</small>
          </button>)}
        </div>
        {selected && <div className="wifi-connect-form"><div><span>所选网络</span><strong>{selected.ssid}</strong></div>{selected.secured && <label><span>密码</span><input type="password" autoComplete="new-password" value={password} disabled={connecting} onChange={(event) => setPassword(event.target.value)} placeholder="请输入 Wi-Fi 密码" /></label>}</div>}
        <footer className="task-dialog-actions"><button type="button" className="button" disabled={connecting} onClick={close}>取消</button><button type="button" className="button button--primary" disabled={!selected || connecting || Boolean(selected?.secured && !password)} onClick={() => void connectSelected()}>{connecting ? "正在连接" : "连接"}</button></footer>
      </section>
    </div>}
  </>;
}
