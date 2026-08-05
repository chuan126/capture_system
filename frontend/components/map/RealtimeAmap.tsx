"use client";

import { useCallback, useEffect, useRef, useState } from "react";

import type { RtkSnapshot } from "@/components/rtk/rtkProtocol";

const PI = Math.PI;
const A = 6378245.0;
const EE = 0.00669342162296594323;

function isOutOfChina(lat: number, lon: number): boolean {
  return lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271;
}

function transformLat(x: number, y: number): number {
  let ret =
    -100.0 +
    2.0 * x +
    3.0 * y +
    0.2 * y * y +
    0.1 * x * y +
    0.2 * Math.sqrt(Math.abs(x));
  ret +=
    ((20.0 * Math.sin(6.0 * x * PI) + 20.0 * Math.sin(2.0 * x * PI)) * 2.0) /
    3.0;
  ret +=
    ((20.0 * Math.sin(y * PI) + 40.0 * Math.sin((y / 3.0) * PI)) * 2.0) / 3.0;
  ret +=
    ((160.0 * Math.sin((y / 12.0) * PI) + 320 * Math.sin((y * PI) / 30.0)) *
      2.0) /
    3.0;
  return ret;
}

function transformLon(x: number, y: number): number {
  let ret =
    300.0 +
    x +
    2.0 * y +
    0.1 * x * x +
    0.1 * x * y +
    0.1 * Math.sqrt(Math.abs(x));
  ret +=
    ((20.0 * Math.sin(6.0 * x * PI) + 20.0 * Math.sin(2.0 * x * PI)) * 2.0) /
    3.0;
  ret +=
    ((20.0 * Math.sin(x * PI) + 40.0 * Math.sin((x / 3.0) * PI)) * 2.0) / 3.0;
  ret +=
    ((150.0 * Math.sin((x / 12.0) * PI) + 300.0 * Math.sin((x / 30.0) * PI)) *
      2.0) /
    3.0;
  return ret;
}

function wgs84ToGcj02(lat: number, lon: number): [number, number] {
  if (isOutOfChina(lat, lon)) return [lon, lat];

  let dLat = transformLat(lon - 105.0, lat - 35.0);
  let dLon = transformLon(lon - 105.0, lat - 35.0);
  const radLat = (lat / 180.0) * PI;
  let magic = Math.sin(radLat);
  magic = 1 - EE * magic * magic;
  const sqrtMagic = Math.sqrt(magic);
  dLat = (dLat * 180.0) / (((A * (1 - EE)) / (magic * sqrtMagic)) * PI);
  dLon = (dLon * 180.0) / ((A / sqrtMagic) * Math.cos(radLat) * PI);
  return [lon + dLon, lat + dLat];
}

const MAX_TRACK_POINTS = 2500;
const AMAP_JS_URL =
  "https://webapi.amap.com/maps?v=2.0&plugin=AMap.Scale,AMap.ToolBar";
const STORAGE_KEY_KEY = "amap_js_key";
const STORAGE_KEY_CODE = "amap_security_code";

type AmapConfig = {
  key: string;
  securityCode: string;
};

type TrackPoint = {
  lng: number;
  lat: number;
};

type AmapMapLike = {
  add?: (overlay: unknown) => void;
  addControl?: (control: unknown) => void;
  destroy?: () => void;
  resize?: () => void;
  setCenter?: (center: [number, number]) => void;
  setZoomAndCenter?: (zoom: number, center: [number, number]) => void;
};

function loadAmapScript(key: string, securityCode: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const existing = document.querySelector(
      'script[src*="webapi.amap.com"]',
    ) as HTMLScriptElement | null;

    if (existing) {
      existing.remove();
      delete (window as unknown as Record<string, unknown>)._AMapSecurityConfig;
      delete (window as unknown as Record<string, unknown>).AMap;
    }

    (window as unknown as Record<string, unknown>)._AMapSecurityConfig = {
      securityJsCode: securityCode,
    };

    const script = document.createElement("script");
    script.src = `${AMAP_JS_URL}&key=${encodeURIComponent(key)}`;
    script.async = true;

    const timeoutId = window.setTimeout(() => {
      script.remove();
      reject(new Error("高德地图 JS API 加载超时，请检查网络、Key 和域名白名单"));
    }, 15000);

    script.onload = () => {
      window.clearTimeout(timeoutId);
      resolve();
    };
    script.onerror = () => {
      window.clearTimeout(timeoutId);
      reject(new Error("高德地图 JS API 加载失败，请检查网络和 Key"));
    };

    document.head.appendChild(script);
  });
}

function vehicleMarkerMarkup(): string {
  return `
    <svg viewBox="0 0 48 66" aria-label="检测车辆">
      <defs>
        <linearGradient id="captureCarBodyGradient" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#4b9cff"/>
          <stop offset="58%" stop-color="#176bff"/>
          <stop offset="100%" stop-color="#0d47b8"/>
        </linearGradient>
        <linearGradient id="captureCarGlassGradient" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#e8f4ff"/>
          <stop offset="100%" stop-color="#9ac7ef"/>
        </linearGradient>
        <filter id="captureCarShadow" x="-40%" y="-30%" width="180%" height="190%">
          <feDropShadow dx="0" dy="3" stdDeviation="2.4" flood-color="#173667" flood-opacity=".34"/>
        </filter>
      </defs>
      <g class="vehicle-direction">
        <path d="M24 1 L30 11 H18 Z" fill="#176bff" opacity=".72"/>
        <circle cx="24" cy="9" r="8" fill="none" stroke="#176bff" stroke-width="1.5" opacity=".22"/>
      </g>
      <g filter="url(#captureCarShadow)">
        <rect x="6" y="17" width="36" height="45" rx="13" fill="#ffffff"/>
        <rect x="8" y="18" width="32" height="42" rx="11" fill="url(#captureCarBodyGradient)"/>
        <path d="M14 27 Q16 21 21 20 H27 Q32 21 34 27 L36 39 H12 Z"
              fill="url(#captureCarGlassGradient)" stroke="#ffffff" stroke-width="1.2"/>
        <path d="M14 40 H34 L32 50 Q30 56 24 57 Q18 56 16 50 Z"
              fill="#dcecff" opacity=".93"/>
        <rect x="5" y="27" width="4" height="11" rx="2" fill="#24334a"/>
        <rect x="39" y="27" width="4" height="11" rx="2" fill="#24334a"/>
        <rect x="5" y="45" width="4" height="10" rx="2" fill="#24334a"/>
        <rect x="39" y="45" width="4" height="10" rx="2" fill="#24334a"/>
        <ellipse cx="15" cy="22" rx="4" ry="2" fill="#fff4b8"/>
        <ellipse cx="33" cy="22" rx="4" ry="2" fill="#fff4b8"/>
        <ellipse cx="15" cy="56" rx="4" ry="2" fill="#ff9cac"/>
        <ellipse cx="33" cy="56" rx="4" ry="2" fill="#ff9cac"/>
        <rect x="18" y="13" width="12" height="7" rx="3.5" fill="#0f9f6e" stroke="#fff" stroke-width="1.3"/>
        <circle cx="24" cy="16.5" r="2" fill="#bff7df"/>
      </g>
    </svg>`;
}

type RealtimeAmapProps = {
  snapshot: RtkSnapshot | null;
  hasFix: boolean;
  connectionDetail: string;
  laneLabel?: string;
  expanded?: boolean;
  onToggleExpanded?: () => void;
};

function MapExpandButton({
  expanded,
  onClick,
}: {
  expanded: boolean;
  onClick: () => void;
}) {
  return (
    <button
      type="button"
      className="panel-expand-button"
      aria-label={expanded ? "退出实时地图放大显示" : "放大显示实时地图"}
      aria-pressed={expanded}
      title={expanded ? "退出放大显示" : "放大显示"}
      onClick={onClick}
    >
      {expanded ? (
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M9 4v5H4M15 4v5h5M9 20v-5H4M15 20v-5h5" />
        </svg>
      ) : (
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M9 4H4v5M15 4h5v5M9 20H4v-5M15 20h5v-5" />
        </svg>
      )}
    </button>
  );
}

export default function RealtimeAmap({
  snapshot,
  hasFix,
  connectionDetail,
  laneLabel = "待任务接入",
  expanded = false,
  onToggleExpanded = () => undefined,
}: RealtimeAmapProps) {
  const [config, setConfig] = useState<AmapConfig>({ key: "", securityCode: "" });
  const [configHydrated, setConfigHydrated] = useState(false);
  const [showSettings, setShowSettings] = useState(false);
  const [draftKey, setDraftKey] = useState("");
  const [draftCode, setDraftCode] = useState("");
  const [mapState, setMapState] = useState<
    "no_key" | "loading" | "error" | "ready"
  >("no_key");
  const [trackPointCount, setTrackPointCount] = useState(0);

  const containerRef = useRef<HTMLDivElement | null>(null);
  const mapRef = useRef<AmapMapLike | null>(null);
  const markerRef = useRef<unknown>(null);
  const polylineRef = useRef<unknown>(null);
  const trackPointsRef = useRef<TrackPoint[]>([]);
  const lastPositionRef = useRef<[number, number] | null>(null);

  useEffect(() => {
    if (typeof window === "undefined") return;

    const storedKey =
      localStorage.getItem(STORAGE_KEY_KEY) ??
      process.env.NEXT_PUBLIC_AMAP_KEY ??
      "";
    const storedCode =
      localStorage.getItem(STORAGE_KEY_CODE) ??
      process.env.NEXT_PUBLIC_AMAP_SECURITY_CODE ??
      "";

    setConfig({ key: storedKey, securityCode: storedCode });
    setDraftKey(storedKey);
    setDraftCode(storedCode);
    setMapState(storedKey ? "loading" : "no_key");
    setConfigHydrated(true);
  }, []);

  const initMap = useCallback(async (key: string, code: string) => {
    if (!containerRef.current) return;

    setMapState("loading");
    try {
      await loadAmapScript(key, code);
      const AMap = (window as unknown as Record<string, unknown>).AMap as
        | Record<string, unknown>
        | undefined;
      const MapConstructor = AMap?.Map as (new (...args: unknown[]) => AmapMapLike) | undefined;

      if (!AMap || !MapConstructor) {
        throw new Error("AMap.Map 不可用，请检查 Key、安全密钥和域名白名单");
      }

      mapRef.current?.destroy?.();
      markerRef.current = null;
      polylineRef.current = null;
      trackPointsRef.current = [];
      setTrackPointCount(0);

      const mapInstance = new MapConstructor(containerRef.current, {
        viewMode: "2D",
        zoom: 13,
        center: [118.0829, 24.5008],
        mapStyle: "amap://styles/whitesmoke",
        resizeEnable: true,
      });
      mapRef.current = mapInstance;

      const Scale = AMap.Scale as (new (...args: unknown[]) => unknown) | undefined;
      const ToolBar = AMap.ToolBar as (new (...args: unknown[]) => unknown) | undefined;
      if (Scale) mapInstance.addControl?.(new Scale());
      if (ToolBar) {
        mapInstance.addControl?.(
          new ToolBar({ position: { top: "72px", right: "12px" } }),
        );
      }

      if (lastPositionRef.current) {
        mapInstance.setZoomAndCenter?.(17, lastPositionRef.current);
      }

      setMapState("ready");
      window.setTimeout(() => mapInstance.resize?.(), 0);
    } catch (error) {
      console.error("高德地图初始化失败", error);
      setMapState("error");
    }
  }, []);

  useEffect(() => {
    if (configHydrated && config.key) {
      void initMap(config.key, config.securityCode);
    }
  }, [configHydrated, config.key, config.securityCode, initMap]);

  const openSettings = useCallback(() => {
    setDraftKey(config.key);
    setDraftCode(config.securityCode);
    setShowSettings(true);
  }, [config.key, config.securityCode]);

  const saveConfig = useCallback(() => {
    const key = draftKey.trim();
    const securityCode = draftCode.trim();
    if (!key) return;

    localStorage.setItem(STORAGE_KEY_KEY, key);
    localStorage.setItem(STORAGE_KEY_CODE, securityCode);
    setConfig({ key, securityCode });
    setShowSettings(false);
    void initMap(key, securityCode);
  }, [draftCode, draftKey, initMap]);

  useEffect(() => {
    if (mapState !== "ready") return;

    const map = mapRef.current;
    const AMap = (window as unknown as Record<string, unknown>).AMap as
      | Record<string, unknown>
      | undefined;
    if (!map || !AMap) return;

    if (
      !hasFix ||
      snapshot?.latitude === null ||
      snapshot?.latitude === undefined ||
      snapshot.longitude === null ||
      snapshot.longitude === undefined
    ) {
      return;
    }

    const gcj = wgs84ToGcj02(snapshot.latitude, snapshot.longitude);
    lastPositionRef.current = gcj;

    if (markerRef.current) {
      const marker = markerRef.current as {
        setAngle?: (angle: number) => void;
        setPosition?: (position: [number, number]) => void;
      };
      marker.setPosition?.(gcj);
      marker.setAngle?.(snapshot.track_degrees ?? 0);
    } else {
      const markerElement = document.createElement("div");
      markerElement.className = "amap-vehicle-marker";
      markerElement.innerHTML = vehicleMarkerMarkup();

      const Marker = AMap.Marker as (new (...args: unknown[]) => unknown) | undefined;
      if (Marker) {
        const marker = new Marker({
          position: gcj,
          content: markerElement,
          offset: [-24, -33],
          zIndex: 120,
        });
        map.add?.(marker);
        markerRef.current = marker;
        (marker as { setAngle?: (angle: number) => void }).setAngle?.(
          snapshot.track_degrees ?? 0,
        );
      }
    }

    const points = trackPointsRef.current;
    const last = points.at(-1);
    if (!last || last.lng !== gcj[0] || last.lat !== gcj[1]) {
      points.push({ lng: gcj[0], lat: gcj[1] });
      if (points.length > MAX_TRACK_POINTS) {
        points.splice(0, points.length - MAX_TRACK_POINTS);
      }
      setTrackPointCount(points.length);
    }

    const path = points.map((point) => [point.lng, point.lat] as [number, number]);
    if (polylineRef.current) {
      (polylineRef.current as { setPath?: (value: [number, number][]) => void })
        .setPath?.(path);
    } else if (path.length >= 2) {
      const Polyline = AMap.Polyline as (new (...args: unknown[]) => unknown) | undefined;
      if (Polyline) {
        const polyline = new Polyline({
          path,
          strokeColor: "#176bff",
          strokeWeight: 5,
          strokeOpacity: 0.88,
          lineJoin: "round",
          lineCap: "round",
          zIndex: 90,
        });
        map.add?.(polyline);
        polylineRef.current = polyline;
      }
    }

    if (points.length === 1) {
      map.setZoomAndCenter?.(17, gcj);
    } else {
      map.setCenter?.(gcj);
    }
  }, [hasFix, mapState, snapshot]);

  useEffect(() => {
    const resize = () => mapRef.current?.resize?.();
    window.addEventListener("resize", resize);
    return () => window.removeEventListener("resize", resize);
  }, []);

  useEffect(() => {
    const container = containerRef.current;
    if (!container || typeof ResizeObserver === "undefined") return;

    const observer = new ResizeObserver(() => mapRef.current?.resize?.());
    observer.observe(container);
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    if (mapState !== "ready") return;
    const frame = window.requestAnimationFrame(() => mapRef.current?.resize?.());
    const timer = window.setTimeout(() => mapRef.current?.resize?.(), 180);
    return () => {
      window.cancelAnimationFrame(frame);
      window.clearTimeout(timer);
    };
  }, [expanded, mapState]);

  const coordinateText =
    hasFix && snapshot?.latitude != null && snapshot.longitude != null
      ? `${snapshot.latitude.toFixed(8)}°, ${snapshot.longitude.toFixed(8)}°`
      : lastPositionRef.current
        ? "保留最后有效位置"
        : "等待有效 RTK 坐标";
  const positionModeText = hasFix ? "RTK 绝对定位" : "RTK 无效";
  const trackStateText =
    mapState !== "ready"
      ? "等待地图配置"
      : hasFix
        ? trackPointCount > 0
          ? `实时绘制 · ${trackPointCount} 点`
          : "等待 RTK"
        : "轨迹已暂停";
  const mapSubtitle = hasFix
    ? "仅在 RTK 有效时绘制绝对轨迹"
    : "保留最后有效位置，暂停轨迹更新";
  const mapTip = hasFix
    ? "RTK 原始 WGS84 坐标转换为 GCJ-02 后显示。定位失效时保留最后有效位置，并停止增加轨迹点。"
    : "当前 RTK 无效。地图保留最后有效位置和已有轨迹，不继续推算车辆绝对位置。";

  return (
    <>
      <article className={`panel dashboard-map-panel${expanded ? " visual-panel--expanded" : ""}`}>
        <div className="map-panel-head">
          <div>
            <h2>RTK 实时地图</h2>
            <p>{mapSubtitle}</p>
          </div>
          <MapExpandButton expanded={expanded} onClick={onToggleExpanded} />
        </div>

        <div className="amap-stage">
          <div ref={containerRef} className="amap-container" />

          {mapState === "no_key" && (
            <div className="amap-empty">
              <div>
                <strong>高德地图尚未配置</strong>
                <span>
                  填写 Web 端 JS API Key 和 securityJsCode 后显示车辆位置与绝对轨迹。
                </span>
                <button type="button" onClick={openSettings}>地图设置</button>
              </div>
            </div>
          )}

          {mapState === "loading" && (
            <div className="amap-empty">
              <div>
                <strong>高德地图正在加载</strong>
                <span>正在连接地图服务并初始化实时轨迹图层。</span>
              </div>
            </div>
          )}

          {mapState === "error" && (
            <div className="amap-empty">
              <div>
                <strong>高德地图加载失败</strong>
                <span>请检查网络、Key、securityJsCode 和域名白名单。</span>
                <button type="button" onClick={openSettings}>重新配置</button>
              </div>
            </div>
          )}

          <div className="amap-status-row">
            <span className="amap-chip">
              定位模式 <strong>{positionModeText}</strong>
            </span>
            <span className="amap-chip">
              作业车道 <strong>{laneLabel}</strong>
            </span>
            <span className="amap-chip">
              轨迹 <strong>{trackStateText}</strong>
            </span>
            <button type="button" className="amap-chip amap-chip--button" onClick={openSettings}>
              地图设置
            </button>
          </div>

          <div className="amap-map-tip">
            <strong>WGS84 · {coordinateText}</strong>
            <span>{mapTip}</span>
            <small>{connectionDetail}</small>
          </div>
        </div>
      </article>

      {showSettings && (
        <div
          className="map-modal-mask"
          role="dialog"
          aria-modal="true"
          aria-labelledby="map-settings-title"
          onClick={(event) => {
            if (event.target === event.currentTarget) setShowSettings(false);
          }}
        >
          <div className="map-modal-panel">
            <div className="map-modal-head">
              <div>
                <h2 id="map-settings-title">地图设置</h2>
                <p>配置高德 Web 端 JS API Key 和 securityJsCode。</p>
              </div>
              <button type="button" aria-label="关闭地图设置" onClick={() => setShowSettings(false)}>
                ×
              </button>
            </div>

            <label>
              <span>JS API Key（Web 端）</span>
              <input
                type="text"
                value={draftKey}
                onChange={(event) => setDraftKey(event.target.value)}
                placeholder="输入高德 Web 端 Key"
                autoFocus
              />
            </label>

            <label>
              <span>安全密钥（securityJsCode）</span>
              <input
                type="text"
                value={draftCode}
                onChange={(event) => setDraftCode(event.target.value)}
                placeholder="输入 securityJsCode"
              />
            </label>

            <div className="map-security-note">
              正式部署应在高德控制台限制允许访问的域名，并通过部署环境提供初始配置。浏览器本地设置仅用于设备端调试。
            </div>

            <div className="map-modal-actions">
              <button type="button" className="button button--soft" onClick={() => setShowSettings(false)}>
                取消
              </button>
              <button
                type="button"
                className="button button--primary"
                disabled={!draftKey.trim()}
                onClick={saveConfig}
              >
                保存并加载地图
              </button>
            </div>
          </div>
        </div>
      )}
    </>
  );
}
