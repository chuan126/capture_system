"use client";

import { useCallback, useEffect, useRef, useState } from "react";

import type { RtkSnapshot } from "@/components/rtk/rtkProtocol";

/* ------------------------------------------------------------------ */
/*  WGS-84 → GCJ-02 坐标转换（高德地图标准算法）                        */
/* ------------------------------------------------------------------ */

const PI = Math.PI;
const X_PI = (PI * 3000.0) / 180.0;
const A = 6378245.0; // 长半轴
const EE = 0.00669342162296594323; // 扁率

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
  if (isOutOfChina(lat, lon)) {
    return [lon, lat];
  }
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

/* ------------------------------------------------------------------ */
/*  常量                                                               */
/* ------------------------------------------------------------------ */

const MAX_TRACK_POINTS = 2500;
const AMAP_JS_URL = "https://webapi.amap.com/maps?v=2.0";
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

/* ------------------------------------------------------------------ */
/*  加载高德 JS API                                                    */
/* ------------------------------------------------------------------ */

function loadAmapScript(key: string, securityCode: string): Promise<void> {
  return new Promise((resolve, reject) => {
    // 如果已经挂载但 key 变化则先卸载
    const existing = document.querySelector(
      'script[src*="webapi.amap.com"]',
    ) as HTMLScriptElement | null;
    if (existing) {
      existing.remove();
      // 同时清理 AMap 全局变量
      delete (window as unknown as Record<string, unknown>)._AMapSecurityConfig;
      delete (window as unknown as Record<string, unknown>).AMap;
    }

    // 安全密钥必须在脚本加载前设置
    (window as unknown as Record<string, unknown>)._AMapSecurityConfig = {
      securityJsCode: securityCode,
    };

    const script = document.createElement("script");
    script.src = `${AMAP_JS_URL}&key=${key}`;
    script.async = true;
    const timeoutId = setTimeout(() => {
      script.remove();
      reject(new Error(`高德地图JS API加载超时（15秒），请检查网络或 Key`));
    }, 15000);
    script.onload = () => {
      clearTimeout(timeoutId);
      resolve();
    };
    script.onerror = () => {
      clearTimeout(timeoutId);
      reject(new Error("高德地图JS API加载失败，请检查网络连接和 Key 是否正确"));
    };
    document.head.appendChild(script);
  });
}

/* ------------------------------------------------------------------ */
/*  车辆 SVG 标记                                                      */
/* ------------------------------------------------------------------ */

function VehicleSvg() {
  return (
    <svg
      viewBox="0 0 42 58"
      xmlns="http://www.w3.org/2000/svg"
      aria-hidden="true"
    >
      {/* 方向指示 */}
      <polygon
        className="amap-vehicle-marker__heading"
        points="21,0 12,18 30,18"
      />
      {/* 车身 */}
      <g className="amap-vehicle-marker__body">
        <rect x="8" y="18" width="26" height="38" rx="6" />
        <path d="M16 56 L16 44 Q16 39 21 38 L21 38 Q26 39 26 44 L26 56 Z" />
        <path d="M10 18 L10 24 L32 24 L32 18 Z" />
        {/* 车窗 */}
        <rect x="14" y="24" width="5" height="10" rx="2" />
        <rect x="23" y="24" width="5" height="10" rx="2" />
      </g>
    </svg>
  );
}

/* ------------------------------------------------------------------ */
/*  组件 Props                                                         */
/* ------------------------------------------------------------------ */

type RealtimeAmapProps = {
  snapshot: RtkSnapshot | null;
  hasFix: boolean;
  connectionDetail: string;
};

/* ------------------------------------------------------------------ */
/*  组件                                                               */
/* ------------------------------------------------------------------ */

export default function RealtimeAmap({
  snapshot,
  hasFix,
  connectionDetail,
}: RealtimeAmapProps) {
  /* ---- 配置状态 ---- */
  const [config, setConfig] = useState<AmapConfig>({ key: "", securityCode: "" });
  const [configHydrated, setConfigHydrated] = useState(false);
  const [showSettings, setShowSettings] = useState(false);
  const [draftKey, setDraftKey] = useState("");
  const [draftCode, setDraftCode] = useState("");

  // 客户端挂载后从 localStorage / env 读取 Key，避免 SSR hydration 错配
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
    setConfigHydrated(true);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  /* ---- 地图状态 ---- */
  const [mapState, setMapState] = useState<
    "no_key" | "loading" | "error" | "ready"
  >(config.key ? "loading" : "no_key");

  /* ---- refs ---- */
  const containerRef = useRef<HTMLDivElement>(null);
  const mapRef = useRef<unknown>(null);
  const markerRef = useRef<unknown>(null);
  const polylineRef = useRef<unknown>(null);
  const trackPointsRef = useRef<TrackPoint[]>([]);
  const lastPositionRef = useRef<[number, number] | null>(null);
  const configRef = useRef(config);
  configRef.current = config;

  /* ---- 初始化地图 ---- */
  const initMap = useCallback(async (key: string, code: string) => {
    if (!containerRef.current) return;
    setMapState("loading");
    try {
      await loadAmapScript(key, code);
      const AMap = (window as unknown as Record<string, unknown>).AMap as
        | Record<string, unknown>
        | undefined;
      if (!AMap || typeof (AMap as Record<string, unknown>).Map !== "function") {
        const detail = AMap
          ? "AMap 已挂载但缺少 Map 构造函数，请检查 Key 和安全密钥是否正确"
          : "window.AMap 未挂载，JS API 脚本可能未正确执行";
        throw new Error(detail);
      }

      // 销毁旧实例
      if (mapRef.current) {
        const old = mapRef.current as { destroy?: () => void };
        old.destroy?.();
      }

      const mapInstance = new (AMap.Map as new (...args: unknown[]) => unknown)(
        containerRef.current,
        {
          zoom: 15,
          center: [118.0894, 24.4798], // 默认厦门，有定位后会更新
          resizeEnable: true,
        },
      );
      mapRef.current = mapInstance;

      // 重置轨迹
      trackPointsRef.current = [];
      polylineRef.current = null;

      // 如果有上次有效位置，尝试恢复
      if (lastPositionRef.current) {
        const [lng, lat] = lastPositionRef.current;
        (mapInstance as { setCenter?: (c: [number, number]) => void }).setCenter?.([lng, lat]);
      }

      setMapState("ready");
    } catch (err) {
      console.error("高德地图初始化失败:", err);
      setMapState("error");
    }
  }, []);

  // 首次挂载且 config 水合完成后，如果有 key 就自动初始化
  useEffect(() => {
    if (configHydrated && config.key) {
      void initMap(config.key, config.securityCode);
    }
  }, [configHydrated, config.key, config.securityCode, initMap]);

  /* ---- 保存配置 ---- */
  const saveConfig = useCallback(() => {
    const key = draftKey.trim();
    const code = draftCode.trim();
    if (!key) return;
    localStorage.setItem(STORAGE_KEY_KEY, key);
    localStorage.setItem(STORAGE_KEY_CODE, code);
    setConfig({ key, securityCode: code });
    setShowSettings(false);
    void initMap(key, code);
  }, [draftKey, draftCode, initMap]);

  /* ---- RTK 快照更新 → 地图标记 & 轨迹 ---- */
  useEffect(() => {
    if (mapState !== "ready") return;
    const map = mapRef.current as
      | (Record<string, unknown> & { setCenter?: (c: [number, number]) => void })
      | null;
    const AMap = (window as unknown as Record<string, unknown>).AMap as
      | Record<string, unknown>
      | undefined;
    if (!map || !AMap) return;

    if (hasFix && snapshot?.latitude != null && snapshot?.longitude != null) {
      const gcj = wgs84ToGcj02(snapshot.latitude, snapshot.longitude);
      lastPositionRef.current = gcj;

      // 更新 / 创建 marker
      if (markerRef.current) {
        const m = markerRef.current as { setPosition?: (p: [number, number]) => void };
        m.setPosition?.(gcj);
      } else {
        // 创建车辆标记元素
        const el = document.createElement("div");
        el.className = "amap-vehicle-marker";
        el.innerHTML = `<svg viewBox="0 0 42 58" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
          <polygon class="amap-vehicle-marker__heading" points="21,0 12,18 30,18"/>
          <g class="amap-vehicle-marker__body">
            <rect x="8" y="18" width="26" height="38" rx="6"/>
            <path d="M16 56 L16 44 Q16 39 21 38 L21 38 Q26 39 26 44 L26 56 Z"/>
            <path d="M10 18 L10 24 L32 24 L32 18 Z"/>
            <rect x="14" y="24" width="5" height="10" rx="2"/>
            <rect x="23" y="24" width="5" height="10" rx="2"/>
          </g>
        </svg>`;

        const Marker = (AMap as Record<string, unknown>).Marker as
          | (new (...args: unknown[]) => unknown)
          | undefined;
        if (Marker) {
          const marker = new Marker({
            position: gcj,
            content: el,
            offset: [-21, -29], // 锚点居中偏下
            zIndex: 100,
          });
          const mapSetMap = (map as Record<string, unknown>).add as
            | ((o: unknown) => void)
            | undefined;
          mapSetMap?.(marker);
          markerRef.current = marker;
        }
      }

      // 更新 marker 朝向
      if (markerRef.current && snapshot.track_degrees != null) {
        const m = markerRef.current as { setAngle?: (a: number) => void };
        m.setAngle?.(snapshot.track_degrees);
      } else if (markerRef.current) {
        const m = markerRef.current as { setAngle?: (a: number) => void };
        m.setAngle?.(0);
      }

      // 更新轨迹
      const points = trackPointsRef.current;
      if (
        points.length === 0 ||
        points[points.length - 1].lng !== gcj[0] ||
        points[points.length - 1].lat !== gcj[1]
      ) {
        points.push({ lng: gcj[0], lat: gcj[1] });
        if (points.length > MAX_TRACK_POINTS) {
          points.splice(0, points.length - MAX_TRACK_POINTS);
        }
      }

      if (polylineRef.current) {
        const pl = polylineRef.current as { setPath?: (p: [number, number][]) => void };
        pl.setPath?.(points.map((p) => [p.lng, p.lat] as [number, number]));
      } else if (points.length >= 2) {
        const Polyline = (AMap as Record<string, unknown>).Polyline as
          | (new (...args: unknown[]) => unknown)
          | undefined;
        if (Polyline) {
          const pl = new Polyline({
            path: points.map((p) => [p.lng, p.lat]),
            strokeColor: "#1769ee",
            strokeWeight: 3,
            strokeOpacity: 0.7,
            lineJoin: "round",
            zIndex: 50,
          });
          const mapAdd = (map as Record<string, unknown>).add as
            | ((o: unknown) => void)
            | undefined;
          mapAdd?.(pl);
          polylineRef.current = pl;
        }
      }

      // 地图跟随
      map.setCenter?.(gcj);
    }
    // hasFix 为 false 时保持最后位置，不增加轨迹点
  }, [snapshot, hasFix, mapState]);

  /* ---- RTK 状态文字 ---- */
  const fixText = hasFix ? "RTK 定位有效" : "RTK 无定位";
  const coordinateText =
    hasFix && snapshot?.latitude != null && snapshot?.longitude != null
      ? `${snapshot.latitude.toFixed(8)}°, ${snapshot.longitude.toFixed(8)}°`
      : "--";

  /* ---- 渲染 ---- */
  return (
    <>
      <article className="panel dashboard-map-panel">
        {/* 头部 */}
        <div className="map-panel-head">
          <div>
            <h2>实时地图</h2>
            <p>高德地图 · 车辆位置与轨迹</p>
          </div>
          <button
            type="button"
            className="map-settings-button"
            onClick={() => {
              setDraftKey(config.key);
              setDraftCode(config.securityCode);
              setShowSettings(true);
            }}
          >
            地图设置
          </button>
        </div>

        {/* 地图区域 */}
        <div className="amap-stage">
          {/* 未配置 Key */}
          {mapState === "no_key" && (
            <div className="amap-empty">
              <span>⌂</span>
              <strong>高德地图未配置</strong>
              <small>
                点击右上角"地图设置"填写高德Web端JS API
                Key和安全密钥后开始使用。也可构建时通过环境变量提供初始值。
              </small>
              <button
                type="button"
                onClick={() => {
                  setDraftKey(config.key);
                  setDraftCode(config.securityCode);
                  setShowSettings(true);
                }}
              >
                立即配置
              </button>
            </div>
          )}

          {/* 加载中 */}
          {mapState === "loading" && (
            <div className="amap-empty">
              <span>⌛</span>
              <strong>地图加载中</strong>
              <small>正在连接高德地图服务…</small>
            </div>
          )}

          {/* 加载失败 */}
          {mapState === "error" && (
            <div className="amap-empty">
              <span>⚠</span>
              <strong>地图加载失败</strong>
              <small>请检查Key和安全密钥是否正确，或网络连接是否正常。</small>
              <button
                type="button"
                onClick={() => {
                  setDraftKey(config.key);
                  setDraftCode(config.securityCode);
                  setShowSettings(true);
                }}
              >
                重新配置
              </button>
            </div>
          )}

          {/* AMap 容器（始终渲染，ready 时填充） */}
          <div ref={containerRef} className="amap-container" />

          {/* 状态标记（地图就绪后叠加） */}
          {mapState === "ready" && (
            <>
              <div className="amap-status-row">
                <span>
                  <i>●</i>
                  <strong>{fixText}</strong>
                </span>
                <span>
                  <i>⏤</i>
                  {trackPointsRef.current.length} 轨迹点
                </span>
              </div>
              <div className="amap-coordinate">
                <span>WGS84</span>
                <small>{coordinateText}</small>
                <span>{connectionDetail}</span>
              </div>
            </>
          )}
        </div>
      </article>

      {/* 设置弹窗 */}
      {showSettings && (
        <div
          className="map-modal-mask"
          onClick={(e) => {
            if (e.target === e.currentTarget) setShowSettings(false);
          }}
        >
          <div className="map-modal-panel">
            <div className="map-modal-head">
              <div>
                <h2>地图设置</h2>
                <p>
                  配置高德Web端JS API
                  Key，Key和安全密钥可在高德开放平台控制台获取。
                </p>
              </div>
              <button type="button" onClick={() => setShowSettings(false)}>
                ×
              </button>
            </div>

            <label>
              <span>JS API Key（Web端）</span>
              <input
                type="text"
                value={draftKey}
                onChange={(e) => setDraftKey(e.target.value)}
                placeholder="输入高德Web端Key"
                autoFocus
              />
            </label>

            <label>
              <span>安全密钥（securityJsCode）</span>
              <input
                type="text"
                value={draftCode}
                onChange={(e) => setDraftCode(e.target.value)}
                placeholder="输入securityJsCode（可选）"
              />
            </label>

            <div className="map-security-note">
              正式部署应在高德控制台限制允许访问的域名。更换Key或安全密钥后，页面会销毁旧地图实例并重新加载JS
              API。
            </div>

            <div className="map-modal-actions">
              <button
                type="button"
                className="button button--soft"
                onClick={() => setShowSettings(false)}
              >
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
